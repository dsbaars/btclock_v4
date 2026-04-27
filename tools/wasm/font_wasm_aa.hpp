// WASM-only alpha sidechannel for the preview canvas.
//
// The device's font.cpp paint loop thresholds stb_truetype's grayscale
// coverage at `a >= 128` and writes a single 1-bpp pixel through
// SetPixelLandscape. That's exactly what the e-paper hardware shows,
// but on-screen it looks jagged at oblique edges.
//
// This header exposes a panel-keyed alpha "sidechannel" wired into the
// same paint primitives: while the preview's AA mode is in flight, the
// primitives ALSO write the raw stb_truetype alpha (or 0/255 for solid
// fills) into a caller-owned byte buffer per panel, in LOGICAL panel
// coordinates. The 1-bpp path is untouched — it still produces exactly
// what a physical panel would render.
//
// Per-panel demux: the screen renderers paint each panel through its
// own LandscapeFb whose native_fb pointer is unique (points into a
// distinct ctx.fbs[i][] row). We key the alpha target on that pointer:
// the binding registers a (fb_ptr → alpha_buf) table, and each paint
// hook looks up its LandscapeFb's fb pointer to pick the right slot.
// This lets a single render pass fill all N panel alpha buffers
// cleanly without touching the renderer code.
//
// Buffer format: each byte is the *ink coverage* at that pixel.
//    0   — no ink (white)
//    255 — full ink (black)
//    1..254 — fractional coverage along a glyph edge.
//
// Orientation: USER-UPRIGHT coords — the image the way the viewer sees
// it on the physical panel, regardless of the panel's per-screen
// `LandscapeFb::rotation`. Buffer dimensions are the panel's NATIVE
// width × height (e.g. 122×250 on a 2.13"), not the rotation-swapped
// logical dimensions.
//
// The font.cpp call sites apply `RotateLogicalToUserUpright` (in
// landscape_rotation.hpp) before invoking these primitives, which
// composes the per-LandscapeFb logical→native transform with the
// global k180-mounted-panel inverse. For Rotation::k180 (digit panels)
// that fold to identity — logical coords already match what the user
// sees. For Rotation::k90Cw (verticalDesc label panels) the transform
// rotates the alpha 90° CW, matching the label text the framebuffer
// writes to native bytes.
//
// white_text handling: for non-inverted text (white_text=false, the
// common case — black ink on a white background), glyph alpha
// ACCUMULATES via max: overlapping glyph edges keep the darker value.
// For inverted text (white_text=true, white ink on a pre-blackened
// background) we take the min of `255 - a`: each glyph pixel erases ink
// proportionally to its coverage, bounded below by 0.
//
// ON DEVICE this whole file is compiled out — all functions live under
// #ifdef BTCLOCK_WASM_BUILD. font.cpp #includes it unconditionally and
// relies on empty inline stubs to vanish when BTCLOCK_WASM_BUILD isn't
// set.

#pragma once

#include <cstdint>
#include <cstddef>

namespace btclock {
namespace wasm_aa {

#ifdef BTCLOCK_WASM_BUILD

// Register N panel targets. `panel_fb_ptrs[i]` is the unique pointer
// identifying panel i (typically the LandscapeFb's native_fb, or any
// stable per-panel address). `alpha_bufs[i]` is the output buffer for
// panel i — must be at least `stride * h` bytes and remain valid until
// Clear() or another SetPanelTargets() is called. `w`/`h` are the
// logical-coord dimensions (same for every panel in this preview).
// stride is typically w (no row padding). Pass `n_panels=0` to disarm.
void SetPanelTargets(std::size_t n_panels,
                     const uint8_t* const* panel_fb_ptrs,
                     uint8_t* const* alpha_bufs,
                     int w, int h, int stride);

// Disarm and forget the target buffers.
void Clear();

// Write one pixel at LOGICAL (lx, ly) into the panel whose native_fb
// equals `panel_key`. `alpha` is the stb_truetype coverage (0..255).
// `white_text`:
//   - false (black-on-white): alpha_buf[idx] = max(prev, alpha)
//   - true  (white-on-black): alpha_buf[idx] = min(prev, 255 - alpha)
// Bounds-checked against the target w/h. A panel_key that doesn't
// match any registered panel is a no-op (so code that paints into
// non-WASM LandscapeFbs stays safe).
void WritePixel(const uint8_t* panel_key, int lx, int ly,
                uint8_t alpha, bool white_text);

// Paint a solid axis-aligned rect at LOGICAL coords into the panel
// whose native_fb equals `panel_key`, with a constant ink value
// (0 = white/no-ink, 255 = full black ink). Used by ClearFb and solid
// fills. Out-of-bounds pixels are clipped.
void FillRect(const uint8_t* panel_key,
              int x, int y, int w, int h, uint8_t alpha);

// RAII scope that silences the WritePixel hook inside SetPixelLandscape.
// DrawText's inner loop records the true grayscale alpha BEFORE the
// 1-bpp threshold, then calls SetPixelLandscape for the bit-write.
// Without this guard, the SetPixelLandscape tap would re-write
// alpha=255 via max and flatten partial-coverage edges back to full
// ink. Usage:
//
//   {
//     wasm_aa::SetPixelSuppressScope guard;
//     // ... SetPixelLandscape calls that shouldn't re-tap the buffer ...
//   }
class SetPixelSuppressScope {
 public:
  SetPixelSuppressScope();
  ~SetPixelSuppressScope();
  SetPixelSuppressScope(const SetPixelSuppressScope&) = delete;
  SetPixelSuppressScope& operator=(const SetPixelSuppressScope&) = delete;
};

// Internal: whether the SetPixelLandscape tap is currently suppressed.
// DrawText's hot loop checks this (indirectly via the RAII guard's
// counter) to skip writing alpha twice.
bool SetPixelSuppressed();

#else  // !BTCLOCK_WASM_BUILD — compile to nothing on device.

inline void SetPanelTargets(std::size_t /*n_panels*/,
                            const uint8_t* const* /*panel_fb_ptrs*/,
                            uint8_t* const* /*alpha_bufs*/,
                            int /*w*/, int /*h*/, int /*stride*/) {}
inline void Clear() {}
inline void WritePixel(const uint8_t* /*panel_key*/,
                       int /*lx*/, int /*ly*/,
                       uint8_t /*alpha*/, bool /*white_text*/) {}
inline void FillRect(const uint8_t* /*panel_key*/,
                     int /*x*/, int /*y*/, int /*w*/, int /*h*/,
                     uint8_t /*alpha*/) {}
inline bool SetPixelSuppressed() { return false; }
// Opaque suppress-scope stub so callers #include this header
// unconditionally. Zero-cost on device.
class SetPixelSuppressScope {
 public:
  SetPixelSuppressScope() = default;
  ~SetPixelSuppressScope() = default;
  SetPixelSuppressScope(const SetPixelSuppressScope&) = delete;
  SetPixelSuppressScope& operator=(const SetPixelSuppressScope&) = delete;
};

#endif

}  // namespace wasm_aa
}  // namespace btclock
