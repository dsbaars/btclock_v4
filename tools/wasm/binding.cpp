// Emscripten bindings for the IDF-port's screen stack.
//
// Two families of bindings:
//
// 1) parse<Screen>  — returns the per-panel *text* (label + digit cells)
//    as an Array<string>. Used by preview.html's simple text-cell mode,
//    doesn't need fonts/framebuffers. Pure-logic helpers from
//    main/screens/common.cpp + screen_math.cpp + fee_rate_layout.hpp.
//
// 2) render<Screen>FrameBuffer — drives the real template-on-N screen
//    renderers from main/screens/*.cpp against an in-memory epd::IEpdPanel
//    shim (tools/wasm/wasm_panel.hpp) backed by plain uint8_t arrays,
//    then returns an Array<Uint8Array> of the N panel framebuffers.
//    JS unpacks the 1-bpp bytes onto a <canvas>. This is the pixel-
//    accurate preview — same font rasterizer, same paint primitives,
//    same rotation/stride as the physical EPD.
//
// The two families live side by side so the HTML can switch between
// them (text mode is cheap and always works; pixel mode matches the
// device exactly).
//
// Preview-only runtime knobs (no device-firmware counterpart): the JS
// side calls setRenderOptions(panels, font_family) to switch between
// the 7-panel (REV_A/REV_B) and 8-panel (V8) layouts and to override
// the big-digit font (antonio/oswald/inter/sourceSerif/merriweather/
// bitter/atkinson) for validation across the firmware's shipped font
// set. The renderer templates are already instantiated for N=7 and N=8
// in main/screens/*.cpp; we dispatch at runtime into the right
// instantiation.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <memory>
#include <string>
#include <vector>

// Pull in the real screen headers — under BTCLOCK_WASM_BUILD they
// resolve to the shim epd::IEpdPanel and pure font.hpp.
#include "font_wasm_aa.hpp"
#include "fonts_app.hpp"
#include "screens/common.hpp"
#include "screens/fee_rate_layout.hpp"
#include "screens/screen_math.hpp"
#include "screens/screens.hpp"

namespace btclock {
// Pure helpers the text-mode bindings reuse. Declared in common.hpp
// but listed again here for clarity; linkage comes from common.cpp /
// screen_math.cpp.
}  // namespace btclock

using emscripten::val;

namespace {

// Max panels the shared framebuffer storage is sized for. Rev A/B are
// 7, V8 is 8 — we budget for the bigger of the two and always allocate
// the full 8 so switching between modes at runtime is free.
constexpr std::size_t kMaxPanels = 8;
// Fixed fb_storage size that the renderer templates take by reference.
constexpr std::size_t kFbBytes = 16 * 296;

// The renderer templates declare `fb_storage[N][16*296]` which is
// generous enough for both 2.13" (250h * 16 = 4000 bytes) and 2.9"
// (296h * 16 = 4736 bytes). Only the 2.13" subrange is actually written
// for the current panel kind — we allocate the full 16*296 anyway to
// keep the template signature happy.
using FbStorage7 = uint8_t[7][kFbBytes];
using FbStorage8 = uint8_t[8][kFbBytes];

// One shared set of panels + framebuffers sized for the 8-panel
// worst case. For 7-panel renders we pass a view over the first 7
// slots via a reference-cast (see As7/As8 below). The AppFonts object
// carries the stb_truetype info_ pointers for each face — safe to
// cache across calls (pure read paths, no internal state mutated
// per-draw).
struct RenderContext {
  std::array<std::unique_ptr<btclock::epd::IEpdPanel>, kMaxPanels> panels;
  uint8_t fbs[kMaxPanels][kFbBytes] = {};
  btclock::AppFonts fonts;

  // Runtime-selectable preview state. `panels_active` is 7 or 8;
  // `font_family` selects which face overrides the stock antonio slot
  // on `fonts` before a render. 0=antonio (stock), 1=oswald, 2=inter,
  // 3=sourceSerif, 4=merriweather, 5=bitter, 6=atkinson,
  // 7=antonioSemiBold, 8=antonioBold, 9=oswaldBold, 10=interBold,
  // 11=sourceSerifBold, 12=merriweatherBold, 13=bitterBold,
  // 14=atkinsonBold, 15=openRunde.
  // `vertical_desc` mirrors the on-device pref — label panels rotate 90°
  // CCW when true so "BLOCK/HEIGHT" etc. read along the panel's long
  // axis. Default false matches the device boot default.
  std::size_t panels_active = 7;
  int font_family = 0;
  bool vertical_desc = false;

  RenderContext() {
    for (std::size_t i = 0; i < kMaxPanels; ++i) {
      panels[i] = std::make_unique<btclock::epd::IEpdPanel>(
          btclock::epd::PanelKind::k2_13);
    }
  }
};

RenderContext& Ctx() {
  static RenderContext ctx;
  return ctx;
}

// Cast helpers so the shared 8-wide fb storage can be handed to a
// template instantiation that expects 7-wide storage (the template's
// first-N elements overlap byte-for-byte with the 8-wide array).
FbStorage7& As7(RenderContext& ctx) {
  return *reinterpret_cast<FbStorage7*>(ctx.fbs);
}
FbStorage8& As8(RenderContext& ctx) {
  return ctx.fbs;
}

// Matching std::array<unique_ptr<epd::IEpdPanel>, N> views over the first N
// elements of ctx.panels. We can't reinterpret_cast the array (it has
// non-trivial members), so build a fresh wrapper whose elements
// alias-own no panels — we transfer-move in and out around the call.
// Simpler: give the renderer a reference to a local std::array we
// constructed by std::move'ing the first N unique_ptrs in, then move
// them back after the call. This keeps lifetime tidy and doesn't leak.
//
// We return by value; caller should swap-back via the provided
// RestorePanels helper once the render returns.
template <std::size_t N>
std::array<std::unique_ptr<btclock::epd::IEpdPanel>, N> BorrowPanels(
    RenderContext& ctx) {
  std::array<std::unique_ptr<btclock::epd::IEpdPanel>, N> out;
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = std::move(ctx.panels[i]);
  }
  return out;
}
template <std::size_t N>
void ReturnPanels(
    RenderContext& ctx,
    std::array<std::unique_ptr<btclock::epd::IEpdPanel>, N>& borrowed) {
  for (std::size_t i = 0; i < N; ++i) {
    ctx.panels[i] = std::move(borrowed[i]);
  }
}

// Override the digit/label/small_chars/unit font roles on the shared
// AppFonts for the duration of a render. The screen renderers reach
// for `fonts.digit()` / `fonts.label()` / etc. via the role accessors;
// `SetFamily` rebinds all four swappable roles to the selected family
// at once, leaving `icon` and `sats_glyph` on their dedicated subset
// fonts (which carry glyphs the family fonts don't have).
//
// 0 = antonio (stock), 1 = oswald, 2 = inter, 3 = sourceSerif,
// 4 = merriweather, 5 = bitter, 6 = atkinson, 7 = antonioSemiBold,
// 8 = antonioBold, 9 = oswaldBold, 10 = interBold,
// 11 = sourceSerifBold, 12 = merriweatherBold, 13 = bitterBold,
// 14 = atkinsonBold, 15 = openRunde. Unknown families fall back to
// antonio — same
// semantics as the old ApplyFontOverride. Numeric ids match
// FontFamily's enum values in main/fonts_app.hpp; keep the cases in
// lockstep.
void ApplyFontOverride(btclock::AppFonts& fonts, int family) {
  btclock::FontFamily f = btclock::FontFamily::kAntonio;
  switch (family) {
    case 1:
      f = btclock::FontFamily::kOswald;
      break;
    case 2:
      f = btclock::FontFamily::kInter;
      break;
    case 3:
      f = btclock::FontFamily::kSourceSerif;
      break;
    case 4:
      f = btclock::FontFamily::kMerriweather;
      break;
    case 5:
      f = btclock::FontFamily::kBitter;
      break;
    case 6:
      f = btclock::FontFamily::kAtkinson;
      break;
    case 7:
      f = btclock::FontFamily::kAntonioSemiBold;
      break;
    case 8:
      f = btclock::FontFamily::kAntonioBold;
      break;
    case 9:
      f = btclock::FontFamily::kOswaldBold;
      break;
    case 10:
      f = btclock::FontFamily::kInterBold;
      break;
    case 11:
      f = btclock::FontFamily::kSourceSerifBold;
      break;
    case 12:
      f = btclock::FontFamily::kMerriweatherBold;
      break;
    case 13:
      f = btclock::FontFamily::kBitterBold;
      break;
    case 14:
      f = btclock::FontFamily::kAtkinsonBold;
      break;
    case 15:
      f = btclock::FontFamily::kOpenRunde;
      break;
    default:
      f = btclock::FontFamily::kAntonio;
      break;
  }
  fonts.SetFamily(f);
}

// Reset the roles back to the stock Antonio family so a render with
// family=0 starts clean regardless of what the previous render did.
// Called at the top of every render alongside ApplyFontOverride so the
// sequence is idempotent.
void ResetFontOverride(btclock::AppFonts& fonts) {
  fonts.SetFamily(btclock::FontFamily::kAntonio);
}

// Zero every panel's framebuffer (up to `n`) to "all white" (0xFF) so
// partial-refresh paths that don't touch every panel still produce a
// valid result. The device's epd::IEpdPanel::Init() also zeroes its shadow
// before the first full refresh; we mirror that here.
void ClearPanels(std::size_t n) {
  auto& ctx = Ctx();
  for (std::size_t i = 0; i < n; ++i) {
    std::memset(ctx.fbs[i], 0xFF, kFbBytes);
  }
}

// --- Alpha-buffer sidechannel ---------------------------------------------
//
// For the "AA preview" mode we also capture the grayscale alpha each
// paint primitive produced, in LOGICAL panel coords. One byte per
// pixel, 0 = white/no-ink, 255 = full black ink. See
// tools/wasm/font_wasm_aa.{hpp,cpp} for the arming and write semantics.
//
// Demux strategy: each panel's LandscapeFb carries its own unique
// native_fb pointer (pointing into ctx.fbs[i][]). We register ONE alpha
// buffer per panel's fb pointer via wasm_aa::SetPanelTargets, then run
// the screen renderer exactly once. Each paint primitive looks up its
// LandscapeFb's native_fb pointer to pick the right alpha slot — same
// mechanism the 1-bpp writes already use, just mirrored into a parallel
// grayscale buffer.
//
// Size per panel: logical_w * logical_h. For 2.13" rotated k180 that's
// 122 * 250 = 30,500 bytes; 8 panels = ~244 KB per render call. We
// allocate these per-render (std::vector on the heap) and clone into
// JS Uint8Arrays before freeing.
struct AlphaBuffers {
  int w;
  int h;
  std::size_t n;
  std::array<std::vector<uint8_t>, kMaxPanels> bufs;

  AlphaBuffers(int w_in, int h_in, std::size_t n_in)
      : w(w_in), h(h_in), n(n_in) {
    const std::size_t bytes =
        static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < n; ++i) {
      bufs[i].assign(bytes, 0);
    }
  }
};

val AlphaBuffersToVal(const AlphaBuffers& ab) {
  val out = val::array();
  for (std::size_t i = 0; i < ab.n; ++i) {
    const std::size_t bytes = ab.bufs[i].size();
    val view = val(emscripten::typed_memory_view(bytes, ab.bufs[i].data()));
    // Clone so the JS side owns stable bytes past this call.
    val clone = val::global("Uint8Array").new_(view);
    out.set(static_cast<int>(i), clone);
  }
  return out;
}

// Arm the alpha sidechannel at ctx.fbs[0..n-1], keyed on each panel's
// fb pointer. Caller must Clear() the sidechannel (or call this with a
// fresh AlphaBuffers) before every render to avoid residue.
void ArmAlphaSidechannel(AlphaBuffers& ab) {
  auto& ctx = Ctx();
  const uint8_t* fb_ptrs[kMaxPanels];
  uint8_t* alpha_ptrs[kMaxPanels];
  for (std::size_t i = 0; i < ab.n; ++i) {
    fb_ptrs[i] = ctx.fbs[i];
    alpha_ptrs[i] = ab.bufs[i].data();
  }
  btclock::wasm_aa::SetPanelTargets(ab.n, fb_ptrs, alpha_ptrs, ab.w, ab.h,
                                    /*stride=*/ab.w);
}

// Convert the shared kMaxPanels * kFbBytes framebuffer array into a JS
// Array<Uint8Array> of length `n`, each element spanning exactly the
// bytes that panel actually uses (stride * height = 16 * 250 = 4000
// for 2.13").
//
// typed_memory_view returns a Uint8Array view onto WASM linear memory
// that stays valid across calls (until WASM memory grows and the view
// is reset by emscripten itself). We clone into a fresh Uint8Array so
// the JS side owns stable bytes even across subsequent renders.
val FrameBuffersToVal(std::size_t n) {
  auto& ctx = Ctx();
  const int stride = btclock::epd::IEpdPanel::kStride;
  const int height = ctx.panels[0]->Height();
  const std::size_t used = static_cast<std::size_t>(stride) * height;

  val out = val::array();
  for (std::size_t i = 0; i < n; ++i) {
    val view = val(emscripten::typed_memory_view(used, ctx.fbs[i]));
    // Copy into a new Uint8Array that the JS caller owns. For 4000
    // bytes × 8 panels this is ~32 KB per render call — negligible.
    val clone = val::global("Uint8Array").new_(view);
    out.set(static_cast<int>(i), clone);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Text-mode bindings (Phase 1 — pre-existing semantics).
// ---------------------------------------------------------------------------

namespace {

// Text-mode still uses the compile-time kPanels that the label-drop
// behaviour was tuned against. Preserved at 7 to keep text-mode output
// identical to what it's always been; the panels-picker only affects
// pixel-mode.
constexpr std::size_t kTextPanels = 7;

val DigitsToArray(const char* label, const std::array<char, 6>& d) {
  val a = val::array();
  a.set(0, std::string(label));
  for (int i = 0; i < 6; ++i) {
    char buf[2] = {d[i], '\0'};
    a.set(i + 1, std::string(buf));
  }
  return a;
}

val parseBlockHeight(int block_height) {
  const uint32_t h =
      block_height < 0 ? 0u : static_cast<uint32_t>(block_height);
  // Label-drop parity: 7-digit heights (>=1_000_000 on a 7-panel board)
  // paint every panel as a digit; see BlockHeightDropsLabel.
  if (btclock::BlockHeightDropsLabel(h, kTextPanels)) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(h));
    val a = val::array();
    const std::size_t len = std::strlen(buf);
    const std::size_t skip = len > kTextPanels ? len - kTextPanels : 0;
    for (std::size_t i = 0; i < kTextPanels; ++i) {
      char cell[2] = {buf[skip + i], '\0'};
      a.set(static_cast<int>(i), std::string(cell));
    }
    return a;
  }
  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  btclock::FormatDigits(h, d.data(), 6);
  return DigitsToArray("BLOCK/HEIGHT", d);
}

val parsePriceData(int price_int, std::string currency) {
  const std::string label = "BTC/" + currency;
  const std::string symbol_storage = btclock::CurrencySymbolUtf8(currency);
  const char* symbol_utf8 = symbol_storage.c_str();
  const bool use_symbol = symbol_utf8[0] != '\0';

  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  std::array<bool, 6> is_sym{};
  const int32_t vv = price_int < 0 ? 0 : price_int;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", vv);
  const std::size_t len = std::strlen(buf);
  if (len >= 6) {
    for (std::size_t i = 0; i < 6; ++i) d[i] = buf[len - 6 + i];
  } else {
    const std::size_t pad = 6 - len;
    for (std::size_t i = pad; i < 6; ++i) d[i] = buf[i - pad];
    if (use_symbol && pad > 0) is_sym[pad - 1] = true;
  }

  val a = val::array();
  a.set(0, label);
  for (int i = 0; i < 6; ++i) {
    if (is_sym[i]) {
      a.set(i + 1, std::string(symbol_utf8));
    } else {
      char cell[2] = {d[i], '\0'};
      a.set(i + 1, std::string(cell));
    }
  }
  return a;
}

val parseSatsPerCurrency(int price_int, std::string currency,
                         bool with_sats_symbol) {
  char price_buf[16];
  std::snprintf(price_buf, sizeof(price_buf), "%d",
                price_int < 0 ? 0 : price_int);
  const int32_t sats = btclock::SatsPerUnit(price_buf);
  const bool moscow = currency == "USD" && sats > 0 && sats < 100000;
  const std::string label = moscow ? "MSCW/TIME" : ("SATS/" + currency);

  const btclock::DigitLayout layout =
      btclock::ComputeMoscowLayout(sats, with_sats_symbol);

  val a = val::array();
  a.set(0, label);
  static const char* kSatsPlaceholder = "\xE2\x9A\xA1";  // U+26A1
  for (int i = 0; i < 6; ++i) {
    if (layout.is_sats[i]) {
      a.set(i + 1, std::string(kSatsPlaceholder));
    } else {
      char cell[2] = {layout.digits[i], '\0'};
      a.set(i + 1, std::string(cell));
    }
  }
  return a;
}

// parseBlockFees — "FEE/RATE" + fee digits (decimal-aware) + "sat/vB"
// unit tail. Accepts a double so fractional values (blockfee2 / nostr
// d=medianFee) round-trip through the preview the same way they do on
// the device. The 7-element output array mirrors the on-device 7-panel
// board: slot 0 = label, slots 1..5 = 5 digit cells, slot 6 = unit.
val parseBlockFees(double fee_sats_vb) {
  std::array<char, 5> d{};
  btclock::LayoutFeeRate<5>(fee_sats_vb, d);
  val a = val::array();
  a.set(0, std::string("FEE/RATE"));
  for (int i = 0; i < 5; ++i) {
    char cell[2] = {d[static_cast<std::size_t>(i)], '\0'};
    a.set(i + 1, std::string(cell));
  }
  a.set(6, std::string("sat/vB"));
  return a;
}

// Halving / market-cap / bitcoin-supply text layouts. The per-digit
// layout helpers live in screen_math.hpp (FormatDigits64, etc.).
// `bigchars` is vestigial — kept for ABI compatibility with the
// preview.html call site. The halving text path always emits the
// blocks-remaining digit form; the time-breakdown variant lives in
// the pixel-mode `renderHalvingCountdownWithFlagsAlpha` binding.
val parseHalvingCountdown(int block_height, bool /*bigchars_inert*/) {
  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  if (block_height >= 0) {
    const uint32_t rem =
        btclock::HalvingCountdown(static_cast<uint32_t>(block_height));
    btclock::FormatDigits(rem, d.data(), 6);
  }
  return DigitsToArray("HAL/VING", d);
}

// `bigchars` is vestigial — see HANDBOOK § "Market cap": the device's
// EPD path always paints the big-char form, so the toggle has no
// effect here either. Kept for preview.html ABI compatibility.
val parseMarketCap(int block_height, int price_int, std::string currency,
                   bool /*bigchars_inert*/) {
  const std::string label = currency + "/MCAP";
  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  if (block_height >= 0 && price_int >= 0) {
    const uint64_t cap = btclock::MarketCap(
        static_cast<uint32_t>(price_int), static_cast<uint32_t>(block_height));
    // 6 slots on the preview; leading digits are truncated on overflow.
    char buf[24];
    btclock::FormatDigits64(cap, buf, 6);
    for (int i = 0; i < 6; ++i) d[i] = buf[i];
  }
  return DigitsToArray(label.c_str(), d);
}

val parseBitcoinSupply(int block_height, bool big_chars, bool show_percent) {
  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  if (block_height >= 0) {
    const uint64_t supply =
        btclock::SupplyAtBlock(static_cast<uint32_t>(block_height));
    if (show_percent) {
      // Match BuildBitcoinSupply's percent branch: "NN.NN%" right-padded
      // into the 6 digit slots, with a trailing '%' so the preview's
      // text mode mirrors what the EPD would paint.
      const double frac =
          std::round((static_cast<double>(supply) / 20999999.9769) * 10000.0) /
          100.0;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%.2f%%", frac);
      const std::size_t len = std::strlen(buf);
      const std::size_t pad = len < 6 ? 6 - len : 0;
      for (std::size_t i = 0; i < 6; ++i) {
        d[i] = i < pad ? ' ' : buf[i - pad];
      }
    } else if (big_chars) {
      // "NN.NM" / "NNNK" big-chars suffix form, anchored in the digit
      // budget so the parse* preview matches the EPD render.
      std::string s = btclock::FormatNumberWithSuffix(supply, 6);
      if (s.size() < 6) s.insert(s.begin(), 6 - s.size(), ' ');
      for (std::size_t i = 0; i < 6; ++i) d[i] = s[i];
    } else {
      char buf[24];
      btclock::FormatDigits64(supply, buf, 6);
      for (int i = 0; i < 6; ++i) d[i] = buf[i];
    }
  }
  return DigitsToArray("BTC/SUPPLY", d);
}

}  // namespace

// ---------------------------------------------------------------------------
// Pixel-mode bindings (Phase 2 — real screen renderers).
//
// Each render<Screen>FrameBuffer wipes the shared fb array, runs the
// matching Render<Screen>Screen template (forcing a full refresh by
// passing empty/zero prev-state), then returns an Array<Uint8Array>
// of the N resulting panel buffers where N is the preview's active
// panel count (7 or 8 — set via setRenderOptions).
// ---------------------------------------------------------------------------

namespace {

// Dispatch helpers — `F7` runs a template<7> render, `F8` runs the
// template<8>. Keeps the per-screen binding code from ballooning
// with parallel branches, and localises the swap-panels dance in one
// place.
template <typename F7, typename F8>
void DispatchByPanels(F7&& f7, F8&& f8) {
  auto& ctx = Ctx();
  // Apply font-family override for this render, then reset after.
  // Cheap (one Font ctor per render); guarantees family 0 always
  // gets a clean stock Antonio regardless of what the previous
  // render did.
  ResetFontOverride(ctx.fonts);
  ApplyFontOverride(ctx.fonts, ctx.font_family);

  if (ctx.panels_active == 8) {
    auto borrowed = BorrowPanels<8>(ctx);
    f8(ctx, borrowed, As8(ctx));
    ReturnPanels<8>(ctx, borrowed);
  } else {
    auto borrowed = BorrowPanels<7>(ctx);
    f7(ctx, borrowed, As7(ctx));
    ReturnPanels<7>(ctx, borrowed);
  }
}

val renderBlockHeight(int block_height) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderBlockHeightScreen<7>(pans, fbs, c.fonts, bh, 0,
                                            /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderBlockHeightScreen<8>(pans, fbs, c.fonts, bh, 0,
                                            /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

// Test-only variant that exposes the decoupled full_refresh_mode knob
// (btclock_v4-jo6). Runs block-height render with caller-supplied
// `prev_height` (for cell-diff reset control) and `full_refresh_mode`,
// then returns an object containing the framebuffers AND the last
// RefreshKind the panel shim captured. Lets smoke_test.mjs verify that
// partial-mode actually flows kPartial to DrawFramebufferStart while
// still producing a valid framebuffer.
val renderBlockHeightWithMode(int block_height, int prev_height,
                              bool full_refresh_mode) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  for (std::size_t i = 0; i < ctx.panels_active; ++i) {
    ctx.panels[i]->reset_refresh_state();
  }
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  const uint32_t ph = prev_height < 0 ? 0 : static_cast<uint32_t>(prev_height);
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderBlockHeightScreen<7>(pans, fbs, c.fonts, bh, ph,
                                            full_refresh_mode, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderBlockHeightScreen<8>(pans, fbs, c.fonts, bh, ph,
                                            full_refresh_mode, vd);
      });
  val out = val::object();
  out.set("frameBuffers", FrameBuffersToVal(ctx.panels_active));
  // Report refresh kind from the first panel that the renderer touched.
  // Every updated panel sees the same kind (PaintDataScreen dispatches
  // uniformly), so picking the first one is sufficient.
  int kind_code = -1;
  int total_refreshes = 0;
  for (std::size_t i = 0; i < ctx.panels_active; ++i) {
    total_refreshes += ctx.panels[i]->refresh_count();
    if (ctx.panels[i]->refresh_count() > 0 && kind_code == -1) {
      kind_code = static_cast<int>(ctx.panels[i]->last_refresh_kind());
    }
  }
  // 0 = kFull, 1 = kPartial, -1 = no panel refreshed (nothing dirty).
  out.set("refreshKind", kind_code);
  out.set("refreshCount", total_refreshes);
  return out;
}

val renderPriceData(int price_int, std::string currency) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  char price_buf[24];
  std::snprintf(price_buf, sizeof(price_buf), "%d",
                price_int < 0 ? 0 : price_int);
  const std::string symbol_storage = btclock::CurrencySymbolUtf8(currency);
  const char* symbol_utf8 = symbol_storage.c_str();
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderBtcPriceScreen<7>(
            pans, fbs, c.fonts, currency, price_buf, "", symbol_utf8, false,
            false, /*share_dot=*/false, /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderBtcPriceScreen<8>(
            pans, fbs, c.fonts, currency, price_buf, "", symbol_utf8, false,
            false, /*share_dot=*/false, /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

val renderSatsPerCurrency(int price_int, std::string currency,
                          bool with_sats_symbol) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  char price_buf[24];
  std::snprintf(price_buf, sizeof(price_buf), "%d",
                price_int < 0 ? 0 : price_int);
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderMoscowTimeScreen<7>(
            pans, fbs, c.fonts, currency, price_buf, "",
            btclock::kSatsVariantDefault, with_sats_symbol, true,
            /*share_dot=*/false, /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderMoscowTimeScreen<8>(
            pans, fbs, c.fonts, currency, price_buf, "",
            btclock::kSatsVariantDefault, with_sats_symbol, true,
            /*share_dot=*/false, /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

val renderBlockFees(int fee_sats_vb) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  const double fee = fee_sats_vb < 0 ? 0.0 : static_cast<double>(fee_sats_vb);
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderFeeRateScreen<7>(pans, fbs, c.fonts, fee, -1,
                                        /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderFeeRateScreen<8>(pans, fbs, c.fonts, fee, -1,
                                        /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

// `bigchars` is vestigial — RenderHalvingScreen has no big-chars
// toggle. The blocks-vs-time toggle is exposed by the dedicated
// `renderHalvingCountdownWithFlagsAlpha` binding.
val renderHalvingCountdown(int block_height, bool /*bigchars_inert*/) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderHalvingScreen<7>(pans, fbs, c.fonts, bh, 0, true,
                                        /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderHalvingScreen<8>(pans, fbs, c.fonts, bh, 0, true,
                                        /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

// `bigchars` is vestigial — see HANDBOOK § "Market cap": the EPD
// path always paints the big-char form. The arg stays for ABI
// compatibility with preview.html.
val renderMarketCap(int block_height, int price_int, std::string currency,
                    bool /*bigchars_inert*/) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  char price_buf[24];
  std::snprintf(price_buf, sizeof(price_buf), "%d",
                price_int < 0 ? 0 : price_int);
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderMarketCapScreen<7>(
            pans, fbs, c.fonts, currency, price_buf, bh, "", 0, true,
            /*share_dot=*/false, /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderMarketCapScreen<8>(
            pans, fbs, c.fonts, currency, price_buf, bh, "", 0, true,
            /*share_dot=*/false, /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

val renderBitcoinSupply(int block_height, bool big_chars, bool show_percent) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderBitcoinSupplyScreen<7>(pans, fbs, c.fonts, bh, 0,
                                              big_chars, show_percent,
                                              /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderBitcoinSupplyScreen<8>(pans, fbs, c.fonts, bh, 0,
                                              big_chars, show_percent,
                                              /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

// --- Mining pool + bitaxe + nostr zap bindings -----------------------------
//
// These four screens don't take a plain integer like block height — they
// consume a `DataSnapshot` sub-struct (PoolStats / BitaxeStats / LatestZap).
// JS assembles a plain args list and we repack it into the struct on this
// side so the binding surface stays simple strings + numbers.

// Build a PoolStats from primitive args. `daily_sats < 0` → nullopt so
// the renderer's "no-sample" branch fires; any non-negative value passes
// through verbatim.
btclock::DataSnapshot::PoolStats MakePoolStats(const std::string& name,
                                               const std::string& hashrate,
                                               double daily_sats) {
  btclock::DataSnapshot::PoolStats out;
  out.name = name;
  out.hashrate = hashrate;
  if (daily_sats >= 0.0) {
    out.daily_sats = static_cast<int64_t>(daily_sats);
  }
  return out;
}

val renderMiningPoolHashrate(std::string pool_name, std::string hashrate) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  const auto stats = MakePoolStats(pool_name, hashrate, -1.0);
  // Empty `prev_pool` triggers the renderer's full-refresh path — a must
  // for the preview, which has no state across calls.
  const btclock::DataSnapshot::PoolStats prev{};
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderMiningPoolHashrateScreen<7>(
            pans, fbs, c.fonts, stats, prev,
            /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderMiningPoolHashrateScreen<8>(
            pans, fbs, c.fonts, stats, prev,
            /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

val renderMiningPoolEarnings(std::string pool_name, double daily_sats) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  // hashrate not consumed by the earnings screen — pass empty string.
  const auto stats = MakePoolStats(pool_name, "", daily_sats);
  const btclock::DataSnapshot::PoolStats prev{};
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderMiningPoolEarningsScreen<7>(
            pans, fbs, c.fonts, stats, prev,
            /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderMiningPoolEarningsScreen<8>(
            pans, fbs, c.fonts, stats, prev,
            /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

val renderBitaxeHashrate(std::string hostname, double hashrate_ghs) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  std::optional<double> ghs;
  if (hashrate_ghs > 0.0) ghs = hashrate_ghs;
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderBitaxeHashrateScreen<7>(
            pans, fbs, c.fonts, hostname, ghs,
            /*full_refresh_mode=*/true, /*prev_value=*/"", vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderBitaxeHashrateScreen<8>(
            pans, fbs, c.fonts, hostname, ghs,
            /*full_refresh_mode=*/true, /*prev_value=*/"", vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

val renderBitaxeBestDiff(std::string hostname, std::string best_diff) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  std::optional<std::string> bd;
  if (!best_diff.empty()) bd = best_diff;
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderBitaxeBestDiffScreen<7>(pans, fbs, c.fonts, hostname, bd,
                                               /*full_refresh_mode=*/true,
                                               /*prev_value=*/"", vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderBitaxeBestDiffScreen<8>(pans, fbs, c.fonts, hostname, bd,
                                               /*full_refresh_mode=*/true,
                                               /*prev_value=*/"", vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

val renderNostrZap(double amount_sats) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  btclock::DataSnapshot::LatestZap zap;
  if (amount_sats >= 0.0) {
    zap.amount_sats = static_cast<int64_t>(amount_sats);
  }
  // message intentionally left empty — the renderer no longer paints it.
  const bool vd = ctx.vertical_desc;
  DispatchByPanels(
      [&](RenderContext& c, auto& pans, FbStorage7& fbs) {
        btclock::RenderNostrZapScreen<7>(pans, fbs, c.fonts, zap, false,
                                         btclock::kSatsVariantDefault,
                                         /*full_refresh_mode=*/true, vd);
      },
      [&](RenderContext& c, auto& pans, FbStorage8& fbs) {
        btclock::RenderNostrZapScreen<8>(pans, fbs, c.fonts, zap, false,
                                         btclock::kSatsVariantDefault,
                                         /*full_refresh_mode=*/true, vd);
      });
  return FrameBuffersToVal(ctx.panels_active);
}

// ---------------------------------------------------------------------------
// AA-mode bindings — companions to each render<Screen>FrameBuffer that
// return the grayscale alpha buffers instead of (or alongside) the 1-bpp
// framebuffer. One byte per logical pixel, 0=no-ink, 255=full-ink. See
// font_wasm_aa.hpp for the semantics.
//
// Implementation: arm the sidechannel at a fresh AlphaBuffers, run the
// exact same render path as the 1-bpp binding, disarm, serialize. The
// 1-bpp fb is still written as a side effect (the paint primitives do
// BOTH), but the caller ignores it — only the alpha array is returned.
// ---------------------------------------------------------------------------

// Helper used by every render*AlphaBuffer: allocate buffers, arm, run,
// disarm, emit. `run_render()` invokes the matching Render<Screen>Screen
// template with ctx.panels/ctx.fbs/ctx.fonts already zeroed.
template <typename Fn>
val RunAlphaRender(Fn&& run_render) {
  auto& ctx = Ctx();
  ClearPanels(ctx.panels_active);
  const int w = ctx.panels[0]->Width();
  const int h = ctx.panels[0]->Height();
  AlphaBuffers ab(w, h, ctx.panels_active);
  ArmAlphaSidechannel(ab);
  // Same font-override dance as the 1-bpp path — the AA and 1-bpp runs
  // need to see identical font state or the two previews won't match.
  ResetFontOverride(ctx.fonts);
  ApplyFontOverride(ctx.fonts, ctx.font_family);
  run_render();
  btclock::wasm_aa::Clear();
  return AlphaBuffersToVal(ab);
}

val renderBlockHeightAlpha(int block_height) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderBlockHeightScreen<8>(borrowed, As8(ctx), ctx.fonts, bh, 0,
                                          true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderBlockHeightScreen<7>(borrowed, As7(ctx), ctx.fonts, bh, 0,
                                          true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderPriceDataAlpha(int price_int, std::string currency) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    char price_buf[24];
    std::snprintf(price_buf, sizeof(price_buf), "%d",
                  price_int < 0 ? 0 : price_int);
    const std::string symbol_storage = btclock::CurrencySymbolUtf8(currency);
    const char* symbol_utf8 = symbol_storage.c_str();
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderBtcPriceScreen<8>(borrowed, As8(ctx), ctx.fonts, currency,
                                       price_buf, "", symbol_utf8, false, false,
                                       false, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderBtcPriceScreen<7>(borrowed, As7(ctx), ctx.fonts, currency,
                                       price_buf, "", symbol_utf8, false, false,
                                       false, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// Same as renderPriceDataAlpha but exposes the `suffixPrice`,
// `mowMode`, and `decimalShareDot` flags. The plain renderer always
// passes false; this variant lets the docs renderer demonstrate the
// k/M-suffix layout, Million-Of-Watoshis layout, and the dot-folding
// compact form at realistic price values.
val renderPriceDataWithFlagsAlpha(int price_int, std::string currency,
                                  bool suffix_price, bool mow_mode,
                                  bool share_dot) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    char price_buf[24];
    std::snprintf(price_buf, sizeof(price_buf), "%d",
                  price_int < 0 ? 0 : price_int);
    // CurrencySymbolUtf8 returns std::string (with an ISO-code fallback
    // for unknown codes); materialise to keep .c_str() pointer stable
    // across the renderer call.
    const std::string symbol_storage = btclock::CurrencySymbolUtf8(currency);
    const char* symbol_utf8 = symbol_storage.c_str();
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderBtcPriceScreen<8>(borrowed, As8(ctx), ctx.fonts, currency,
                                       price_buf, "", symbol_utf8, suffix_price,
                                       mow_mode, share_dot, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderBtcPriceScreen<7>(borrowed, As7(ctx), ctx.fonts, currency,
                                       price_buf, "", symbol_utf8, suffix_price,
                                       mow_mode, share_dot, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderSatsPerCurrencyAlpha(int price_int, std::string currency,
                               bool with_sats_symbol) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    char price_buf[24];
    std::snprintf(price_buf, sizeof(price_buf), "%d",
                  price_int < 0 ? 0 : price_int);
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderMoscowTimeScreen<8>(
          borrowed, As8(ctx), ctx.fonts, currency, price_buf, "",
          btclock::kSatsVariantDefault, with_sats_symbol, true,
          /*share_dot=*/false, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderMoscowTimeScreen<7>(
          borrowed, As7(ctx), ctx.fonts, currency, price_buf, "",
          btclock::kSatsVariantDefault, with_sats_symbol, true,
          /*share_dot=*/false, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// Moscow-time renderer with the `useSatsSymbol` flag exposed. When
// `use_sats_symbol=false` the sats glyph in the marker cell is
// suppressed (rendered blank), so the docs can show the impact of
// the `useSatsSymbol` toggle.
val renderSatsPerCurrencyWithFlagsAlpha(int price_int, std::string currency,
                                        bool use_sats_symbol) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    char price_buf[24];
    std::snprintf(price_buf, sizeof(price_buf), "%d",
                  price_int < 0 ? 0 : price_int);
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderMoscowTimeScreen<8>(
          borrowed, As8(ctx), ctx.fonts, currency, price_buf, "",
          btclock::kSatsVariantDefault, use_sats_symbol, true,
          /*share_dot=*/false, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderMoscowTimeScreen<7>(
          borrowed, As7(ctx), ctx.fonts, currency, price_buf, "",
          btclock::kSatsVariantDefault, use_sats_symbol, true,
          /*share_dot=*/false, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderBlockFeesAlpha(int fee_sats_vb) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const double fee = fee_sats_vb < 0 ? 0.0 : static_cast<double>(fee_sats_vb);
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderFeeRateScreen<8>(borrowed, As8(ctx), ctx.fonts, fee, -1,
                                      true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderFeeRateScreen<7>(borrowed, As7(ctx), ctx.fonts, fee, -1,
                                      true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// Fee renderer that takes a double so the docs can show the
// `blockFeeDec=true` decimal-form layout (e.g. 4.5 sats/vB rendered
// as "4.5"). The integer form is what renderBlockFeesAlpha already
// covers; the decimal form only kicks in when the fractional value
// fits the digit-cell budget.
val renderBlockFeesDecimalAlpha(double fee_sats_vb) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const double fee = fee_sats_vb < 0.0 ? 0.0 : fee_sats_vb;
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderFeeRateScreen<8>(borrowed, As8(ctx), ctx.fonts, fee, -1,
                                      true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderFeeRateScreen<7>(borrowed, As7(ctx), ctx.fonts, fee, -1,
                                      true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// `bigchars` is vestigial — see renderHalvingCountdown above.
val renderHalvingCountdownAlpha(int block_height, bool /*bigchars_inert*/) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderHalvingScreen<8>(borrowed, As8(ctx), ctx.fonts, bh, 0,
                                      true, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderHalvingScreen<7>(borrowed, As7(ctx), ctx.fonts, bh, 0,
                                      true, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// Halving countdown with the `useBlkCountdown` flag exposed. When
// `as_blocks=false` the layout switches from blocks-remaining to
// time-remaining (years / days / hours / minutes form).
val renderHalvingCountdownWithFlagsAlpha(int block_height, bool as_blocks) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderHalvingScreen<8>(borrowed, As8(ctx), ctx.fonts, bh, 0,
                                      as_blocks, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderHalvingScreen<7>(borrowed, As7(ctx), ctx.fonts, bh, 0,
                                      as_blocks, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// `bigchars` is vestigial — see renderMarketCap above.
val renderMarketCapAlpha(int block_height, int price_int, std::string currency,
                         bool /*bigchars_inert*/) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    char price_buf[24];
    std::snprintf(price_buf, sizeof(price_buf), "%d",
                  price_int < 0 ? 0 : price_int);
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderMarketCapScreen<8>(borrowed, As8(ctx), ctx.fonts, currency,
                                        price_buf, bh, "", 0, true,
                                        /*share_dot=*/false, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderMarketCapScreen<7>(borrowed, As7(ctx), ctx.fonts, currency,
                                        price_buf, bh, "", 0, true,
                                        /*share_dot=*/false, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderBitcoinSupplyAlpha(int block_height, bool big_chars,
                             bool show_percent) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderBitcoinSupplyScreen<8>(borrowed, As8(ctx), ctx.fonts, bh,
                                            0, big_chars, show_percent, true,
                                            vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderBitcoinSupplyScreen<7>(borrowed, As7(ctx), ctx.fonts, bh,
                                            0, big_chars, show_percent, true,
                                            vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderMiningPoolHashrateAlpha(std::string pool_name, std::string hashrate) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const auto stats = MakePoolStats(pool_name, hashrate, -1.0);
    const btclock::DataSnapshot::PoolStats prev{};
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderMiningPoolHashrateScreen<8>(borrowed, As8(ctx), ctx.fonts,
                                                 stats, prev, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderMiningPoolHashrateScreen<7>(borrowed, As7(ctx), ctx.fonts,
                                                 stats, prev, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderMiningPoolEarningsAlpha(std::string pool_name, double daily_sats) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const auto stats = MakePoolStats(pool_name, "", daily_sats);
    const btclock::DataSnapshot::PoolStats prev{};
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderMiningPoolEarningsScreen<8>(borrowed, As8(ctx), ctx.fonts,
                                                 stats, prev, true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderMiningPoolEarningsScreen<7>(borrowed, As7(ctx), ctx.fonts,
                                                 stats, prev, true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderBitaxeHashrateAlpha(std::string hostname, double hashrate_ghs) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    std::optional<double> ghs;
    if (hashrate_ghs > 0.0) ghs = hashrate_ghs;
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderBitaxeHashrateScreen<8>(borrowed, As8(ctx), ctx.fonts,
                                             hostname, ghs, true, "", vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderBitaxeHashrateScreen<7>(borrowed, As7(ctx), ctx.fonts,
                                             hostname, ghs, true, "", vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderBitaxeBestDiffAlpha(std::string hostname, std::string best_diff) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    std::optional<std::string> bd;
    if (!best_diff.empty()) bd = best_diff;
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderBitaxeBestDiffScreen<8>(borrowed, As8(ctx), ctx.fonts,
                                             hostname, bd, true, "", vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderBitaxeBestDiffScreen<7>(borrowed, As7(ctx), ctx.fonts,
                                             hostname, bd, true, "", vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

val renderNostrZapAlpha(double amount_sats) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    btclock::DataSnapshot::LatestZap zap;
    if (amount_sats >= 0.0) {
      zap.amount_sats = static_cast<int64_t>(amount_sats);
    }
    // message intentionally left empty — the renderer no longer paints it.
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderNostrZapScreen<8>(borrowed, As8(ctx), ctx.fonts, zap,
                                       false, btclock::kSatsVariantDefault,
                                       /*full_refresh_mode=*/true, vd);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderNostrZapScreen<7>(borrowed, As7(ctx), ctx.fonts, zap,
                                       false, btclock::kSatsVariantDefault,
                                       /*full_refresh_mode=*/true, vd);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// Clock screen — date/HH:MM. Args are (hour 0..23, minute 0..59,
// mday 1..31, month 1..12, hide_leading_zero). Always renders as a
// "valid" frame (no NTP-pending placeholder) so the docs render
// shows the populated clock face.
val renderClockAlpha(int hour, int minute, int mday, int month,
                     bool hide_leading_zero) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const bool vd = ctx.vertical_desc;
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderClockScreen<8>(
          borrowed, As8(ctx), ctx.fonts,
          /*valid=*/true, hour, minute, mday, month,
          /*prev_valid=*/false, /*prev_hour=*/0, /*prev_minute=*/0,
          /*prev_mday=*/0, /*prev_month=*/0,
          /*full_refresh_mode=*/true, vd, hide_leading_zero);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderClockScreen<7>(
          borrowed, As7(ctx), ctx.fonts,
          /*valid=*/true, hour, minute, mday, month,
          /*prev_valid=*/false, /*prev_hour=*/0, /*prev_minute=*/0,
          /*prev_mday=*/0, /*prev_month=*/0,
          /*full_refresh_mode=*/true, vd, hide_leading_zero);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// Debug overlay — IP/SSID/heap/PSRAM/HW+FW+built/uptime. Mirrors the
// fields the firmware's event_loop populates from runtime state, but
// every value is caller-supplied so the docs render reflects a
// realistic snapshot without depending on esp_app_desc / heap_caps.
val renderDebugAlpha(std::string ip, std::string ssid, int free_heap_bytes,
                     int free_psram_bytes, std::string hw_name,
                     std::string fw_version, std::string built, int uptime_s) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    btclock::DebugScreenInfo info;
    info.ip = std::move(ip);
    info.ssid = std::move(ssid);
    info.free_heap =
        free_heap_bytes < 0 ? 0u : static_cast<uint32_t>(free_heap_bytes);
    info.free_psram =
        free_psram_bytes < 0 ? 0u : static_cast<uint32_t>(free_psram_bytes);
    info.hw_name = hw_name.c_str();
    info.fw_version = fw_version.c_str();
    info.built = built.c_str();
    info.uptime_s = uptime_s < 0 ? 0u : static_cast<uint32_t>(uptime_s);
    if (ctx.panels_active == 8) {
      auto borrowed = BorrowPanels<8>(ctx);
      btclock::RenderDebugScreen<8>(borrowed, As8(ctx), ctx.fonts, info,
                                    /*full_refresh=*/true);
      ReturnPanels<8>(ctx, borrowed);
    } else {
      auto borrowed = BorrowPanels<7>(ctx);
      btclock::RenderDebugScreen<7>(borrowed, As7(ctx), ctx.fonts, info,
                                    /*full_refresh=*/true);
      ReturnPanels<7>(ctx, borrowed);
    }
  });
}

// Runtime-switchable preview knobs. `panels` is 7 or 8 (anything else
// is clamped to 7); `font_family` is 0=antonio (stock), 1=oswald,
// 2=inter, 3=sourceSerif, 4=merriweather, 5=bitter, 6=atkinson,
// 7=antonioSemiBold, 8=antonioBold, 9=oswaldBold, 10=interBold,
// 11=sourceSerifBold, 12=merriweatherBold, 13=bitterBold,
// 14=atkinsonBold, 15=openRunde. Unknown font families fall back to
// stock. Call whenever the user changes the UI selector; safe to call
// before every render.
void setRenderOptions(int panels, int font_family) {
  auto& ctx = Ctx();
  ctx.panels_active = (panels == 8) ? 8 : 7;
  if (font_family < 0 || font_family > 15) font_family = 0;
  ctx.font_family = font_family;
}

// Mirror of the `verticalDesc` device pref — flips the label panel's
// orientation 90° CCW so "BLOCK/HEIGHT" and sibling labels read along
// the panel's long axis. Exposed as a separate setter so the preview
// UI can toggle it without recomputing the panels/font_family state;
// safe to call before every render.
void setVerticalDesc(bool on) {
  Ctx().vertical_desc = on;
}

// Panel dimension metadata — JS needs this to set the <canvas> size
// and drive the 1-bpp iteration. Rotation is fixed at k180 to match
// the REV_B solder orientation; native is 122x250 for 2.13" panels.
// `panels` reflects the current setRenderOptions selection so the UI
// can size its row correctly.
// Debug-only — returns bbox metrics for a single codepoint at the given
// pixel height, querying any of the AppFonts roles. Used by the bolt-
// centering analysis script to compare what DrawCodepointCentered
// "thinks" the glyph extents are vs. what gets rasterized.
val getCodepointMetrics(std::string role, int codepoint, double pixel_height) {
  const auto& f = Ctx().fonts;
  const btclock::Font* font = nullptr;
  if (role == "icon")
    font = &f.icon();
  else if (role == "digit")
    font = &f.digit();
  else if (role == "label")
    font = &f.label();
  else if (role == "sats_glyph")
    font = &f.sats_glyph();
  else if (role == "atkinson")
    font = &f.atkinson();
  else
    font = &f.icon();
  const auto m = font->GetMetrics(codepoint, static_cast<float>(pixel_height));
  val o = val::object();
  o.set("xoff", m.xoff);
  o.set("yoff", m.yoff);
  o.set("w", m.w);
  o.set("h", m.h);
  o.set("advance", m.advance);
  return o;
}

val getPanelDimensions() {
  auto& ctx = Ctx();
  val o = val::object();
  o.set("width", ctx.panels[0]->Width());
  o.set("height", ctx.panels[0]->Height());
  o.set("stride", btclock::epd::IEpdPanel::kStride);
  // 0 = k0, 1 = k90Cw, 2 = k180, 3 = k90Ccw (matches Rotation enum).
  o.set("rotation", 2);
  o.set("panels", static_cast<int>(ctx.panels_active));
  return o;
}

}  // namespace

EMSCRIPTEN_BINDINGS(btclock_idf_screens) {
  // Text mode.
  emscripten::function("parseBlockHeight", &parseBlockHeight);
  emscripten::function("parsePriceData", &parsePriceData);
  emscripten::function("parseSatsPerCurrency", &parseSatsPerCurrency);
  emscripten::function("parseBlockFees", &parseBlockFees);
  emscripten::function("parseHalvingCountdown", &parseHalvingCountdown);
  emscripten::function("parseMarketCap", &parseMarketCap);
  emscripten::function("parseBitcoinSupply", &parseBitcoinSupply);

  // Pixel mode (1-bpp — what the physical panel shows).
  emscripten::function("renderBlockHeightFrameBuffer", &renderBlockHeight);
  emscripten::function("renderBlockHeightWithMode", &renderBlockHeightWithMode);
  emscripten::function("renderPriceDataFrameBuffer", &renderPriceData);
  emscripten::function("renderSatsPerCurrencyFrameBuffer",
                       &renderSatsPerCurrency);
  emscripten::function("renderBlockFeesFrameBuffer", &renderBlockFees);
  emscripten::function("renderHalvingCountdownFrameBuffer",
                       &renderHalvingCountdown);
  emscripten::function("renderMarketCapFrameBuffer", &renderMarketCap);
  emscripten::function("renderBitcoinSupplyFrameBuffer", &renderBitcoinSupply);
  emscripten::function("renderMiningPoolHashrateFrameBuffer",
                       &renderMiningPoolHashrate);
  emscripten::function("renderMiningPoolEarningsFrameBuffer",
                       &renderMiningPoolEarnings);
  emscripten::function("renderBitaxeHashrateFrameBuffer",
                       &renderBitaxeHashrate);
  emscripten::function("renderBitaxeBestDiffFrameBuffer",
                       &renderBitaxeBestDiff);
  emscripten::function("renderNostrZapFrameBuffer", &renderNostrZap);

  // AA mode (grayscale alpha, logical-coord, preview-only — one byte
  // per pixel, 0=white/no-ink..255=full-black-ink).
  emscripten::function("renderBlockHeightAlphaBuffer", &renderBlockHeightAlpha);
  emscripten::function("renderPriceDataAlphaBuffer", &renderPriceDataAlpha);
  emscripten::function("renderPriceDataWithFlagsAlphaBuffer",
                       &renderPriceDataWithFlagsAlpha);
  emscripten::function("renderSatsPerCurrencyAlphaBuffer",
                       &renderSatsPerCurrencyAlpha);
  emscripten::function("renderSatsPerCurrencyWithFlagsAlphaBuffer",
                       &renderSatsPerCurrencyWithFlagsAlpha);
  emscripten::function("renderBlockFeesDecimalAlphaBuffer",
                       &renderBlockFeesDecimalAlpha);
  emscripten::function("renderHalvingCountdownWithFlagsAlphaBuffer",
                       &renderHalvingCountdownWithFlagsAlpha);
  emscripten::function("renderBlockFeesAlphaBuffer", &renderBlockFeesAlpha);
  emscripten::function("renderHalvingCountdownAlphaBuffer",
                       &renderHalvingCountdownAlpha);
  emscripten::function("renderMarketCapAlphaBuffer", &renderMarketCapAlpha);
  emscripten::function("renderBitcoinSupplyAlphaBuffer",
                       &renderBitcoinSupplyAlpha);
  emscripten::function("renderMiningPoolHashrateAlphaBuffer",
                       &renderMiningPoolHashrateAlpha);
  emscripten::function("renderMiningPoolEarningsAlphaBuffer",
                       &renderMiningPoolEarningsAlpha);
  emscripten::function("renderBitaxeHashrateAlphaBuffer",
                       &renderBitaxeHashrateAlpha);
  emscripten::function("renderBitaxeBestDiffAlphaBuffer",
                       &renderBitaxeBestDiffAlpha);
  emscripten::function("renderNostrZapAlphaBuffer", &renderNostrZapAlpha);
  emscripten::function("renderClockAlphaBuffer", &renderClockAlpha);
  emscripten::function("renderDebugAlphaBuffer", &renderDebugAlpha);
  emscripten::function("getCodepointMetrics", &getCodepointMetrics);

  emscripten::function("getPanelDimensions", &getPanelDimensions);
  emscripten::function("setRenderOptions", &setRenderOptions);
  emscripten::function("setVerticalDesc", &setVerticalDesc);
}
