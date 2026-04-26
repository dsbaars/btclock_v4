// Shared helper that emits the `lightLevel` field on a cJSON status
// payload when an ambient-light sensor is wired. Split out of
// control_server.cpp so both the REST handler and the SSE status
// builder route through the same code path, and so the shape can be
// pinned from host tests (no IDF headers leak into this header).
//
// Field contract (matches what `/api/status` publishes, and what the
// WebUI's `status.lightLevel` reader expects):
//
//   - "lightLevel" : float lux rounded to one decimal, ONLY emitted
//                    when `available` is true. Boards without a BH1750
//                    (Rev A, V8) pass available=false and the key is
//                    absent — the WebUI feature-detects via
//                    `/api/settings`' `hasLightLevel`.
//
// The one-decimal quantisation matches both the serial-log format
// (`lux=%.1f` in main.cpp) and the old firmware's settings-endpoint
// emission, so the WebUI sees the same number regardless of which
// endpoint it polled.
//
// See also: components/webserver/include/control_server.hpp's
// LightSensorIface for the runtime surface this helper reads from.

#pragma once

#include <cmath>

#include "cJSON.h"

namespace btclock {

inline void AttachLightLevelJson(cJSON* root, bool available, float lux) {
  if (!root) return;
  if (!available) return;
  // Quantise to one decimal before handing to cJSON — cJSON prints
  // numbers with `%1.15g`, so an un-rounded 15.833333 would bleed into
  // the JSON as `15.833333015441895`. Widen to double BEFORE the
  // multiply/round so the 0.1 recip of 10 is representable — rounding
  // in float leaves the result as e.g. 15.8000001907... which cJSON
  // then serialises in full. Matches `lux=%.1f` in the serial log.
  const double rounded = std::round(static_cast<double>(lux) * 10.0) / 10.0;
  cJSON_AddNumberToObject(root, "lightLevel", rounded);
}

}  // namespace btclock
