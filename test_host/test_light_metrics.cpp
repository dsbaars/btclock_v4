// Pins the /api/status `lightLevel` field contract. See
// components/webserver/include/light_metrics.hpp for the contract.
// The WebUI feature-detects sensor presence via /api/settings'
// `hasLightLevel`; /api/status must therefore omit `lightLevel`
// entirely when no sensor is present (so the WebUI doesn't render a
// stale 0 / null readout), and emit it as a one-decimal float when
// one is.

#include "cJSON.h"
#include "doctest.h"
#include "light_metrics.hpp"

namespace {

// Roundtrips the attached payload through cJSON's printer so the test
// pins the on-the-wire shape (one-decimal quantisation), not just the
// in-memory double. Matches how BuildStatusJson serialises.
std::string PrintedLightLevel(cJSON* root) {
  cJSON* n = cJSON_GetObjectItemCaseSensitive(root, "lightLevel");
  if (!cJSON_IsNumber(n)) return "<missing>";
  char* txt = cJSON_PrintUnformatted(n);
  if (!txt) return "<oom>";
  std::string out(txt);
  free(txt);
  return out;
}

}  // namespace

TEST_CASE("AttachLightLevelJson: emits lightLevel when sensor available") {
  cJSON* root = cJSON_CreateObject();
  btclock::AttachLightLevelJson(root, /*available=*/true, /*lux=*/15.8f);
  cJSON* n = cJSON_GetObjectItemCaseSensitive(root, "lightLevel");
  REQUIRE(cJSON_IsNumber(n));
  CHECK(n->valuedouble == doctest::Approx(15.8));
  // Printed shape must be a compact one-decimal float — matches the
  // serial log's `lux=%.1f` so the WebUI readout stays stable.
  CHECK(PrintedLightLevel(root) == "15.8");
  cJSON_Delete(root);
}

TEST_CASE("AttachLightLevelJson: omits lightLevel when unavailable") {
  cJSON* root = cJSON_CreateObject();
  btclock::AttachLightLevelJson(root, /*available=*/false, /*lux=*/0.0f);
  cJSON* n = cJSON_GetObjectItemCaseSensitive(root, "lightLevel");
  // The WebUI feature-detects via /api/settings' `hasLightLevel`; if
  // we emit a zero or null here, Rev A / V8 would show a bogus "0 lx"
  // readout. Absence is the contract.
  CHECK(n == nullptr);
  cJSON_Delete(root);
}

TEST_CASE("AttachLightLevelJson: quantises sub-decimal noise to one digit") {
  // BH1750 raw reads land on e.g. 15.8333... after the /1.2 datasheet
  // scaling. Without rounding, cJSON's %1.15g printer would serialise
  // the full float-as-double repr (~15.833333015441895). Pin the
  // one-decimal shape so the WebUI number stays stable across polls.
  cJSON* root = cJSON_CreateObject();
  btclock::AttachLightLevelJson(root, /*available=*/true, /*lux=*/15.8333f);
  CHECK(PrintedLightLevel(root) == "15.8");
  cJSON_Delete(root);
}

TEST_CASE("AttachLightLevelJson: emits zero lux when that's the reading") {
  // "Dark room" case — sensor is present and reporting zero. We still
  // want the field, the readout just says 0. Only the `available=false`
  // path suppresses the key.
  cJSON* root = cJSON_CreateObject();
  btclock::AttachLightLevelJson(root, /*available=*/true, /*lux=*/0.0f);
  cJSON* n = cJSON_GetObjectItemCaseSensitive(root, "lightLevel");
  REQUIRE(cJSON_IsNumber(n));
  CHECK(n->valuedouble == doctest::Approx(0.0));
  cJSON_Delete(root);
}

TEST_CASE("AttachLightLevelJson: null root is a no-op") {
  // Guards against OOM call sites — BuildStatusJson bails early if
  // cJSON_CreateObject() returned null, but the helper must still not
  // deref it if called.
  btclock::AttachLightLevelJson(nullptr, true, 12.3f);
  CHECK(true);
}
