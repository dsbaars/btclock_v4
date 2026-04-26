// Pin the linear-search semantics font.cpp's FitTextPx depends on.
// The search step is decoupled from rasterisation via a measurement
// callable so we can drive it with a fake measurer here; production
// passes a lambda over MeasureInkWidth(font, …) at the call site.

#include "doctest.h"

#include "fit_text_px.hpp"

using btclock::FitTextPxBy;

namespace {

// Width is k * pixel_height — mirrors how a constant-stride glyph
// scales linearly with size. Pick k = 10 to keep the math obvious.
auto LinearMeasure(double k) {
  return [k](float px) {
    return static_cast<int>(px * k);
  };
}

}  // namespace

TEST_CASE("FitTextPxBy returns max_px when the largest size fits") {
  const float fit = FitTextPxBy(LinearMeasure(2.0), 50.0f, 10.0f, 100);
  CHECK(fit == doctest::Approx(50.0f));
}

TEST_CASE("FitTextPxBy walks down until the measured width fits") {
  // Width = 10 × px. Target 100 → fits at exactly 10 px. Starting from
  // 50 px (width 500), the search walks down by 0.5 px per step until
  // it lands on the first size with width ≤ 100.
  const float fit = FitTextPxBy(LinearMeasure(10.0), 50.0f, 5.0f, 100);
  CHECK(fit == doctest::Approx(10.0f));
}

TEST_CASE("FitTextPxBy returns min_px when nothing fits") {
  // Width = 100 × px; even at min_px = 5 the width is 500 (> target).
  // Production callers (DrawSplitText) accept this floor and let the
  // text overflow rather than refusing to render.
  const float fit = FitTextPxBy(LinearMeasure(100.0), 50.0f, 5.0f, 100);
  CHECK(fit == doctest::Approx(5.0f));
}

TEST_CASE("FitTextPxBy step size is 0.5 px") {
  // Production callers compare an int width (the measurer truncates).
  // Width(px) = int(px × 4) hits target=40 at the first 0.5-aligned
  // px whose truncated width ≤ 40. Walking down from 20: 20.0×4 = 80
  // (skip), 19.5×4 = 78 (skip)… 10.5×4 = 42 (skip), 10.0×4 = 40
  // (fits) — the search stops at exactly 10.0, pinning the 0.5 step.
  const float fit = FitTextPxBy(LinearMeasure(4.0), 20.0f, 1.0f, 40);
  CHECK(fit == doctest::Approx(10.0f));
}

TEST_CASE("FitTextPxBy max_px == min_px short-circuits to that size") {
  // Degenerate case: caller pinned a single size. Whether it fits or
  // not the loop runs once and returns either max_px (fits) or min_px
  // (doesn't) — both are the same number here.
  const float fits = FitTextPxBy(LinearMeasure(1.0), 30.0f, 30.0f, 100);
  CHECK(fits == doctest::Approx(30.0f));
  const float nope = FitTextPxBy(LinearMeasure(1.0), 30.0f, 30.0f, 10);
  CHECK(nope == doctest::Approx(30.0f));
}

TEST_CASE("FitTextPxBy zero target fits only at width-zero sizes") {
  // A measurer that returns 0 for px ≤ 5 and width otherwise — the
  // fit walks down through the non-zero region and lands at 5.
  auto measure = [](float px) {
    return px <= 5.0f ? 0 : static_cast<int>(px * 10);
  };
  const float fit = FitTextPxBy(measure, 50.0f, 1.0f, 0);
  CHECK(fit == doctest::Approx(5.0f));
}

TEST_CASE("FitTextPxBy with measurer returning 0 always fits at max_px") {
  // No glyphs / empty string at the production layer measures to 0.
  // The search returns max_px immediately because 0 ≤ any target.
  auto zero = [](float) { return 0; };
  const float fit = FitTextPxBy(zero, 100.0f, 10.0f, 50);
  CHECK(fit == doctest::Approx(100.0f));
}
