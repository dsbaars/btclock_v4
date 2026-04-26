// Private helpers shared across screen renderers.
//
// Not part of the public screens API — only the per-screen .cpp files
// include this. Keep it header-only where it matters (tiny templates /
// constants) and push non-template impls into common.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// Under the emscripten/WASM preview build (tools/wasm/build.sh) the
// real epd_ssd1680.hpp can't be pulled in — it pulls driver/gpio.h,
// driver/spi_master.h, mcp23017.hpp. We substitute a minimal EpdPanel
// shim (wasm_panel.hpp) that exposes just the Width/Height/kStride
// surface and stubs out DrawFramebufferStart/WaitForRefresh. font.hpp
// itself is pure (cstdint/cstddef only) so it's the same in both
// builds — it carries LandscapeFb/Rotation + the paint-primitive decls
// that every renderer uses.
#ifdef BTCLOCK_WASM_BUILD
#include "font.hpp"
#include "wasm_panel.hpp"
#else
#include "epd_ssd1680.hpp"
#include "font.hpp"
#endif

#include "fonts_app.hpp"
#include "screens/screen_math.hpp"

namespace btclock {

// Every digit call site in the screen renderers passes this. Don't widen
// it with punctuation or anything else with a deep descender: the ref
// box's `below_baseline` directly lowers the computed glyph baseline,
// and mixing wider and narrower refs across screens produces visibly
// inconsistent vertical positions (see test_host/test_screen_ref_chars
// for the regression that motivated pinning this).
inline constexpr const char* kDigitRef = "0123456789";

// Build a LandscapeFb view over panel `i`'s framebuffer. Templated on N
// so the array-of-arrays type propagates naturally; there's no allocation.
// Compiles against either the real EpdPanel (components/epd_ssd1680) or
// the WASM shim (tools/wasm/wasm_panel.hpp) — both expose the same
// kStride/Width/Height surface.
template <size_t N>
LandscapeFb PrepFb(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                   uint8_t (&fb_storage)[N][16 * 296], size_t i) {
  LandscapeFb lfb = {};
  lfb.native_fb = fb_storage[i];
  lfb.native_stride = panels[i]->kStride;
  lfb.native_width = panels[i]->Width();
  lfb.native_height = panels[i]->Height();
  lfb.rotation = Rotation::k180;
  return lfb;
}

// Right-justify the decimal form of `h` into `digits[slots]`; leading
// positions get ' ' as blanks. Leading digits are truncated if `h`
// exceeds `slots` — the next block-height decade rollover is years out.
void FormatDigits(uint32_t h, char* digits, size_t slots);

// 1e8 / price_usd, rounded, clamped to [0, 4e9). Returns -1 on parse
// failure or out-of-range.
int32_t SatsPerUnit(const std::string& price_str);

// Integer part of price, rounded half-up. Returns -1 on parse failure
// or if the value exceeds the 6-digit display range (> 2e9).
int32_t PriceInt(const std::string& price_str);

struct DigitLayout {
  std::array<char, 6> digits{' ', ' ', ' ', ' ', ' ', ' '};
  std::array<bool, 6> is_sats{};
};

// Lay out up to 6 digits from `sats` with an optional sats-glyph prefix
// placed one slot before the first digit. Returns all-blank on `sats<0`.
// On overflow (> 6 digits), leading digits are truncated and no symbol.
// Divergence vs. old firmware: old parseSatsPerCurrency drops the
// SATS/MSCW label on 7-digit sats (price < ~$100) and fills all 7
// panels. Not fixed here — tracked in btclock_v3_fci-f7y.
DigitLayout ComputeMoscowLayout(int32_t sats, bool use_symbol);

// Templated variant used by the V8 8-panel layout (Bug 3). The 6-slot
// fixed `ComputeMoscowLayout` left panel 7 blank; this helper adapts to
// an arbitrary digit-slot count so the renderer can fill all N-1 slots.
// Returns all-blank on `sats<0`. On overflow (> Slots digits), leading
// digits are truncated and no sats symbol is emitted.
template <size_t Slots>
struct MoscowLayoutN {
  std::array<char, Slots> digits{};
  std::array<bool, Slots> is_sats{};
  MoscowLayoutN() {
    for (size_t i = 0; i < Slots; ++i) {
      digits[i] = ' ';
      is_sats[i] = false;
    }
  }
};

template <size_t Slots>
inline MoscowLayoutN<Slots> ComputeMoscowLayoutN(int32_t sats,
                                                 bool use_symbol) {
  MoscowLayoutN<Slots> l;
  if (sats < 0) return l;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(sats));
  const size_t len = std::strlen(buf);
  if (len >= Slots) {
    const size_t start = len - Slots;
    for (size_t i = 0; i < Slots; ++i) l.digits[i] = buf[start + i];
    return l;
  }
  const size_t pad = Slots - len;
  for (size_t i = 0; i < Slots; ++i) {
    l.digits[i] = (i < pad) ? ' ' : buf[i - pad];
  }
  if (use_symbol && pad > 0) {
    l.is_sats[pad - 1] = true;
    l.digits[pad - 1] = ' ';
  }
  return l;
}

// UTF-8 currency symbol for the given ISO code, or "" if no glyph is
// available yet (the Antonio subset covers $, £, ¥, €; see
// components/fonts/assets/README.md for the codepoint list). Empty
// means the price screen won't paint a symbol panel — just the label
// and digits. Callers treat empty the same as "no symbol requested".
const char* CurrencySymbolUtf8(const std::string& ccy);

// Pure halving / supply / clock math moved to screen_math.hpp so host
// tests can include it without pulling the ESP-IDF-dependent font and
// EPD headers above. See screen_math.hpp for the helpers themselves.

// -----------------------------------------------------------------------------
// DRY paint skeleton shared across data screens.
//
// Every data-screen renderer implements the same mechanical pipeline:
//   1. decide whether each panel needs repainting (vs prev snapshot),
//   2. ClearFb + draw the panel's content,
//   3. DrawFramebufferStart(full? kFull : kPartial) on each dirty panel,
//   4. WaitForRefresh on each dirty panel.
//
// Step 1 is per-screen logic (BTC price diff vs block-height digit diff
// vs pool-earnings tuple diff — too shape-specific to DRY). Steps 2-4
// are mechanically identical and expensive to get wrong (a missed
// WaitForRefresh corrupts the next frame; an unclear full/partial
// dispatch leaves stale ink). `PaintDataScreen` pulls steps 2-4 out
// into one place so each screen only owns its diff + slot layout.
//
// Layout: the caller fills an `std::array<PaintSlot, N>` with one
// `PaintSlot` per panel describing WHAT that panel should show; the
// parallel `std::array<bool, N> update` mask says WHICH panels need
// paint this frame. Panels where `update[i] == false` are skipped
// (no clear, no refresh). Panels where `update[i] == true` are
// cleared to white and refreshed regardless of slot kind, so a cell
// transitioning from a digit to `kBlank` gets visibly blanked on
// partial refresh too. The diff layer is the single source of truth
// for "this panel needs work this frame"; `update[i]` is authoritative.
//
// The full-vs-partial decision lives in the caller — `PaintDataScreen`
// takes `full_refresh` as input and applies it uniformly to every
// updated panel. Mixed full/partial across panels in a single frame
// is not supported (the EPD driver's activation sequence cares).
//
// Pixel heights and ref-char sets for label / digit / unit roles are
// fixed per-kind below to match the historical baselines; if a screen
// needs a different metric, add a new PaintSlot::Kind rather than
// widening the existing ones.

struct PaintSlot {
  enum Kind {
    // Panel left blank. PaintDataScreen clears the framebuffer to
    // white before painting, so a kBlank slot results in a blank panel
    // on both full and partial refresh whenever `update[i]==true`.
    kBlank,
    // "TOP/BOTTOM" split-text label (two lines centred top-half /
    // bottom-half). `text` holds "TOP/BOTTOM"; the first '/' separates
    // the halves. Rendered at the label font role and the 54pt size
    // the screens have used historically.
    kLabelSplit,
    // Single-line label at the label font role (split point-size).
    // Ref chars: uppercase+digits so label punctuation doesn't float.
    kLabel,
    // Single-character digit cell at the digit font role, 180pt.
    // Ref chars: `kDigitRef` so baselines line up across digit panels.
    // `text` contains the glyph — typically one char but multi-byte
    // UTF-8 is fine (currency symbols round-trip as 2-3 byte strings).
    kDigit,
    // 3-digit small-chars group at the small_chars font role, 90pt.
    // Used by the bitcoin_supply / market_cap small-chars layouts.
    kSmallGroup,
    // Sats-symbol glyph at the sats_glyph font role (130pt). `text`
    // holds the UTF-8 sats glyph string (4 bytes incl. NUL).
    kSatsGlyph,
    // Currency symbol cell rendered at the digit font role + 180pt
    // (so it lines up with the digit baseline). `text` is a UTF-8
    // currency glyph ("$", "€", "¥", etc.).
    kCurrencyGlyph,
    // "TOP/BOTTOM" split at the unit font role (smaller than label;
    // matches fee-rate's "sat/vB" trailing panel).
    kUnitSplit,
    // Raw 1-bpp bitmap passthrough. `bitmap` + `bmp_w` + `bmp_h`.
    // `text` is ignored. Used by mining-pool logos / bitaxe icon.
    kIconBitmap,
    // MDI glyph painted from the icon font role at the digit pixel
    // height. `mdi_codepoint` carries the raw codepoint from
    // `mdi_codepoints.hpp`; `text` is ignored. Goes through
    // DrawCodepointCentered (font.hpp) so the bitmap bbox drives the
    // baseline — kDigitRef would compute a zero ref box for codepoints
    // in the Private Use Area. Default size matches kSatsGlyph (130 px)
    // so MDI glyphs visually weight-match the sats glyph.
    kMdiIcon,
  } kind = kBlank;

  // For split kinds (`kLabelSplit` / `kUnitSplit`), "TOP/BOTTOM"
  // with the first '/' as the separator. For single-line kinds,
  // the literal text to paint. Empty string paints nothing.
  std::string text;

  // For `kIconBitmap` only.
  const uint8_t* bitmap = nullptr;
  int bmp_w = 0;
  int bmp_h = 0;

  // Optional ref-chars override. When non-null, replaces the default
  // ref-chars set that each kind ships with. Used by screens like
  // fee_rate whose unit panel ("sat/vB") historically renders against
  // a focused ref set that excludes descender-carrying glyphs — the
  // default label ref (A..Z + digits) includes 'Q' which shifts the
  // bottom-text baseline downward vs. the pre-refactor output. Default
  // nullptr keeps every other call site on the kind's shipped ref so
  // labels stay visually aligned across screens.
  const char* ref_override = nullptr;

  // Optional pixel-height override. When > 0, replaces the default
  // pixel height that each kind ships with. Used by the bitaxe screens
  // whose OFFLINE banner and best-diff tail historically render at a
  // slightly smaller digit size (160 px instead of the digit role's
  // 180 px) so single-letter suffixes like "M" / "G" don't clip the
  // panel edge at Antonio's metrics. Default 0 keeps every other call
  // site on the kind's shipped pixel height.
  float pixel_height_override = 0.0f;

  // For `kMdiIcon` only — raw MDI codepoint (e.g.
  // `mdi::kIconLightningBolt`). 0 is a no-op (panel stays cleared).
  // Field order kept after `pixel_height_override` so the historical
  // brace-init signature `PaintSlot{kind, text, bitmap, w, h, ref}`
  // used across the data-screen renderers stays binary-compatible.
  std::uint32_t mdi_codepoint = 0;
};

// Non-template paint-one-panel helper. Declared here so the template
// wrapper below stays header-only (it references EpdPanel whose type
// differs between device / WASM builds) without pulling the paint
// primitives into the template body.
//
// `vertical_desc` rotates kLabel / kLabelSplit slots 90° CCW relative to
// the caller's orientation (on device that's k180, so the effective
// native-relative rotation becomes k90Cw). Other slot kinds ignore the
// flag. Ports the v3 `splitText`'s `preferences.getBool("verticalDesc")`
// branch (see btclock_v3_fci/src/lib/drivers/epd/epd.cpp).
void PaintSlotIntoFb(LandscapeFb& lfb, const AppFonts& fonts,
                     const PaintSlot& slot, bool vertical_desc = false);

template <std::size_t N>
void PaintDataScreen(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                     uint8_t (&fb_storage)[N][16 * 296], const AppFonts& fonts,
                     const std::array<PaintSlot, N>& slots,
                     const std::array<bool, N>& update, bool full_refresh,
                     bool vertical_desc = false) {
  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;

  // Paint phase — fill the shadow framebuffers for every updated panel.
  // kBlank slots are cleared-to-white and refreshed too: that's how a
  // cell transitioning from a digit to blank actually disappears under
  // partial refresh. The earlier kBlank skip was a wrong optimization —
  // it conflated "permanent blank" with "transition to blank", causing
  // ghost ink whenever a variable-width run shrank (price digits,
  // market-cap padding, zap-overlay middle cells).
  for (std::size_t i = 0; i < N; ++i) {
    if (!update[i]) continue;
    auto lfb = PrepFb(panels, fb_storage, i);
    ClearFb(lfb, /*white=*/true);
    PaintSlotIntoFb(lfb, fonts, slots[i], vertical_desc);
  }

  // Refresh phase — fan the activation out across all updated panels
  // first (Start is non-blocking; the per-panel refresh runs in
  // parallel), then wait on them.
  for (std::size_t i = 0; i < N; ++i) {
    if (!update[i]) continue;
    panels[i]->DrawFramebufferStart(fb_storage[i], kind);
  }
  for (std::size_t i = 0; i < N; ++i) {
    if (!update[i]) continue;
    panels[i]->WaitForRefresh();
  }
}

}  // namespace btclock
