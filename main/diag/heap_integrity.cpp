#include "diag/heap_integrity.hpp"

#include <atomic>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace btclock {
namespace {

constexpr const char* kTag = "heap-integrity";

// 30 s strikes a balance: short enough that a corruption detected here
// is still close in time to whatever caused it, long enough that the
// CPU cost of walking ~300 allocator blocks is invisible. Tunable via
// the Kconfig knob below.
constexpr TickType_t kCheckInterval =
    pdMS_TO_TICKS(CONFIG_BTCLOCK_HEAP_INTEGRITY_INTERVAL_MS);

std::atomic<bool> g_started{false};

[[noreturn]] void MonitorTask(void*) {
  // First pass after a short settle so we're well past boot-time
  // allocator churn. Failing the check during boot would mean the
  // device never makes it to a usable state — drop a clearer log
  // marker than the generic panic banner so the operator can grep
  // `HEAP_INTEGRITY_FAILED` straight out of the coredump strings.
  vTaskDelay(pdMS_TO_TICKS(5000));
  for (;;) {
    if (!heap_caps_check_integrity_all(/*print_errors=*/true)) {
      ESP_LOGE(kTag, "HEAP_INTEGRITY_FAILED — aborting to capture coredump");
      // Give the log line a chance to flush over USB-Serial-JTAG
      // before the panic dispatch tears the world down.
      vTaskDelay(pdMS_TO_TICKS(50));
      abort();
    }
    vTaskDelay(kCheckInterval);
  }
}

}  // namespace

void InitHeapIntegrityMonitor() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;
  // 4 KiB stack is generous for the heap walker — IDF's heap-check
  // implementation is recursion-free and uses no large locals, but
  // ESP_LOGE/printf paths can drop ~512 B and we want headroom.
  // tskIDLE_PRIORITY + 1 keeps the check off the critical path; the
  // walker preempts cleanly via vTaskDelay between passes.
  xTaskCreate(&MonitorTask, "heap-integrity", 4096, nullptr,
              tskIDLE_PRIORITY + 1, nullptr);
}

}  // namespace btclock
