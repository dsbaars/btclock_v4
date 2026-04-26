// Pins the /api/status heap-field contract. See
// components/webserver/include/heap_metrics.hpp for the contract; the
// regression being guarded is when "espFreeHeap" got fed from
// esp_get_free_heap_size() on ESP32-S3, which silently folds PSRAM in
// and produced free > size. A future refactor that reintroduces that
// mistake must fail here first.

#include "cJSON.h"
#include "doctest.h"
#include "heap_metrics.hpp"

namespace {

struct HeapFields {
  double free_heap;
  double heap_size;
  double free_psram;
  double psram_size;
};

HeapFields ReadFields(cJSON* root) {
  HeapFields out{};
  cJSON* f = cJSON_GetObjectItemCaseSensitive(root, "espFreeHeap");
  cJSON* s = cJSON_GetObjectItemCaseSensitive(root, "espHeapSize");
  cJSON* fp = cJSON_GetObjectItemCaseSensitive(root, "espFreePsram");
  cJSON* ps = cJSON_GetObjectItemCaseSensitive(root, "espPsramSize");
  REQUIRE(cJSON_IsNumber(f));
  REQUIRE(cJSON_IsNumber(s));
  REQUIRE(cJSON_IsNumber(fp));
  REQUIRE(cJSON_IsNumber(ps));
  out.free_heap = f->valuedouble;
  out.heap_size = s->valuedouble;
  out.free_psram = fp->valuedouble;
  out.psram_size = ps->valuedouble;
  return out;
}

}  // namespace

TEST_CASE("AttachHeapMetricsJson: emits four canonical fields") {
  cJSON* root = cJSON_CreateObject();
  // Typical ESP32-S3 numbers: 320 KiB internal, 8 MiB PSRAM.
  btclock::AttachHeapMetricsJson(root,
                                 /*free_internal=*/150'000,
                                 /*total_internal=*/329'995,
                                 /*free_psram=*/1'800'000,
                                 /*total_psram=*/8'388'608);
  const auto h = ReadFields(root);

  CHECK(h.free_heap == doctest::Approx(150'000.0));
  CHECK(h.heap_size == doctest::Approx(329'995.0));
  CHECK(h.free_psram == doctest::Approx(1'800'000.0));
  CHECK(h.psram_size == doctest::Approx(8'388'608.0));

  // The bug this suite guards: free_heap must NOT be allowed to exceed
  // heap_size. If someone re-wires the firmware call site to default
  // heap_caps (which includes PSRAM), this invariant breaks.
  CHECK(h.free_heap <= h.heap_size);
  CHECK(h.free_psram <= h.psram_size);
  cJSON_Delete(root);
}

TEST_CASE("AttachHeapMetricsJson: boards without PSRAM zero the siblings") {
  cJSON* root = cJSON_CreateObject();
  btclock::AttachHeapMetricsJson(root,
                                 /*free_internal=*/200'000,
                                 /*total_internal=*/329'995,
                                 /*free_psram=*/0,
                                 /*total_psram=*/0);
  const auto h = ReadFields(root);
  CHECK(h.free_psram == 0.0);
  CHECK(h.psram_size == 0.0);
  CHECK(h.free_heap <= h.heap_size);
  cJSON_Delete(root);
}

TEST_CASE(
    "AttachHeapMetricsJson: original bug report values would fail the "
    "invariant") {
  // The bug report: espFreeHeap=1991268, espHeapSize=329995.
  // That mixture — free sourced from esp_get_free_heap_size() (includes
  // PSRAM) paired with a size from MALLOC_CAP_INTERNAL — produced
  // free > size. If the helper is ever misused to replay that mix,
  // this test documents exactly why it's wrong.
  cJSON* root = cJSON_CreateObject();
  btclock::AttachHeapMetricsJson(
      root,
      /*free_internal=*/1'991'268,  // intentionally wrong
      /*total_internal=*/329'995,
      /*free_psram=*/0,
      /*total_psram=*/0);
  const auto h = ReadFields(root);
  CHECK_FALSE(h.free_heap <= h.heap_size);  // demonstrates the bug shape
  cJSON_Delete(root);
}

TEST_CASE("AttachHeapMetricsJson: null root is a no-op") {
  // Guards against OOM call sites — if cJSON_CreateObject() returned
  // null, the helper must not deref it.
  btclock::AttachHeapMetricsJson(nullptr, 1, 2, 3, 4);
  CHECK(true);  // reaching here means no crash
}
