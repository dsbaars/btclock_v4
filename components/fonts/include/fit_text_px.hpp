// Linear-search "what pixel_height fits in target_w" extracted from
// font.cpp's FitTextPx. The original couples the search step to
// MeasureInkWidth(font, ...) which pulls in stb_truetype. By taking a
// measurement callable, the search itself is testable host-side with a
// fake measurer; production passes a lambda that calls MeasureInkWidth
// for the real Font.

#pragma once

namespace btclock {

// Walks max_px → min_px in 0.5px steps and returns the first size
// whose measured width is ≤ target_w. Returns min_px if no size fits.
// Step size matches the original implementation — fine-grained enough
// that the visible result doesn't jitter between calls.
template <typename MeasureFn>
inline float FitTextPxBy(MeasureFn&& measure, float max_px, float min_px,
                         int target_w) {
  for (float px = max_px; px >= min_px; px -= 0.5f) {
    if (measure(px) <= target_w) return px;
  }
  return min_px;
}

// Scale an auto-fit width target by a user-facing percentage while keeping
// the result in [1, target_w]. `percent` is defensively clamped to 1..100.
inline int ScaleTargetWidthByPercent(int target_w, float percent) {
  if (target_w <= 0) return target_w;
  if (!(percent > 0.0f)) percent = 100.0f;
  if (percent < 1.0f) percent = 1.0f;
  if (percent > 100.0f) percent = 100.0f;
  int scaled =
      static_cast<int>(static_cast<float>(target_w) * (percent / 100.0f));
  if (scaled < 1) scaled = 1;
  if (scaled > target_w) scaled = target_w;
  return scaled;
}

}  // namespace btclock
