// Host tests for the verticalDesc-rotation layout helper.
//
// The full PaintSlot / PaintSlotIntoFb pipeline pulls in font.hpp +
// stb_truetype + the fonts bundle, which we don't want to drag onto
// the host target. The rotation math itself, however, is tiny and
// pure: it swaps the logical (w, h) of a panel when vertical_desc is
// armed AND the slot kind is a label. This file pins that math
// against the 2.13" panel dims (122x250 native, k180 default so
// logical = 122x250 pre-flip, 250x122 post-flip).
//
// The pixel-layer validation — "panel 0 framebuffer hash differs under
// verticalDesc; panels 1..N-1 bit-identical" — lives in the WASM
// smoke test tools/wasm/vertical_desc_hash_check.mjs. Keeping the two
// layers split avoids dragging the font stack onto the host target
// while still pinning both halves of the contract.

#include <cstddef>
#include <cstdint>

#include "doctest.h"

namespace {

// Mirror of PaintSlot::Kind's subset we care about here. Matches
// main/screens/common.hpp — the helper below is a pure port of the
// one-line selector PaintSlotIntoFb uses.
enum class SlotKind : std::uint8_t {
  kBlank,
  kLabel,
  kLabelSplit,
  kDigit,
  kSmallGroup,
  kSatsGlyph,
  kCurrencyGlyph,
  kUnitSplit,
  kIconBitmap,
};

struct PanelDims {
  int w;  // logical width seen by the paint primitive
  int h;  // logical height seen by the paint primitive
};

// Given a panel's native_w/native_h at the renderer's default k180
// orientation (logical == native pre-flip on 0/180), plus `vertical_desc`
// and the slot kind, return the logical dims the paint primitive will
// see. Matches the swap done inside PaintSlotIntoFb:
//
//   if (vertical_desc && (kind == kLabel || kind == kLabelSplit))
//     rotation = k90Cw;  // swaps logical w <-> h
//
// k90Cw's LogicalWidth is the native height; LogicalHeight is the
// native width. Everything else keeps the k180 dims.
PanelDims LogicalDimsAfterVerticalDesc(int native_w, int native_h,
                                       SlotKind kind, bool vertical_desc) {
  const bool rotate = vertical_desc && (kind == SlotKind::kLabel ||
                                        kind == SlotKind::kLabelSplit);
  return rotate ? PanelDims{native_h, native_w} : PanelDims{native_w, native_h};
}

// Mirrors the fit-width selector in PaintSlotIntoFb's kLabelSplit branch:
// when verticalDesc rotates the label, DrawSplitText still scales against
// physical panel width (native_w), not the rotated logical width.
int SplitLabelFitWidthAfterVerticalDesc(int native_w, int native_h,
                                        bool vertical_desc) {
  const auto dims = LogicalDimsAfterVerticalDesc(
      native_w, native_h, SlotKind::kLabelSplit, vertical_desc);
  const bool rotate = vertical_desc;
  return rotate ? native_w : dims.w;
}

// 2.13" panel — the variant wired for the REV_A/REV_B/V8 boards the
// renderers actually target.
constexpr int kNativeW = 122;
constexpr int kNativeH = 250;

}  // namespace

TEST_CASE("vertical_desc=false keeps logical dims at native (k180 path)") {
  // Every kind renders unchanged. Spot-checks picked so a regression
  // in the switch would flip at least one of them; the label kinds are
  // the important ones (they MUST NOT rotate when the flag is off).
  const SlotKind kinds[] = {
      SlotKind::kBlank,         SlotKind::kLabel,      SlotKind::kLabelSplit,
      SlotKind::kDigit,         SlotKind::kSmallGroup, SlotKind::kSatsGlyph,
      SlotKind::kCurrencyGlyph, SlotKind::kUnitSplit,  SlotKind::kIconBitmap,
  };
  for (const auto k : kinds) {
    const auto d = LogicalDimsAfterVerticalDesc(kNativeW, kNativeH, k,
                                                /*vertical_desc=*/false);
    CHECK(d.w == kNativeW);
    CHECK(d.h == kNativeH);
  }
}

TEST_CASE("vertical_desc=true rotates label slots 90°, leaves other kinds") {
  // Label kinds swap into a 250×122 landscape-long region so the text
  // runs along the panel's long axis.
  const auto lbl =
      LogicalDimsAfterVerticalDesc(kNativeW, kNativeH, SlotKind::kLabel, true);
  CHECK(lbl.w == kNativeH);
  CHECK(lbl.h == kNativeW);

  const auto lbl_split = LogicalDimsAfterVerticalDesc(
      kNativeW, kNativeH, SlotKind::kLabelSplit, true);
  CHECK(lbl_split.w == kNativeH);
  CHECK(lbl_split.h == kNativeW);

  // Every non-label kind stays put — the flag must not touch digit /
  // glyph / unit / icon slots.
  const SlotKind leaveAlone[] = {
      SlotKind::kBlank,      SlotKind::kDigit,         SlotKind::kSmallGroup,
      SlotKind::kSatsGlyph,  SlotKind::kCurrencyGlyph, SlotKind::kUnitSplit,
      SlotKind::kIconBitmap,
  };
  for (const auto k : leaveAlone) {
    const auto d = LogicalDimsAfterVerticalDesc(kNativeW, kNativeH, k,
                                                /*vertical_desc=*/true);
    CHECK(d.w == kNativeW);
    CHECK(d.h == kNativeH);
  }
}

TEST_CASE("Rotation is reversible — applying the flag twice returns to base") {
  // The rotation is a pure predicate on (kind, flag); toggling the flag
  // back off restores the k180 dims. Guards against accidental mutation
  // of an input LandscapeFb that callers share across panels.
  auto rotated =
      LogicalDimsAfterVerticalDesc(kNativeW, kNativeH, SlotKind::kLabel, true);
  CHECK(rotated.w == kNativeH);
  CHECK(rotated.h == kNativeW);

  auto restored =
      LogicalDimsAfterVerticalDesc(kNativeW, kNativeH, SlotKind::kLabel, false);
  CHECK(restored.w == kNativeW);
  CHECK(restored.h == kNativeH);
}

TEST_CASE("vertical_desc split-label fit target stays width-based") {
  // Regression guard: when verticalDesc rotates label split slots into
  // 250x122 logical space, width fitting must still use 122px physical width.
  const auto rotated_dims = LogicalDimsAfterVerticalDesc(
      kNativeW, kNativeH, SlotKind::kLabelSplit, /*vertical_desc=*/true);
  CHECK(rotated_dims.w == kNativeH);
  CHECK(rotated_dims.h == kNativeW);
  CHECK(SplitLabelFitWidthAfterVerticalDesc(kNativeW, kNativeH,
                                            /*vertical_desc=*/true) ==
        kNativeW);

  // Non-rotated path keeps the same 122px target.
  CHECK(SplitLabelFitWidthAfterVerticalDesc(kNativeW, kNativeH,
                                            /*vertical_desc=*/false) ==
        kNativeW);
}
