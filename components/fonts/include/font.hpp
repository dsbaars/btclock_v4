#pragma once

#include <cstddef>
#include <cstdint>

namespace btclock {

// U+20BF BITCOIN SIGN — callers probe `Font::HasCodepoint` on the digit
// face before painting a ₿ marker from the active bundle.
inline constexpr int kUnicodeBitcoinSign = 0x20BF;

// Per-render rotation of the logical coordinate system over the native
// (portrait, MSB-first 1bpp) framebuffer.
//   k0      — logical (x, y) maps directly to native (x, y).
//   k90Cw   — 90° clockwise: logical (x, y) -> native (native_width-1 - y, x).
//   k180    — 180°: (native_width-1 - x, native_height-1 - y).
//             BTClock Rev B uses this because the 2.13" panels are
//             soldered with the SSD1680 segment driver pointing down.
//   k90Ccw  — 90° counter-clockwise: (y, native_height-1 - x).
enum class Rotation : uint8_t { k0, k90Cw, k180, k90Ccw };

// Panel-framebuffer view used by the renderer. `native_width` is the
// *visible* pixel width (122 for 2.13", 128 for 2.9"); `native_stride`
// is bytes-per-row (16 for both). Logical width/height swap when
// rotation is k90Cw or k90Ccw — use LogicalWidth/LogicalHeight to get
// the right numbers for centring calculations.
struct LandscapeFb {
  uint8_t* native_fb;
  int native_stride;
  int native_width;  // visible, 0..native_width-1 addressable
  int native_height;
  Rotation rotation = Rotation::k180;
};

inline int LogicalWidth(const LandscapeFb& fb) {
  return (fb.rotation == Rotation::k0 || fb.rotation == Rotation::k180)
             ? fb.native_width
             : fb.native_height;
}
inline int LogicalHeight(const LandscapeFb& fb) {
  return (fb.rotation == Rotation::k0 || fb.rotation == Rotation::k180)
             ? fb.native_height
             : fb.native_width;
}

// Clear the buffer to 0x00 (all black) or 0xFF (all white).
void ClearFb(LandscapeFb& fb, bool white);

// Set one pixel in landscape coordinates. Rotation is selected per
// LandscapeFb (Rotation::k0/k90Cw/k180/k90Ccw) and dispatched in
// SetPixelLandscape — see font.cpp lines 137-147 for the per-case
// (lx, ly) -> (nx, ny) mapping. Each case clamps against
// `native_width` / `native_height` (the *visible* extents from
// epd::IEpdPanel::Width/Height — 122×250 for the 2.13"), NOT against the
// 16-byte row stride (128 bits/row on SSD1680). Columns 122..127 in each
// row are not driven as visible ink.
// white=true sets the bit (white), white=false clears it (black).
void SetPixelLandscape(LandscapeFb& fb, int lx, int ly, bool white);

class Font {
 public:
  // ttf_data may be raw TTF/OTF bytes OR a gzip-compressed wrapper
  // (magic 0x1f 0x8b at offset 0). On device the ESP-IDF build embeds
  // .ttf.gz blobs and the ctor decompresses them into PSRAM via miniz
  // from ROM; on host / WASM the bytes are uncompressed and used in
  // place. Raw bytes must remain valid for the Font's lifetime; for
  // the gzip case the Font takes ownership of the decompressed buffer
  // and frees it in the dtor.
  Font(const uint8_t* ttf_data, size_t ttf_size);
  ~Font();
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;
  Font(Font&&) = delete;
  Font& operator=(Font&&) = delete;

  struct GlyphMetrics {
    int w = 0;
    int h = 0;
    int xoff = 0;     // distance from pen x to top-left of bitmap
    int yoff = 0;     // distance from baseline (positive y = below)
    int advance = 0;  // pen advance after this glyph
  };

  GlyphMetrics GetMetrics(int codepoint, float pixel_height) const;

  // True when the loaded face's cmap maps `unicode_codepoint` to a glyph
  // index other than 0 (stb_truetype convention: unmapped → 0 / .notdef).
  // Cheap lookup — safe to call when choosing digit-font symbols at runtime.
  bool HasCodepoint(int unicode_codepoint) const;

  // Rasterize into caller-provided alpha buffer (w*h bytes). No bounds
  // checks — caller uses metrics.w * metrics.h.
  void RenderGlyph(int codepoint, float pixel_height, uint8_t* out, int w,
                   int h) const;

  // Ascent in pixels, post-scale. Use as baseline offset from text top.
  int AscentPx(float pixel_height) const;

  // Vertical bounding box for a *reference* character set. Use this to
  // pick a consistent baseline: rather than centering each string's own
  // bbox (which makes "." float to the middle of the panel), center the
  // reference set once and render all text against that same baseline.
  //
  //   above_baseline: max upward extent across the reference glyphs
  //                   (positive — typical digits for "0123456789" ≈ 0.72*px_h)
  //   below_baseline: max downward extent (positive; digits are 0,
  //                   punctuation like ',' or '_' may be > 0)
  struct ReferenceBox {
    int above_baseline = 0;
    int below_baseline = 0;
  };
  ReferenceBox GetReferenceBox(const char* chars, float pixel_height) const;

 private:
  void* info_ = nullptr;
  const uint8_t* ttf_;
  size_t ttf_size_;
  // Non-null iff the ctor decompressed a gzip blob into a PSRAM buffer
  // that we own. ttf_ then aliases this; freed by the dtor.
  uint8_t* owned_buffer_ = nullptr;
};

// Draws text onto a landscape framebuffer with left-edge-of-text at
// (x, y) where y is the baseline row. One shot — no caching.
// Returns x-advance (total rendered width in pixels).
int DrawTextLandscape(LandscapeFb& fb, int x, int y_baseline, const char* text,
                      const Font& font, float pixel_height, bool white_text);

// Measure width of text at given pixel height, without rendering.
int MeasureTextWidth(const char* text, const Font& font, float pixel_height);

// Measure the *ink* width (bitmap coverage) of text, plus the left
// bearing of the first glyph. Centering on ink width gives visually
// even margins; pen-advance measurement doesn't account for sidebearings.
int MeasureInkWidth(const char* text, const Font& font, float pixel_height,
                    int* left_bearing_out = nullptr);

// Find the largest pixel_height in [min_px, max_px] at which `text`
// rendered in `font` has an ink width ≤ `target_w`. Linear search at
// 0.5 pt granularity — cheap and always finds a fit for any text
// shorter than the panel can hold at ~10 pt.
float FitTextPx(const char* text, const Font& font, float max_px, float min_px,
                int target_w);

// Runtime split-label auto-fit target percentage (100 = historical full
// width target). ScreenManager updates this from settings each render.
void SetGlobalLabelFitPercent(float percent);
float GetGlobalLabelFitPercent();

// Centre `text` horizontally on a `panel_w`-wide panel and place its
// baseline using the vertical bounding box of the `ref_chars` set —
// typically the digits "0123456789". Strings containing only "." or "_"
// then sit at the same baseline as digits do rather than floating to
// the panel centre.
//
// `x_offset_px` / `y_offset_px` shift the result by the given pixels in
// logical coords (positive x = right, positive y = down in logical
// space, which after a Rotation::k180 LandscapeFb becomes physical UP).
// Use x_offset to compensate for per-glyph ink-width asymmetry; use
// y_offset to compensate for panel/case vertical asymmetries where the
// visible area is not symmetric around the addressed gate lines.
void DrawTextCentered(LandscapeFb& fb, int panel_w, int panel_h,
                      const char* text, const char* ref_chars, const Font& font,
                      float pixel_height, bool white_text, int x_offset_px = 0,
                      int y_offset_px = 0);

// Two-line layout with a horizontal separator at the panel vertical
// centre. Each half is centred horizontally; vertically, each half is
// offset from the divider using that string's own glyph extents (union
// bbox) so upper/lower ink is symmetric — `ref_chars` is only used when
// a half is empty (fallback baseline). Typical use:
// DrawSplitText(lfb, W, H, "BLOCK", "HEIGHT",
// "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", antonio, 70.0f, false).
void DrawSplitText(LandscapeFb& fb, int panel_w, int panel_h,
                   const char* top_text, const char* bottom_text,
                   const char* ref_chars, const Font& font, float pixel_height,
                   bool white_text, int fit_target_w_override = -1);

// Render one glyph (by raw codepoint) centred on the panel, sized via
// its own bitmap bbox rather than the digit-ref baseline — MDI icons
// live in the Private Use Area so `kDigitRef` contains none of their
// codepoints and would compute a zero reference box.
void DrawCodepointCentered(LandscapeFb& fb, int panel_w, int panel_h,
                           std::uint32_t codepoint, const Font& font,
                           float pixel_height, bool white_text);

// Mini-markdown renderer for multi-line text with inline-bold lines.
//
// Format:
//   * lines are separated by '\n' ('\r' is ignored)
//   * a line starting with '*' renders in `bold`; all '*' in that line
//     are stripped (matching the existing firmware's behaviour — see
//     src/lib/drivers/epd/epd.cpp:renderText)
//   * other lines render in `regular`
//
// Lines are centred horizontally per-line and the whole block is
// centred vertically on the panel.
void DrawMarkdown(LandscapeFb& fb, int panel_w, int panel_h, const char* text,
                  const Font& regular, const Font& bold, float pixel_height,
                  bool white_text);

// Render a QR code (already generated via qrcodegen) centred on the
// panel's logical rect (panel_w × panel_h). The QR occupies the largest
// square that fits, scaled to integer module size. `module` is 0 for
// white, 1 for black per qrcodegen convention; black modules become
// `!white_bg` in the framebuffer.
//
// `size` is the matrix dimension (e.g. 29 for version 3, 33 for v4).
// `get_module(x, y)` is a callback into qrcodegen that returns 1/0.
void DrawQrCode(LandscapeFb& fb, int panel_w, int panel_h, int size,
                bool (*get_module)(int x, int y, const void* ctx),
                const void* ctx, bool white_bg);

// Embedded font blobs.
//
// On device these are gzipped at configure time (see
// components/fonts/CMakeLists.txt) and embedded as `<Name>.ttf.gz`,
// so the linker exposes `_binary_<Name>_ttf_gz_start` / `_end` symbols
// for each asset. The kFontTtf pointers below alias those compressed
// payloads via asm() labels; Font's ctor sniffs the gzip magic and
// decompresses into a PSRAM-resident buffer at boot.
//
// Under the WASM host build (tools/wasm/build.sh) no such linker magic
// exists, AND no compression is applied — a generator script
// (tools/wasm/gen_font_blobs.py) emits a parallel source file that
// defines real kAntonioTtf + kAntonioTtfSize arrays of *raw* TTF
// bytes. Font's ctor sees no gzip magic and uses the bytes in place.
// We pick one or the other via BTCLOCK_WASM_BUILD.
#ifdef BTCLOCK_WASM_BUILD
extern const uint8_t kAntonioTtf[];
extern const size_t kAntonioTtfSize;
extern const uint8_t kAntonioSemiBoldTtf[];
extern const size_t kAntonioSemiBoldTtfSize;
extern const uint8_t kAntonioBoldTtf[];
extern const size_t kAntonioBoldTtfSize;
extern const uint8_t kOswaldTtf[];
extern const size_t kOswaldTtfSize;
extern const uint8_t kOswaldBoldTtf[];
extern const size_t kOswaldBoldTtfSize;
extern const uint8_t kInterTtf[];
extern const size_t kInterTtfSize;
extern const uint8_t kInterBoldTtf[];
extern const size_t kInterBoldTtfSize;
extern const uint8_t kOpenRundeTtf[];
extern const size_t kOpenRundeTtfSize;
extern const uint8_t kOpenRundeBoldTtf[];
extern const size_t kOpenRundeBoldTtfSize;
extern const uint8_t kRobotoTtf[];
extern const size_t kRobotoTtfSize;
extern const uint8_t kRobotoBoldTtf[];
extern const size_t kRobotoBoldTtfSize;
extern const uint8_t kNotoSansTtf[];
extern const size_t kNotoSansTtfSize;
extern const uint8_t kNotoSansBoldTtf[];
extern const size_t kNotoSansBoldTtfSize;
extern const uint8_t kUbuntuTtf[];
extern const size_t kUbuntuTtfSize;
extern const uint8_t kUbuntuBoldTtf[];
extern const size_t kUbuntuBoldTtfSize;
extern const uint8_t kAzeretTtf[];
extern const size_t kAzeretTtfSize;
extern const uint8_t kSourceSerifTtf[];
extern const size_t kSourceSerifTtfSize;
extern const uint8_t kSourceSerifBoldTtf[];
extern const size_t kSourceSerifBoldTtfSize;
extern const uint8_t kMerriweatherTtf[];
extern const size_t kMerriweatherTtfSize;
extern const uint8_t kMerriweatherBoldTtf[];
extern const size_t kMerriweatherBoldTtfSize;
extern const uint8_t kBitterTtf[];
extern const size_t kBitterTtfSize;
extern const uint8_t kBitterBoldTtf[];
extern const size_t kBitterBoldTtfSize;
extern const uint8_t kAtkinsonTtf[];
extern const size_t kAtkinsonTtfSize;
extern const uint8_t kAtkinsonBoldTtf[];
extern const size_t kAtkinsonBoldTtfSize;
extern const uint8_t kSatoshiSymbolTtf[];
extern const size_t kSatoshiSymbolTtfSize;
extern const uint8_t kMaterialDesignIconsTtf[];
extern const size_t kMaterialDesignIconsTtfSize;
#else
extern const uint8_t kAntonioTtf[] asm("_binary_Antonio_ttf_gz_start");
extern const uint8_t kAntonioTtfEnd[] asm("_binary_Antonio_ttf_gz_end");
extern const uint8_t kAntonioSemiBoldTtf[] asm(
    "_binary_AntonioSemiBold_ttf_gz_start");
extern const uint8_t kAntonioSemiBoldTtfEnd[] asm(
    "_binary_AntonioSemiBold_ttf_gz_end");
extern const uint8_t kAntonioBoldTtf[] asm("_binary_AntonioBold_ttf_gz_start");
extern const uint8_t kAntonioBoldTtfEnd[] asm("_binary_AntonioBold_ttf_gz_end");
extern const uint8_t kOswaldTtf[] asm("_binary_Oswald_ttf_gz_start");
extern const uint8_t kOswaldTtfEnd[] asm("_binary_Oswald_ttf_gz_end");
extern const uint8_t kOswaldBoldTtf[] asm("_binary_OswaldBold_ttf_gz_start");
extern const uint8_t kOswaldBoldTtfEnd[] asm("_binary_OswaldBold_ttf_gz_end");
extern const uint8_t kInterTtf[] asm("_binary_Inter_ttf_gz_start");
extern const uint8_t kInterTtfEnd[] asm("_binary_Inter_ttf_gz_end");
extern const uint8_t kInterBoldTtf[] asm("_binary_InterBold_ttf_gz_start");
extern const uint8_t kInterBoldTtfEnd[] asm("_binary_InterBold_ttf_gz_end");
extern const uint8_t kOpenRundeTtf[] asm("_binary_OpenRunde_ttf_gz_start");
extern const uint8_t kOpenRundeTtfEnd[] asm("_binary_OpenRunde_ttf_gz_end");
extern const uint8_t kOpenRundeBoldTtf[] asm(
    "_binary_OpenRundeBold_ttf_gz_start");
extern const uint8_t kOpenRundeBoldTtfEnd[] asm(
    "_binary_OpenRundeBold_ttf_gz_end");
extern const uint8_t kRobotoTtf[] asm("_binary_Roboto_ttf_gz_start");
extern const uint8_t kRobotoTtfEnd[] asm("_binary_Roboto_ttf_gz_end");
extern const uint8_t kRobotoBoldTtf[] asm("_binary_RobotoBold_ttf_gz_start");
extern const uint8_t kRobotoBoldTtfEnd[] asm("_binary_RobotoBold_ttf_gz_end");
extern const uint8_t kNotoSansTtf[] asm("_binary_NotoSans_ttf_gz_start");
extern const uint8_t kNotoSansTtfEnd[] asm("_binary_NotoSans_ttf_gz_end");
extern const uint8_t kNotoSansBoldTtf[] asm(
    "_binary_NotoSansBold_ttf_gz_start");
extern const uint8_t kNotoSansBoldTtfEnd[] asm(
    "_binary_NotoSansBold_ttf_gz_end");
extern const uint8_t kUbuntuTtf[] asm("_binary_Ubuntu_ttf_gz_start");
extern const uint8_t kUbuntuTtfEnd[] asm("_binary_Ubuntu_ttf_gz_end");
extern const uint8_t kUbuntuBoldTtf[] asm("_binary_UbuntuBold_ttf_gz_start");
extern const uint8_t kUbuntuBoldTtfEnd[] asm("_binary_UbuntuBold_ttf_gz_end");
extern const uint8_t kAzeretTtf[] asm("_binary_Azeret_ttf_gz_start");
extern const uint8_t kAzeretTtfEnd[] asm("_binary_Azeret_ttf_gz_end");
extern const uint8_t kSourceSerifTtf[] asm("_binary_SourceSerif_ttf_gz_start");
extern const uint8_t kSourceSerifTtfEnd[] asm("_binary_SourceSerif_ttf_gz_end");
extern const uint8_t kSourceSerifBoldTtf[] asm(
    "_binary_SourceSerifBold_ttf_gz_start");
extern const uint8_t kSourceSerifBoldTtfEnd[] asm(
    "_binary_SourceSerifBold_ttf_gz_end");
extern const uint8_t kMerriweatherTtf[] asm(
    "_binary_Merriweather_ttf_gz_start");
extern const uint8_t kMerriweatherTtfEnd[] asm(
    "_binary_Merriweather_ttf_gz_end");
extern const uint8_t kMerriweatherBoldTtf[] asm(
    "_binary_MerriweatherBold_ttf_gz_start");
extern const uint8_t kMerriweatherBoldTtfEnd[] asm(
    "_binary_MerriweatherBold_ttf_gz_end");
extern const uint8_t kBitterTtf[] asm("_binary_Bitter_ttf_gz_start");
extern const uint8_t kBitterTtfEnd[] asm("_binary_Bitter_ttf_gz_end");
extern const uint8_t kBitterBoldTtf[] asm("_binary_BitterBold_ttf_gz_start");
extern const uint8_t kBitterBoldTtfEnd[] asm("_binary_BitterBold_ttf_gz_end");
extern const uint8_t kAtkinsonTtf[] asm("_binary_Atkinson_ttf_gz_start");
extern const uint8_t kAtkinsonTtfEnd[] asm("_binary_Atkinson_ttf_gz_end");
extern const uint8_t kAtkinsonBoldTtf[] asm(
    "_binary_AtkinsonBold_ttf_gz_start");
extern const uint8_t kAtkinsonBoldTtfEnd[] asm(
    "_binary_AtkinsonBold_ttf_gz_end");
// Satoshi Symbol — PUA U+E000–U+E00F (16 sat-progress variants of the
// satoshi unit glyph). NOT the same as ₿ (U+20BF, the Bitcoin Sign /
// whole-bitcoin unit). Used only by sat-progress / Moscow-time screens.
extern const uint8_t kSatoshiSymbolTtf[] asm(
    "_binary_SatoshiSymbol_ttf_gz_start");
extern const uint8_t kSatoshiSymbolTtfEnd[] asm(
    "_binary_SatoshiSymbol_ttf_gz_end");
// Material Design Icons — subsetted to just the icons the firmware
// actually paints. Regenerate via tools/fonts/regen_mdi.sh; the
// codepoint constants for each icon live in mdi_codepoints.hpp.
extern const uint8_t kMaterialDesignIconsTtf[] asm(
    "_binary_MaterialDesignIcons_ttf_gz_start");
extern const uint8_t kMaterialDesignIconsTtfEnd[] asm(
    "_binary_MaterialDesignIcons_ttf_gz_end");
#endif

}  // namespace btclock
