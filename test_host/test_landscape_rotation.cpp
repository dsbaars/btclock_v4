// Pin the four-rotation logical→native coord transform that lives at
// the heart of SetPixelLandscape. The math is one line per case in
// production today but the four cases together carry enough off-by-one
// risk (corner pixels, rotated strides) that pinning them in a host
// test guards every screen that depends on a particular orientation
// — particularly Rev B's k180 default, where a sign flip would land
// the entire frame upside-down on the panel without a unit test
// catching it.

#include "doctest.h"
#include "font.hpp"
#include "landscape_rotation.hpp"

using btclock::NativeXY;
using btclock::RotateLogicalToNative;
using btclock::Rotation;

namespace {
// Match what fits a 2.13" panel: 122 × 250 native portrait.
constexpr int kNW = 122;
constexpr int kNH = 250;

bool Eq(NativeXY a, int x, int y) {
  return a.x == x && a.y == y;
}
}  // namespace

// ----- k0 (no rotation) -----

TEST_CASE("k0 maps logical (0,0) to native (0,0)") {
  CHECK(Eq(RotateLogicalToNative(0, 0, kNW, kNH, Rotation::k0), 0, 0));
}

TEST_CASE("k0 preserves arbitrary logical coords") {
  CHECK(Eq(RotateLogicalToNative(7, 13, kNW, kNH, Rotation::k0), 7, 13));
  CHECK(Eq(RotateLogicalToNative(kNW - 1, kNH - 1, kNW, kNH, Rotation::k0),
           kNW - 1, kNH - 1));
}

// ----- k180 (Rev B default) -----

TEST_CASE("k180 maps logical (0,0) to native (nw-1, nh-1)") {
  CHECK(Eq(RotateLogicalToNative(0, 0, kNW, kNH, Rotation::k180), kNW - 1,
           kNH - 1));
}

TEST_CASE("k180 maps native bottom-right back to logical origin") {
  CHECK(Eq(RotateLogicalToNative(kNW - 1, kNH - 1, kNW, kNH, Rotation::k180), 0,
           0));
}

TEST_CASE("k180 is an involution on every interior pixel") {
  // Rotating twice by 180° restores the original. Walk a coarse grid
  // — every step exercises the same arithmetic so a sample is enough.
  for (int x = 0; x < kNW; x += 7) {
    for (int y = 0; y < kNH; y += 9) {
      const auto once = RotateLogicalToNative(x, y, kNW, kNH, Rotation::k180);
      const auto twice =
          RotateLogicalToNative(once.x, once.y, kNW, kNH, Rotation::k180);
      CHECK(twice.x == x);
      CHECK(twice.y == y);
    }
  }
}

// ----- k90Cw (clockwise) -----

TEST_CASE("k90Cw maps logical (0,0) to native (nw-1, 0)") {
  CHECK(Eq(RotateLogicalToNative(0, 0, kNW, kNH, Rotation::k90Cw), kNW - 1, 0));
}

TEST_CASE("k90Cw rotates the four corners cleanly") {
  // Top-right of logical → top-left of native.
  CHECK(Eq(RotateLogicalToNative(kNH - 1, 0, kNW, kNH, Rotation::k90Cw),
           kNW - 1, kNH - 1));
  // Bottom-left of logical → top-right of native.
  CHECK(Eq(RotateLogicalToNative(0, kNW - 1, kNW, kNH, Rotation::k90Cw), 0, 0));
}

// ----- k90Ccw (counter-clockwise) -----

TEST_CASE("k90Ccw maps logical (0,0) to native (0, nh-1)") {
  CHECK(
      Eq(RotateLogicalToNative(0, 0, kNW, kNH, Rotation::k90Ccw), 0, kNH - 1));
}

TEST_CASE("k90Ccw is the inverse of k90Cw") {
  // CW then CCW (or CCW then CW) restores the original. Walk a
  // sample of the *logical* rect — note the rotated pass operates in
  // (height × width) so swap the dims for the second hop.
  for (int x = 0; x < kNH; x += 11) {
    for (int y = 0; y < kNW; y += 5) {
      const auto cw = RotateLogicalToNative(x, y, kNW, kNH, Rotation::k90Cw);
      const auto back =
          RotateLogicalToNative(cw.x, cw.y, kNH, kNW, Rotation::k90Ccw);
      CHECK(back.x == x);
      CHECK(back.y == y);
    }
  }
}

// ----- Edges -----

TEST_CASE("Rotation transforms with 1×1 framebuffer collapse to (0,0)") {
  // Degenerate but legal — every rotation on a single-pixel panel
  // lands at (0, 0). Catches a sign error that would surface as
  // negative coordinates.
  const NativeXY at0 = RotateLogicalToNative(0, 0, 1, 1, Rotation::k0);
  const NativeXY at90Cw = RotateLogicalToNative(0, 0, 1, 1, Rotation::k90Cw);
  const NativeXY at180 = RotateLogicalToNative(0, 0, 1, 1, Rotation::k180);
  const NativeXY at90Ccw = RotateLogicalToNative(0, 0, 1, 1, Rotation::k90Ccw);
  CHECK(Eq(at0, 0, 0));
  CHECK(Eq(at90Cw, 0, 0));
  CHECK(Eq(at180, 0, 0));
  CHECK(Eq(at90Ccw, 0, 0));
}

TEST_CASE("Rotation transforms preserve native bounds for interior pixels") {
  // Walk every rotation across a coarse grid and assert the result
  // stays inside [0, nw) × [0, nh) (k90 cases swap dims for the
  // logical iteration). A negative or out-of-bounds output here would
  // surface as a stray write or out-of-bounds read in the byte index
  // calculation that follows in SetPixelLandscape.
  for (int rot_idx = 0; rot_idx < 4; ++rot_idx) {
    const auto rot = static_cast<Rotation>(rot_idx);
    const int lw = (rot == Rotation::k0 || rot == Rotation::k180) ? kNW : kNH;
    const int lh = (rot == Rotation::k0 || rot == Rotation::k180) ? kNH : kNW;
    for (int x = 0; x < lw; x += 13) {
      for (int y = 0; y < lh; y += 17) {
        const auto out = RotateLogicalToNative(x, y, kNW, kNH, rot);
        CHECK(out.x >= 0);
        CHECK(out.x < kNW);
        CHECK(out.y >= 0);
        CHECK(out.y < kNH);
      }
    }
  }
}

TEST_CASE("Rotation is constexpr-evaluable") {
  // Pinning constexpr-ness: callers may use this in compile-time
  // contexts (e.g. unit-test fixtures), and dropping constexpr would
  // be a silent break.
  constexpr auto p = RotateLogicalToNative(5, 7, 100, 200, Rotation::k180);
  static_assert(p.x == 94 && p.y == 192, "rotation math constexpr");
  CHECK(p.x == 94);
  CHECK(p.y == 192);
}
