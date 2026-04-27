// Pure coord-transform extracted from font.cpp's SetPixelLandscape so
// the rotation math can be unit-tested without a framebuffer. Each
// case is a one-liner today but the four cases together carry enough
// off-by-one risk (corners, rotated stride) that pinning them in a
// host test guards the screens that depend on a particular orientation
// (Rev B's k180 default in particular).

#pragma once

#include "font.hpp"  // for Rotation

namespace btclock {

struct NativeXY {
  int x;
  int y;
};

// Map a logical (lx, ly) inside a native (nw × nh) framebuffer through
// `rot` into native (nx, ny). Caller is responsible for any logical
// bounds-checking — this transform doesn't validate inputs because it
// also runs inside fill loops where the caller has already clipped.
inline constexpr NativeXY RotateLogicalToNative(int lx, int ly, int nw, int nh,
                                                Rotation rot) {
  // Mirror SetPixelLandscape's switch verbatim. Anything that diverges
  // here would silently mis-paint the frame.
  switch (rot) {
    case Rotation::k0:
      return {lx, ly};
    case Rotation::k90Cw:
      return {nw - 1 - ly, lx};
    case Rotation::k180:
      return {nw - 1 - lx, nh - 1 - ly};
    case Rotation::k90Ccw:
      return {ly, nh - 1 - lx};
  }
  return {lx, ly};
}

// Map a logical (lx, ly) to the orientation a viewer of the physical
// panel sees ("user-upright"). The panels are physically mounted with
// the EPD pin straps facing upward, which the firmware models as a
// global Rotation::k180 inverse on top of any per-screen rotation:
// applying RotateLogicalToNative(rot) and then inverting k180 gives
// the upright image.
//
// For Rotation::k180 (the common case for digit panels) this folds to
// identity — logical coords already match what the user sees. For a
// label panel using Rotation::k90Cw the transform becomes
// (lx, ly) → (ly, nh - 1 - lx), which is the 90° clockwise visual
// rotation that makes the label text read top-to-bottom.
//
// Used by the WASM AA sidechannel so its alpha buffers come out in the
// same orientation regardless of fb.rotation, and by host tests that
// pin the rotation math.
inline constexpr NativeXY RotateLogicalToUserUpright(int lx, int ly, int nw,
                                                     int nh, Rotation rot) {
  const NativeXY n = RotateLogicalToNative(lx, ly, nw, nh, rot);
  return {nw - 1 - n.x, nh - 1 - n.y};
}

}  // namespace btclock
