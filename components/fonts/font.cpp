#include "font.hpp"

#include <cstring>
#include <new>
#include <string>
#include <vector>

// Under the emscripten host build (tools/wasm/build.sh) the ESP-IDF
// heap_caps + logging headers aren't available. Swap in stdlib
// malloc and stubbed log macros — the paint primitives below are
// identical either way; only the glyph-scratch allocator changes
// SPIRAM -> malloc.
#ifdef BTCLOCK_WASM_BUILD
#include <cstdio>
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
static inline void* heap_caps_malloc(size_t n, int /*caps*/) {
  return std::malloc(n);
}
#define ESP_LOGE(tag, fmt, ...) (void)(tag)
#define ESP_LOGI(tag, fmt, ...) (void)(tag)
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif
#include "fit_text_px.hpp"
#include "landscape_rotation.hpp"
#include "markdown_parse.hpp"
#include "stb_truetype.h"

// WASM-only alpha sidechannel. Under BTCLOCK_WASM_BUILD the binding
// layer (tools/wasm/binding.cpp) registers one alpha buffer per panel
// (keyed on the LandscapeFb's native_fb pointer) and the paint
// primitives here mirror each write into the matching buffer. On device
// the header isn't on the include path (it lives under tools/wasm/),
// so we drop in equivalent zero-cost inline stubs that compile away.
// Full semantics: see tools/wasm/font_wasm_aa.hpp.
#ifdef BTCLOCK_WASM_BUILD
#include "font_wasm_aa.hpp"
#else
namespace btclock {
namespace wasm_aa {
inline void SetPanelTargets(std::size_t, const uint8_t* const*, uint8_t* const*,
                            int, int, int) {}
inline void Clear() {}
inline void WritePixel(const uint8_t*, int, int, uint8_t, bool) {}
inline void FillRect(const uint8_t*, int, int, int, int, uint8_t) {}
inline bool SetPixelSuppressed() {
  return false;
}
class SetPixelSuppressScope {
 public:
  SetPixelSuppressScope() = default;
  ~SetPixelSuppressScope() = default;
  SetPixelSuppressScope(const SetPixelSuppressScope&) = delete;
  SetPixelSuppressScope& operator=(const SetPixelSuppressScope&) = delete;
};
}  // namespace wasm_aa
}  // namespace btclock
#endif

namespace btclock {
namespace {
constexpr const char* kTag = "fonts";

// --- UTF-8 decoding ---
//
// Text passed around is UTF-8 (narrow string literals default to UTF-8 on
// every compiler this project uses). Decode a single codepoint and
// advance `p`. On malformed input, advances one byte and returns U+FFFD.
int NextCodepoint(const char*& p) {
  const uint8_t c = static_cast<uint8_t>(*p);
  if (c < 0x80) {
    ++p;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && (static_cast<uint8_t>(p[1]) & 0xC0) == 0x80) {
    const int cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(p[1]) & 0x3F);
    p += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0 && (static_cast<uint8_t>(p[1]) & 0xC0) == 0x80 &&
      (static_cast<uint8_t>(p[2]) & 0xC0) == 0x80) {
    const int cp = ((c & 0x0F) << 12) |
                   ((static_cast<uint8_t>(p[1]) & 0x3F) << 6) |
                   (static_cast<uint8_t>(p[2]) & 0x3F);
    p += 3;
    return cp;
  }
  if ((c & 0xF8) == 0xF0 && (static_cast<uint8_t>(p[1]) & 0xC0) == 0x80 &&
      (static_cast<uint8_t>(p[2]) & 0xC0) == 0x80 &&
      (static_cast<uint8_t>(p[3]) & 0xC0) == 0x80) {
    const int cp = ((c & 0x07) << 18) |
                   ((static_cast<uint8_t>(p[1]) & 0x3F) << 12) |
                   ((static_cast<uint8_t>(p[2]) & 0x3F) << 6) |
                   (static_cast<uint8_t>(p[3]) & 0x3F);
    p += 4;
    return cp;
  }
  ++p;
  return 0xFFFD;
}

}  // namespace

// --- Landscape framebuffer helpers ---

void ClearFb(LandscapeFb& fb, bool white) {
  std::memset(fb.native_fb, white ? 0xFF : 0x00,
              static_cast<size_t>(fb.native_stride) *
                  static_cast<size_t>(fb.native_height));
  // Mirror into the WASM AA buffer (if armed). Panel key = native_fb
  // pointer — unique per panel because each panel's LandscapeFb points
  // into a distinct ctx.fbs[i] slice. The buffer is sized to the
  // panel's NATIVE dimensions (122×250 on a 2.13") regardless of
  // fb.rotation, so a uniform clear must span those native bounds —
  // not LogicalWidth/Height, which swap on k90Cw label panels and
  // would leave half the buffer untouched.
  wasm_aa::FillRect(fb.native_fb, 0, 0, fb.native_width, fb.native_height,
                    white ? 0 : 255);
}

// Map a logical (lx, ly) through the rotation into native (nx, ny) and
// write the bit.
void SetPixelLandscape(LandscapeFb& fb, int lx, int ly, bool white) {
  const int nw = fb.native_width;
  const int nh = fb.native_height;
  // Bounds-check in logical coords first so a 250x128 landscape target
  // and a 128x250 portrait target behave identically.
  if (lx < 0 || lx >= LogicalWidth(fb)) return;
  if (ly < 0 || ly >= LogicalHeight(fb)) return;

  // Mirror solid-ink pixels into the WASM AA buffer (no-op on device).
  // Callers here are the fill primitives (FillRect/FillRoundRect corners,
  // QR modules) — always full coverage. `white` has the same meaning it
  // has in the 1-bpp layer: true=white/no-ink, false=black/ink. DrawText
  // wraps its inner SetPixelLandscape calls in a SetPixelSuppressScope so
  // the raw pre-threshold alpha it already recorded isn't clobbered back
  // to 255 by the post-threshold fill.
  //
  // Apply the same logical→user-upright transform as the framebuffer
  // path so a k90Cw label panel's pixels land at the panel-rotated
  // coords, not at their pre-rotation logical position (which would
  // exceed native_width and clip — bd btclock_v4-m67).
  if (!wasm_aa::SetPixelSuppressed()) {
    const NativeXY u = RotateLogicalToUserUpright(lx, ly, nw, nh, fb.rotation);
    wasm_aa::WritePixel(fb.native_fb, u.x, u.y, white ? 0 : 255,
                        /*white_text=*/false);
  }

  const NativeXY n = RotateLogicalToNative(lx, ly, nw, nh, fb.rotation);
  const int nx = n.x;
  const int ny = n.y;
  const int byte_idx = ny * fb.native_stride + (nx >> 3);
  const uint8_t bit = static_cast<uint8_t>(1U << (7 - (nx & 7)));
  if (white) {
    fb.native_fb[byte_idx] |= bit;
  } else {
    fb.native_fb[byte_idx] &= static_cast<uint8_t>(~bit);
  }
}

// --- Font ---

Font::Font(const uint8_t* ttf_data, size_t ttf_size)
    : ttf_(ttf_data), ttf_size_(ttf_size) {
  auto* info = new stbtt_fontinfo();
  if (stbtt_InitFont(info, ttf_data,
                     stbtt_GetFontOffsetForIndex(ttf_data, 0)) == 0) {
    ESP_LOGE(kTag, "stbtt_InitFont failed");
    delete info;
    return;
  }
  info_ = info;
  ESP_LOGI(kTag, "font loaded %u bytes", static_cast<unsigned>(ttf_size));
}

Font::GlyphMetrics Font::GetMetrics(int codepoint, float pixel_height) const {
  GlyphMetrics m = {};
  if (info_ == nullptr) return m;
  auto* info = static_cast<stbtt_fontinfo*>(info_);
  const float scale = stbtt_ScaleForPixelHeight(info, pixel_height);
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  stbtt_GetCodepointBitmapBox(info, codepoint, scale, scale, &x0, &y0, &x1,
                              &y1);
  int advance = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(info, codepoint, &advance, &lsb);
  m.w = x1 - x0;
  m.h = y1 - y0;
  m.xoff = x0;
  m.yoff = y0;
  m.advance = static_cast<int>(advance * scale);
  return m;
}

void Font::RenderGlyph(int codepoint, float pixel_height, uint8_t* out, int w,
                       int h) const {
  if (info_ == nullptr) return;
  auto* info = static_cast<stbtt_fontinfo*>(info_);
  const float scale = stbtt_ScaleForPixelHeight(info, pixel_height);
  stbtt_MakeCodepointBitmap(info, out, w, h, w, scale, scale, codepoint);
}

int Font::AscentPx(float pixel_height) const {
  if (info_ == nullptr) return 0;
  auto* info = static_cast<stbtt_fontinfo*>(info_);
  const float scale = stbtt_ScaleForPixelHeight(info, pixel_height);
  int ascent = 0, descent = 0, line_gap = 0;
  stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
  return static_cast<int>(ascent * scale);
}

Font::ReferenceBox Font::GetReferenceBox(const char* chars,
                                         float pixel_height) const {
  ReferenceBox rb = {};
  if (chars == nullptr) return rb;
  const char* p = chars;
  while (*p) {
    const int cp = NextCodepoint(p);
    const auto m = GetMetrics(cp, pixel_height);
    if (m.w <= 0 || m.h <= 0) continue;
    const int above = -m.yoff;       // positive if glyph extends above baseline
    const int below = m.yoff + m.h;  // positive if glyph extends below baseline
    if (above > rb.above_baseline) rb.above_baseline = above;
    if (below > rb.below_baseline) rb.below_baseline = below;
  }
  return rb;
}

// --- Text rendering ---

namespace {

// Scratch buffer for the biggest glyph we expect to rasterize. Lives in
// PSRAM so it doesn't eat internal RAM.
constexpr size_t kMaxGlyphBytes = 200 * 200;  // 40 KB
uint8_t* glyph_buf() {
  static uint8_t* buf = nullptr;
  if (buf == nullptr) {
    buf = static_cast<uint8_t*>(
        heap_caps_malloc(kMaxGlyphBytes, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
      ESP_LOGE(kTag, "glyph PSRAM alloc failed, falling back to internal");
      buf = static_cast<uint8_t*>(
          heap_caps_malloc(kMaxGlyphBytes, MALLOC_CAP_8BIT));
    }
  }
  return buf;
}

}  // namespace

int DrawTextLandscape(LandscapeFb& fb, int x, int y_baseline, const char* text,
                      const Font& font, float pixel_height, bool white_text) {
  int pen_x = x;
  uint8_t* buf = glyph_buf();
  const char* p = text;
  while (*p) {
    const int cp = NextCodepoint(p);
    const auto m = font.GetMetrics(cp, pixel_height);
    if (m.w > 0 && m.h > 0 && buf != nullptr) {
      const size_t need = static_cast<size_t>(m.w) * m.h;
      if (need <= kMaxGlyphBytes) {
        font.RenderGlyph(cp, pixel_height, buf, m.w, m.h);
        const int top_left_x = pen_x + m.xoff;
        const int top_left_y = y_baseline + m.yoff;
        // Suppress the secondary wasm_aa tap inside SetPixelLandscape —
        // we feed the real grayscale alpha below (pre-threshold), which
        // carries more information than the thresholded fill would.
        // Scope no-ops on device.
        wasm_aa::SetPixelSuppressScope aa_guard;
        for (int dy = 0; dy < m.h; ++dy) {
          for (int dx = 0; dx < m.w; ++dx) {
            const uint8_t a = buf[dy * m.w + dx];
            // Feed the raw grayscale alpha into the WASM AA sidechannel
            // (no-op on device). This is what gives the preview canvas
            // smooth edges even though the physical panel still gets
            // the thresholded 1bpp pixel below.
            //
            // Apply the same logical→user-upright transform the
            // framebuffer write below uses so a k90Cw label panel's
            // glyph alpha lands at the panel-rotated coords (otherwise
            // text reads horizontally and clips past native_width — bd
            // btclock_v4-m67).
            const int lx = top_left_x + dx;
            const int ly = top_left_y + dy;
            const NativeXY u = RotateLogicalToUserUpright(
                lx, ly, fb.native_width, fb.native_height, fb.rotation);
            wasm_aa::WritePixel(fb.native_fb, u.x, u.y, a, white_text);
            if (a >= 128) {
              SetPixelLandscape(fb, lx, ly, white_text);
            }
          }
        }
      }
    }
    pen_x += m.advance;
  }
  return pen_x - x;
}

int MeasureTextWidth(const char* text, const Font& font, float pixel_height) {
  int total = 0;
  const char* p = text;
  while (*p) {
    const int cp = NextCodepoint(p);
    total += font.GetMetrics(cp, pixel_height).advance;
  }
  return total;
}

// Trim the leading xoff of the first glyph and trailing (advance - w - xoff)
// of the last, so we measure the *ink* width (bitmap coverage) rather than
// the pen-advance width. Centering with advance leaves uneven visual
// margins; centering with ink centers the visible strokes.
float FitTextPx(const char* text, const Font& font, float max_px, float min_px,
                int target_w) {
  return FitTextPxBy(
      [&](float px) { return MeasureInkWidth(text, font, px, nullptr); },
      max_px, min_px, target_w);
}

int MeasureInkWidth(const char* text, const Font& font, float pixel_height,
                    int* left_bearing_out) {
  int ink_left = 0;
  int ink_right = 0;
  int pen = 0;
  bool first = true;
  const char* p = text;
  while (*p) {
    const int cp = NextCodepoint(p);
    const auto m = font.GetMetrics(cp, pixel_height);
    if (m.w > 0 && m.h > 0) {
      const int gl = pen + m.xoff;
      const int gr = gl + m.w;
      if (first) {
        ink_left = gl;
        first = false;
      }
      if (gr > ink_right) ink_right = gr;
    }
    pen += m.advance;
  }
  if (first) {
    if (left_bearing_out) *left_bearing_out = 0;
    return 0;
  }
  if (left_bearing_out) *left_bearing_out = ink_left;
  return ink_right - ink_left;
}

namespace {

// Centre one line horizontally (ink-width based) with a baseline derived
// from `ref_box` sitting inside a region [y_top, y_top + region_h].
void DrawLineCentered(LandscapeFb& fb, int panel_w, int y_top, int region_h,
                      const char* text, const Font::ReferenceBox& ref_box,
                      const Font& font, float pixel_height, bool white_text,
                      int x_offset_px = 0, int y_offset_px = 0) {
  int left_bearing = 0;
  const int ink_w = MeasureInkWidth(text, font, pixel_height, &left_bearing);
  const int x_origin = (panel_w - ink_w) / 2 - left_bearing + x_offset_px;

  const int ref_h = ref_box.above_baseline + ref_box.below_baseline;
  const int ref_top = y_top + (region_h - ref_h) / 2;
  const int y_baseline = ref_top + ref_box.above_baseline + y_offset_px;

  DrawTextLandscape(fb, x_origin, y_baseline, text, font, pixel_height,
                    white_text);
}

void FillRect(LandscapeFb& fb, int x, int y, int w, int h, bool white) {
  for (int dy = 0; dy < h; ++dy) {
    for (int dx = 0; dx < w; ++dx) {
      SetPixelLandscape(fb, x + dx, y + dy, white);
    }
  }
}

// Pill-ended horizontal bar: corners rounded with the given radius.
// Radius clamped to min(w/2, h/2).
void FillRoundRect(LandscapeFb& fb, int x, int y, int w, int h, int r,
                   bool white) {
  if (r < 0) r = 0;
  if (r > h / 2) r = h / 2;
  if (r > w / 2) r = w / 2;
  if (r == 0) {
    FillRect(fb, x, y, w, h, white);
    return;
  }
  // Middle strip: full-height rectangle between the two rounded ends.
  FillRect(fb, x + r, y, w - 2 * r, h, white);
  // Side strips (between the corner arcs): full-width minus corners.
  FillRect(fb, x, y + r, r, h - 2 * r, white);
  FillRect(fb, x + w - r, y + r, r, h - 2 * r, white);
  // Four corner arcs.
  for (int dy = 0; dy <= r; ++dy) {
    for (int dx = 0; dx <= r; ++dx) {
      if (dx * dx + dy * dy <= r * r) {
        SetPixelLandscape(fb, x + r - dx, y + r - dy, white);          // TL
        SetPixelLandscape(fb, x + w - r - 1 + dx, y + r - dy, white);  // TR
        SetPixelLandscape(fb, x + r - dx, y + h - r - 1 + dy, white);  // BL
        SetPixelLandscape(fb, x + w - r - 1 + dx, y + h - r - 1 + dy,
                          white);  // BR
      }
    }
  }
}

}  // namespace

void DrawTextCentered(LandscapeFb& fb, int panel_w, int panel_h,
                      const char* text, const char* ref_chars, const Font& font,
                      float pixel_height, bool white_text, int x_offset_px,
                      int y_offset_px) {
  const auto rb = font.GetReferenceBox(ref_chars, pixel_height);
  DrawLineCentered(fb, panel_w, /*y_top=*/0, /*region_h=*/panel_h, text, rb,
                   font, pixel_height, white_text, x_offset_px, y_offset_px);
}

void DrawCodepointCentered(LandscapeFb& fb, int panel_w, int panel_h,
                           std::uint32_t codepoint, const Font& font,
                           float pixel_height, bool white_text) {
  // MDI glyphs live in U+F0000..U+FFFFF (PUA Plane 15); DrawTextLandscape
  // walks a UTF-8 null-terminated string, so encode the codepoint here
  // rather than plumbing a codepoint path through the text pipeline.
  // All MDI icons land in the 4-byte UTF-8 range.
  char buf[5] = {0};
  std::size_t n = 0;
  if (codepoint < 0x80u) {
    buf[n++] = static_cast<char>(codepoint);
  } else if (codepoint < 0x800u) {
    buf[n++] = static_cast<char>(0xC0u | (codepoint >> 6));
    buf[n++] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
  } else if (codepoint < 0x10000u) {
    buf[n++] = static_cast<char>(0xE0u | (codepoint >> 12));
    buf[n++] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
    buf[n++] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
  } else {
    buf[n++] = static_cast<char>(0xF0u | (codepoint >> 18));
    buf[n++] = static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
    buf[n++] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
    buf[n++] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
  }
  buf[n] = '\0';

  const auto m = font.GetMetrics(static_cast<int>(codepoint), pixel_height);
  if (m.w <= 0 || m.h <= 0) return;

  // Centre on the *rasterized* ink, not stbtt's bbox. The bbox returned
  // by GetCodepointBitmapBox tightly hugs ink for type-design fonts but
  // some MDI glyphs (notably mdi-lightning-bolt) carry whitespace on
  // one side of the bbox — bbox-centering then shifts the visible ink
  // by half the asymmetry. Rasterize once into a stack-bounded scratch
  // buffer, find the leftmost/topmost/rightmost/bottommost cells whose
  // alpha clears the same >=128 threshold SetPixelLandscape uses, and
  // centre on that.
  const std::size_t need = static_cast<std::size_t>(m.w) * m.h;
  if (need > kMaxGlyphBytes) {
    // Fall back to bbox centering — the scratch buffer is shared with
    // DrawTextLandscape and we don't double-allocate for one frame's
    // edge case.
    const int x_origin = (panel_w - m.w) / 2 - m.xoff;
    const int top_y = (panel_h - m.h) / 2;
    const int y_baseline = top_y - m.yoff;
    DrawTextLandscape(fb, x_origin, y_baseline, buf, font, pixel_height,
                      white_text);
    return;
  }
  uint8_t* scratch = glyph_buf();
  if (scratch == nullptr) return;
  font.RenderGlyph(static_cast<int>(codepoint), pixel_height, scratch, m.w,
                   m.h);
  int ix_min = m.w, ix_max = -1, iy_min = m.h, iy_max = -1;
  for (int y = 0; y < m.h; ++y) {
    const uint8_t* row = scratch + y * m.w;
    for (int x = 0; x < m.w; ++x) {
      if (row[x] < 128) continue;
      if (x < ix_min) ix_min = x;
      if (x > ix_max) ix_max = x;
      if (y < iy_min) iy_min = y;
      if (y > iy_max) iy_max = y;
    }
  }
  if (ix_max < 0) return;  // entirely sub-threshold

  const int ink_w = ix_max - ix_min + 1;
  const int ink_h = iy_max - iy_min + 1;
  // x_origin is the pen-x DrawTextLandscape interprets; it draws ink at
  // pen_x + m.xoff + ix_min for the leftmost visible column. Solve for
  // x_origin so that lands at (panel_w - ink_w)/2.
  const int x_origin = (panel_w - ink_w) / 2 - (m.xoff + ix_min);
  const int top_y = (panel_h - ink_h) / 2;
  const int y_baseline = top_y - (m.yoff + iy_min);
  DrawTextLandscape(fb, x_origin, y_baseline, buf, font, pixel_height,
                    white_text);
}

void DrawQrCode(LandscapeFb& fb, int panel_w, int panel_h, int size,
                bool (*get_module)(int, int, const void*), const void* ctx,
                bool white_bg) {
  if (size <= 0) return;
  // Pick the biggest integer module size that fits both dimensions.
  const int max_dim = panel_w < panel_h ? panel_w : panel_h;
  int mod_px = max_dim / size;
  if (mod_px <= 0) mod_px = 1;
  const int qr_px = size * mod_px;
  const int x0 = (panel_w - qr_px) / 2;
  const int y0 = (panel_h - qr_px) / 2;

  for (int my = 0; my < size; ++my) {
    for (int mx = 0; mx < size; ++mx) {
      const bool dark = get_module(mx, my, ctx);
      const bool white = white_bg ? !dark : dark;
      for (int dy = 0; dy < mod_px; ++dy) {
        for (int dx = 0; dx < mod_px; ++dx) {
          SetPixelLandscape(fb, x0 + mx * mod_px + dx, y0 + my * mod_px + dy,
                            white);
        }
      }
    }
  }
}

void DrawMarkdown(LandscapeFb& fb, int panel_w, int panel_h, const char* text,
                  const Font& regular, const Font& bold, float pixel_height,
                  bool white_text) {
  const auto lines = ParseMarkdownLines(text);

  // Auto-fit: shrink pixel_height uniformly so the widest line fits in
  // `panel_w` minus a small side gutter. Without this, a runtime-bound
  // string like the provisioning panel's "BTClock-XXXX" SSID overflows
  // the right edge of a 122 px panel at the caller's requested 18 px.
  // We scale the whole block uniformly rather than per-line so the
  // bold-header / value visual hierarchy and the line spacing stay
  // consistent across the block.
  constexpr int kSidePadding = 4;
  constexpr float kMinPixelHeight = 8.0f;
  const int target_w = panel_w - 2 * kSidePadding;
  if (target_w > 0) {
    int widest_at_request = 0;
    for (const auto& line : lines) {
      if (line.text.empty()) continue;
      const Font& f = line.is_bold ? bold : regular;
      const int w =
          MeasureInkWidth(line.text.c_str(), f, pixel_height, nullptr);
      if (w > widest_at_request) widest_at_request = w;
    }
    if (widest_at_request > target_w) {
      const float scaled = pixel_height * static_cast<float>(target_w) /
                           static_cast<float>(widest_at_request);
      pixel_height = scaled < kMinPixelHeight ? kMinPixelHeight : scaled;
    }
  }

  // Vertical layout: each line occupies `line_h` px; total height is
  // lines.size() * line_h. Centre the block on the panel.
  const float line_h = pixel_height * 1.15f;
  const int block_h = static_cast<int>(lines.size() * line_h);
  const int ascent = regular.AscentPx(pixel_height);
  int y = (panel_h - block_h) / 2 + ascent;

  for (const auto& line : lines) {
    if (line.text.empty()) {
      y += static_cast<int>(line_h);
      continue;
    }
    const Font& f = line.is_bold ? bold : regular;
    int lb = 0;
    const int ink_w = MeasureInkWidth(line.text.c_str(), f, pixel_height, &lb);
    const int x = (panel_w - ink_w) / 2 - lb;
    DrawTextLandscape(fb, x, y, line.text.c_str(), f, pixel_height, white_text);
    y += static_cast<int>(line_h);
  }
}

void DrawSplitText(LandscapeFb& fb, int panel_w, int panel_h,
                   const char* top_text, const char* bottom_text,
                   const char* ref_chars, const Font& font, float pixel_height,
                   bool white_text) {
  // Layout mirrors the existing firmware's splitText
  // (src/lib/drivers/epd/epd.cpp): a 6 px pill-ended separator at the
  // panel vertical centre, each text's reference box offset by `kGap`
  // from the centre line, and the separator as wide as the narrower of
  // the two ink widths.
  constexpr int kGap = 12;  // pixels between centre and text edge
  constexpr int kLineThickness = 6;
  constexpr int kLineRadius = 3;

  // Auto-fit: shrink `pixel_height` so the wider of the two strings
  // doesn't overflow the panel. Antonio (condensed) at the caller's
  // requested 54 px already fits comfortably, so this is a no-op there;
  // Inter at the same 54 px would clip "BLOCK"/"HEIGHT" off the right
  // edge — the loop walks down to ~46 px until the wider string fits.
  // kSidePadding leaves a small visual gutter so glyphs don't kiss the
  // panel edge after the fit.
  constexpr int kSidePadding = 4;
  constexpr float kMinPixelHeight = 16.0f;
  const int target_w = panel_w - 2 * kSidePadding;
  if (target_w > 0) {
    const float top_fit =
        FitTextPx(top_text, font, pixel_height, kMinPixelHeight, target_w);
    const float bot_fit =
        FitTextPx(bottom_text, font, pixel_height, kMinPixelHeight, target_w);
    pixel_height = top_fit < bot_fit ? top_fit : bot_fit;
  }

  const auto rb = font.GetReferenceBox(ref_chars, pixel_height);
  const int centre_y = panel_h / 2;

  // Top text: place its baseline so the reference box's lowest point
  // (below_baseline extent) sits exactly `kGap` above centre_y.
  const int top_baseline = centre_y - kGap - rb.below_baseline;
  // Bottom text: baseline so the reference box's highest point starts
  // `kGap` below centre_y.
  const int bottom_baseline = centre_y + kGap + rb.above_baseline;

  int top_lb = 0, bot_lb = 0;
  const int top_ink_w = MeasureInkWidth(top_text, font, pixel_height, &top_lb);
  const int bot_ink_w =
      MeasureInkWidth(bottom_text, font, pixel_height, &bot_lb);
  const int top_x = (panel_w - top_ink_w) / 2 - top_lb;
  const int bot_x = (panel_w - bot_ink_w) / 2 - bot_lb;

  DrawTextLandscape(fb, top_x, top_baseline, top_text, font, pixel_height,
                    white_text);
  DrawTextLandscape(fb, bot_x, bottom_baseline, bottom_text, font, pixel_height,
                    white_text);

  // Separator: as wide as the shorter ink width (matching production).
  const int line_w = top_ink_w < bot_ink_w ? top_ink_w : bot_ink_w;
  if (line_w > 0) {
    const int line_x = (panel_w - line_w) / 2;
    const int line_y = centre_y - kLineThickness / 2;
    FillRoundRect(fb, line_x, line_y, line_w, kLineThickness, kLineRadius,
                  white_text);
  }
}

}  // namespace btclock
