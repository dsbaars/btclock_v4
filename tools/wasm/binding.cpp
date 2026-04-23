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
//    renderers from main/screens/*.cpp against an in-memory EpdPanel
//    shim (tools/wasm/wasm_panel.hpp) backed by plain uint8_t arrays,
//    then returns an Array<Uint8Array> of the 7 panel framebuffers.
//    JS unpacks the 1-bpp bytes onto a <canvas>. This is the pixel-
//    accurate preview — same font rasterizer, same paint primitives,
//    same rotation/stride as the physical EPD.
//
// The two families live side by side so the HTML can switch between
// them (text mode is cheap and always works; pixel mode matches the
// device exactly).

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

// Pull in the real screen headers — under BTCLOCK_WASM_BUILD they
// resolve to the shim EpdPanel and pure font.hpp.
#include "font_wasm_aa.hpp"
#include "fonts_app.hpp"
#include "screens/common.hpp"
#include "screens/fee_rate_layout.hpp"
#include "screens/screens.hpp"

namespace btclock {
// Pure helpers the text-mode bindings reuse. Declared in common.hpp
// but listed again here for clarity; linkage comes from common.cpp /
// screen_math.cpp.
}  // namespace btclock

using emscripten::val;

namespace {

// 7 panels — the current REV_B topology. 8-panel variants are a
// follow-up (beads 90q stays in_progress for that).
constexpr std::size_t kPanels = 7;
// Fixed fb_storage size that the renderer templates take by reference.
constexpr std::size_t kFbBytes = 16 * 296;

// The renderer templates declare `fb_storage[N][16*296]` which is
// generous enough for both 2.13" (250h * 16 = 4000 bytes) and 2.9"
// (296h * 16 = 4736 bytes). Only the 2.13" subrange is actually written
// for the current panel kind — we allocate the full 16*296 anyway to
// keep the template signature happy.
using FbStorage = uint8_t[kPanels][kFbBytes];

// One shared set of panels + framebuffers. The AppFonts object carries
// the stb_truetype info_ pointers for each face — safe to cache across
// calls (pure read paths, no internal state mutated per-draw).
struct RenderContext {
  std::array<std::unique_ptr<btclock::EpdPanel>, kPanels> panels;
  FbStorage fbs{};
  btclock::AppFonts fonts;

  RenderContext() {
    for (std::size_t i = 0; i < kPanels; ++i) {
      panels[i] = std::make_unique<btclock::EpdPanel>(
          btclock::PanelKind::k2_13);
    }
  }
};

RenderContext& Ctx() {
  static RenderContext ctx;
  return ctx;
}

// Zero every panel's framebuffer to "all white" (0xFF) so partial-
// refresh paths that don't touch every panel still produce a valid
// result. The device's EpdPanel::Init() also zeroes its shadow before
// the first full refresh; we mirror that here.
void ClearAllPanels() {
  auto& ctx = Ctx();
  for (std::size_t i = 0; i < kPanels; ++i) {
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
// 122 * 250 = 30,500 bytes; 7 panels = ~213 KB per render call. We
// allocate these per-render (std::vector on the heap) and clone into
// JS Uint8Arrays before freeing.
struct AlphaBuffers {
  int w;
  int h;
  std::array<std::vector<uint8_t>, kPanels> bufs;

  AlphaBuffers(int w_in, int h_in) : w(w_in), h(h_in) {
    const std::size_t bytes = static_cast<std::size_t>(w) *
                              static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < kPanels; ++i) {
      bufs[i].assign(bytes, 0);
    }
  }
};

val AlphaBuffersToVal(const AlphaBuffers& ab) {
  val out = val::array();
  for (std::size_t i = 0; i < kPanels; ++i) {
    const std::size_t bytes = ab.bufs[i].size();
    val view = val(emscripten::typed_memory_view(bytes, ab.bufs[i].data()));
    // Clone so the JS side owns stable bytes past this call.
    val clone = val::global("Uint8Array").new_(view);
    out.set(i, clone);
  }
  return out;
}

// Arm the alpha sidechannel at ctx.fbs[0..kPanels-1], keyed on each
// panel's fb pointer. Caller must Clear() the sidechannel (or call this
// with a fresh AlphaBuffers) before every render to avoid residue.
void ArmAlphaSidechannel(AlphaBuffers& ab) {
  auto& ctx = Ctx();
  const uint8_t* fb_ptrs[kPanels];
  uint8_t* alpha_ptrs[kPanels];
  for (std::size_t i = 0; i < kPanels; ++i) {
    fb_ptrs[i] = ctx.fbs[i];
    alpha_ptrs[i] = ab.bufs[i].data();
  }
  btclock::wasm_aa::SetPanelTargets(kPanels, fb_ptrs, alpha_ptrs,
                                    ab.w, ab.h, /*stride=*/ab.w);
}

// Convert the shared kPanels * kFbBytes framebuffer array into a JS
// Array<Uint8Array> where each element spans exactly the bytes the
// panel actually uses (stride * height = 16 * 250 = 4000 for 2.13").
//
// typed_memory_view returns a Uint8Array view onto WASM linear memory
// that stays valid across calls (until WASM memory grows and the view
// is reset by emscripten itself). We clone into a fresh Uint8Array so
// the JS side owns stable bytes even across subsequent renders.
val FrameBuffersToVal() {
  auto& ctx = Ctx();
  const int stride = btclock::EpdPanel::kStride;
  const int height = ctx.panels[0]->Height();
  const std::size_t used = static_cast<std::size_t>(stride) * height;

  val out = val::array();
  for (std::size_t i = 0; i < kPanels; ++i) {
    val view = val(emscripten::typed_memory_view(used, ctx.fbs[i]));
    // Copy into a new Uint8Array that the JS caller owns. For 4000
    // bytes × 7 panels this is ~28 KB per render call — negligible.
    val clone = val::global("Uint8Array").new_(view);
    out.set(i, clone);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Text-mode bindings (Phase 1 — pre-existing semantics).
// ---------------------------------------------------------------------------

namespace {

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
  const uint32_t h = block_height < 0 ? 0u
                                      : static_cast<uint32_t>(block_height);
  // Label-drop parity: 7-digit heights (>=1_000_000 on a 7-panel board)
  // paint every panel as a digit; see BlockHeightDropsLabel.
  if (btclock::BlockHeightDropsLabel(h, kPanels)) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(h));
    val a = val::array();
    const std::size_t len = std::strlen(buf);
    const std::size_t skip = len > kPanels ? len - kPanels : 0;
    for (std::size_t i = 0; i < kPanels; ++i) {
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
  const char* symbol_utf8 = btclock::CurrencySymbolUtf8(currency);
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
  const bool moscow =
      currency == "USD" && sats > 0 && sats < 100000;
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
val parseHalvingCountdown(int block_height, bool /*bigchars_unused*/) {
  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  if (block_height >= 0) {
    const uint32_t rem = btclock::HalvingCountdown(
        static_cast<uint32_t>(block_height));
    btclock::FormatDigits(rem, d.data(), 6);
  }
  return DigitsToArray("HAL/VING", d);
}

val parseMarketCap(int block_height, int price_int, std::string currency,
                   bool /*bigchars_unused*/) {
  const std::string label = currency + "/MCAP";
  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  if (block_height >= 0 && price_int >= 0) {
    const uint64_t cap = btclock::MarketCap(
        static_cast<uint32_t>(price_int),
        static_cast<uint32_t>(block_height));
    // 6 slots on the preview; leading digits are truncated on overflow.
    char buf[24];
    btclock::FormatDigits64(cap, buf, 6);
    for (int i = 0; i < 6; ++i) d[i] = buf[i];
  }
  return DigitsToArray(label.c_str(), d);
}

val parseBitcoinSupply(int block_height, bool /*bigchars_unused*/,
                       bool /*percent_unused*/) {
  std::array<char, 6> d{' ', ' ', ' ', ' ', ' ', ' '};
  if (block_height >= 0) {
    const uint64_t supply = btclock::SupplyAtBlock(
        static_cast<uint32_t>(block_height));
    char buf[24];
    btclock::FormatDigits64(supply, buf, 6);
    for (int i = 0; i < 6; ++i) d[i] = buf[i];
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
// of the 7 resulting panel buffers.
// ---------------------------------------------------------------------------

namespace {

val renderBlockHeight(int block_height) {
  auto& ctx = Ctx();
  ClearAllPanels();
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  btclock::RenderBlockHeightScreen<kPanels>(
      ctx.panels, ctx.fbs, ctx.fonts, bh, /*prev_height=*/0);
  return FrameBuffersToVal();
}

val renderPriceData(int price_int, std::string currency) {
  auto& ctx = Ctx();
  ClearAllPanels();
  char price_buf[24];
  std::snprintf(price_buf, sizeof(price_buf), "%d",
                price_int < 0 ? 0 : price_int);
  const char* symbol_utf8 = btclock::CurrencySymbolUtf8(currency);
  btclock::RenderBtcPriceScreen<kPanels>(
      ctx.panels, ctx.fbs, ctx.fonts, currency, price_buf,
      /*prev_price=*/"", symbol_utf8);
  return FrameBuffersToVal();
}

val renderSatsPerCurrency(int price_int, std::string currency,
                          bool /*with_sats_symbol_unused*/) {
  auto& ctx = Ctx();
  ClearAllPanels();
  char price_buf[24];
  std::snprintf(price_buf, sizeof(price_buf), "%d",
                price_int < 0 ? 0 : price_int);
  btclock::RenderMoscowTimeScreen<kPanels>(
      ctx.panels, ctx.fbs, ctx.fonts, currency, price_buf,
      /*prev_price=*/"", btclock::kSatsVariantDefault);
  return FrameBuffersToVal();
}

val renderBlockFees(int fee_sats_vb) {
  auto& ctx = Ctx();
  ClearAllPanels();
  btclock::RenderFeeRateScreen<kPanels>(
      ctx.panels, ctx.fbs, ctx.fonts,
      /*fee_sats_vb=*/fee_sats_vb < 0 ? 0 : fee_sats_vb,
      /*prev_fee_sats_vb=*/-1);
  return FrameBuffersToVal();
}

val renderHalvingCountdown(int block_height, bool /*bigchars_unused*/) {
  auto& ctx = Ctx();
  ClearAllPanels();
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  btclock::RenderHalvingScreen<kPanels>(
      ctx.panels, ctx.fbs, ctx.fonts, bh, /*prev_height=*/0);
  return FrameBuffersToVal();
}

val renderMarketCap(int block_height, int price_int, std::string currency,
                    bool /*bigchars_unused*/) {
  auto& ctx = Ctx();
  ClearAllPanels();
  char price_buf[24];
  std::snprintf(price_buf, sizeof(price_buf), "%d",
                price_int < 0 ? 0 : price_int);
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  btclock::RenderMarketCapScreen<kPanels>(
      ctx.panels, ctx.fbs, ctx.fonts, currency, price_buf, bh,
      /*prev_price=*/"", /*prev_height=*/0);
  return FrameBuffersToVal();
}

val renderBitcoinSupply(int block_height, bool /*bigchars_unused*/,
                        bool /*percent_unused*/) {
  auto& ctx = Ctx();
  ClearAllPanels();
  const uint32_t bh =
      block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
  btclock::RenderBitcoinSupplyScreen<kPanels>(
      ctx.panels, ctx.fbs, ctx.fonts, bh, /*prev_height=*/0);
  return FrameBuffersToVal();
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
  ClearAllPanels();
  const int w = ctx.panels[0]->Width();
  const int h = ctx.panels[0]->Height();
  AlphaBuffers ab(w, h);
  ArmAlphaSidechannel(ab);
  run_render();
  btclock::wasm_aa::Clear();
  return AlphaBuffersToVal(ab);
}

val renderBlockHeightAlpha(int block_height) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    btclock::RenderBlockHeightScreen<kPanels>(
        ctx.panels, ctx.fbs, ctx.fonts, bh, /*prev_height=*/0);
  });
}

val renderPriceDataAlpha(int price_int, std::string currency) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    char price_buf[24];
    std::snprintf(price_buf, sizeof(price_buf), "%d",
                  price_int < 0 ? 0 : price_int);
    const char* symbol_utf8 = btclock::CurrencySymbolUtf8(currency);
    btclock::RenderBtcPriceScreen<kPanels>(
        ctx.panels, ctx.fbs, ctx.fonts, currency, price_buf,
        /*prev_price=*/"", symbol_utf8);
  });
}

val renderSatsPerCurrencyAlpha(int price_int, std::string currency,
                               bool /*with_sats_symbol_unused*/) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    char price_buf[24];
    std::snprintf(price_buf, sizeof(price_buf), "%d",
                  price_int < 0 ? 0 : price_int);
    btclock::RenderMoscowTimeScreen<kPanels>(
        ctx.panels, ctx.fbs, ctx.fonts, currency, price_buf,
        /*prev_price=*/"", btclock::kSatsVariantDefault);
  });
}

val renderBlockFeesAlpha(int fee_sats_vb) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    btclock::RenderFeeRateScreen<kPanels>(
        ctx.panels, ctx.fbs, ctx.fonts,
        /*fee_sats_vb=*/fee_sats_vb < 0 ? 0 : fee_sats_vb,
        /*prev_fee_sats_vb=*/-1);
  });
}

val renderHalvingCountdownAlpha(int block_height, bool /*bigchars_unused*/) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    btclock::RenderHalvingScreen<kPanels>(
        ctx.panels, ctx.fbs, ctx.fonts, bh, /*prev_height=*/0);
  });
}

val renderMarketCapAlpha(int block_height, int price_int,
                         std::string currency,
                         bool /*bigchars_unused*/) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    char price_buf[24];
    std::snprintf(price_buf, sizeof(price_buf), "%d",
                  price_int < 0 ? 0 : price_int);
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    btclock::RenderMarketCapScreen<kPanels>(
        ctx.panels, ctx.fbs, ctx.fonts, currency, price_buf, bh,
        /*prev_price=*/"", /*prev_height=*/0);
  });
}

val renderBitcoinSupplyAlpha(int block_height, bool /*bigchars_unused*/,
                             bool /*percent_unused*/) {
  return RunAlphaRender([&]() {
    auto& ctx = Ctx();
    const uint32_t bh =
        block_height < 0 ? 0 : static_cast<uint32_t>(block_height);
    btclock::RenderBitcoinSupplyScreen<kPanels>(
        ctx.panels, ctx.fbs, ctx.fonts, bh, /*prev_height=*/0);
  });
}

// Panel dimension metadata — JS needs this to set the <canvas> size
// and drive the 1-bpp iteration. Rotation is fixed at k180 to match
// the REV_B solder orientation; native is 122x250 for 2.13" panels.
val getPanelDimensions() {
  auto& ctx = Ctx();
  val o = val::object();
  o.set("width", ctx.panels[0]->Width());
  o.set("height", ctx.panels[0]->Height());
  o.set("stride", btclock::EpdPanel::kStride);
  // 0 = k0, 1 = k90Cw, 2 = k180, 3 = k90Ccw (matches Rotation enum).
  o.set("rotation", 2);
  o.set("panels", static_cast<int>(kPanels));
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
  emscripten::function("renderPriceDataFrameBuffer", &renderPriceData);
  emscripten::function("renderSatsPerCurrencyFrameBuffer",
                       &renderSatsPerCurrency);
  emscripten::function("renderBlockFeesFrameBuffer", &renderBlockFees);
  emscripten::function("renderHalvingCountdownFrameBuffer",
                       &renderHalvingCountdown);
  emscripten::function("renderMarketCapFrameBuffer", &renderMarketCap);
  emscripten::function("renderBitcoinSupplyFrameBuffer",
                       &renderBitcoinSupply);

  // AA mode (grayscale alpha, logical-coord, preview-only — one byte
  // per pixel, 0=white/no-ink..255=full-black-ink).
  emscripten::function("renderBlockHeightAlphaBuffer",
                       &renderBlockHeightAlpha);
  emscripten::function("renderPriceDataAlphaBuffer", &renderPriceDataAlpha);
  emscripten::function("renderSatsPerCurrencyAlphaBuffer",
                       &renderSatsPerCurrencyAlpha);
  emscripten::function("renderBlockFeesAlphaBuffer", &renderBlockFeesAlpha);
  emscripten::function("renderHalvingCountdownAlphaBuffer",
                       &renderHalvingCountdownAlpha);
  emscripten::function("renderMarketCapAlphaBuffer", &renderMarketCapAlpha);
  emscripten::function("renderBitcoinSupplyAlphaBuffer",
                       &renderBitcoinSupplyAlpha);

  emscripten::function("getPanelDimensions", &getPanelDimensions);
}
