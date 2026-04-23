// See font_wasm_aa.hpp — implementation of the alpha sidechannel used
// by the preview canvas' AA mode. Never compiled into device firmware;
// the body is entirely behind BTCLOCK_WASM_BUILD.

#include "font_wasm_aa.hpp"

#ifdef BTCLOCK_WASM_BUILD

#include <array>

namespace btclock {
namespace wasm_aa {
namespace {

// Hard-cap matches the current binding's kPanels (7). A small stack-
// sized table keeps the lookup trivial — linear scan of at most 8
// entries, way cheaper than any hashing.
constexpr std::size_t kMaxPanels = 8;

struct PanelSlot {
  const uint8_t* fb_key = nullptr;  // matches LandscapeFb::native_fb
  uint8_t* alpha = nullptr;         // output buffer
};

std::array<PanelSlot, kMaxPanels> g_slots{};
std::size_t g_n_slots = 0;
int g_w = 0;
int g_h = 0;
int g_stride = 0;

// Nesting counter for SetPixelSuppressScope. While > 0, SetPixelLandscape's
// call site in font.cpp skips its wasm_aa::WritePixel tap — DrawText's
// raw-alpha WritePixel (called BEFORE the 1-bpp threshold) is the
// authoritative value for those pixels.
int g_suppress_depth = 0;

// Linear search — tiny N, branch-predictable, faster than a map.
uint8_t* FindAlphaBuf(const uint8_t* fb_key) {
  for (std::size_t i = 0; i < g_n_slots; ++i) {
    if (g_slots[i].fb_key == fb_key) return g_slots[i].alpha;
  }
  return nullptr;
}

}  // namespace

void SetPanelTargets(std::size_t n_panels,
                     const uint8_t* const* panel_fb_ptrs,
                     uint8_t* const* alpha_bufs,
                     int w, int h, int stride) {
  g_n_slots = n_panels < kMaxPanels ? n_panels : kMaxPanels;
  for (std::size_t i = 0; i < g_n_slots; ++i) {
    g_slots[i].fb_key = panel_fb_ptrs[i];
    g_slots[i].alpha = alpha_bufs[i];
  }
  g_w = w;
  g_h = h;
  g_stride = stride;
}

void Clear() {
  g_n_slots = 0;
  g_w = 0;
  g_h = 0;
  g_stride = 0;
}

void WritePixel(const uint8_t* panel_key, int lx, int ly,
                uint8_t alpha, bool white_text) {
  if (g_n_slots == 0) return;
  uint8_t* buf = FindAlphaBuf(panel_key);
  if (buf == nullptr) return;
  if (lx < 0 || lx >= g_w) return;
  if (ly < 0 || ly >= g_h) return;
  uint8_t& slot = buf[ly * g_stride + lx];
  if (!white_text) {
    // Accumulate max: overlapping glyph edges keep the darker value.
    if (alpha > slot) slot = alpha;
  } else {
    // Inverted paint — glyph alpha erases ink. Bound below by 0.
    const int erased = 255 - static_cast<int>(alpha);
    if (erased < static_cast<int>(slot)) {
      slot = static_cast<uint8_t>(erased);
    }
  }
}

void FillRect(const uint8_t* panel_key,
              int x, int y, int w, int h, uint8_t alpha) {
  if (g_n_slots == 0) return;
  uint8_t* buf = FindAlphaBuf(panel_key);
  if (buf == nullptr) return;
  if (w <= 0 || h <= 0) return;
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w; if (x1 > g_w) x1 = g_w;
  int y1 = y + h; if (y1 > g_h) y1 = g_h;
  for (int yy = y0; yy < y1; ++yy) {
    uint8_t* row = buf + yy * g_stride;
    for (int xx = x0; xx < x1; ++xx) {
      row[xx] = alpha;
    }
  }
}

SetPixelSuppressScope::SetPixelSuppressScope() { ++g_suppress_depth; }
SetPixelSuppressScope::~SetPixelSuppressScope() { --g_suppress_depth; }

bool SetPixelSuppressed() { return g_suppress_depth > 0; }

}  // namespace wasm_aa
}  // namespace btclock

#endif  // BTCLOCK_WASM_BUILD
