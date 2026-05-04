// Bitaxe local-network HTTP data source.
//
// Polls `http://<bitaxeHostname>/api/system/info` every
// `settings/bitaxePollSec` seconds (default 10 s, range 5..300) and
// writes the parsed values into DataSnapshot::BitaxeStats. The cadence
// is re-read from NVS at the top of every poll loop iteration so a
// live PATCH applies on the next tick — no reboot required (bd
// btclock_v4-6hq). The source is NOT a mining-pool poller — it talks
// to the Bitaxe's own HTTP server on the LAN, not to an upstream pool,
// so it does not share the pool_base.cpp plumbing. Plain HTTP (no TLS)
// because the Bitaxe UI doesn't ship with a cert.
//
// Lifecycle (Start / Stop / factory):
//   - At boot main.cpp calls MakeBitaxeSource() which reads
//     `bitaxeEnabled` and `bitaxeHostname` from the settings namespace.
//     Disabled or empty hostname -> returns nullptr (no task created).
//   - A live PATCH to those two prefs takes effect on the next reboot
//     — matches the mining-pool source factory's documented timing.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace btclock {
namespace bitaxe {

class BitaxeSource : public DataSource {
 public:
  explicit BitaxeSource(std::string hostname);
  ~BitaxeSource() override;

  const char* name() const override { return "bitaxe"; }
  esp_err_t Start(DataHub& hub) override;
  esp_err_t Stop() override;

 private:
  static void TaskTrampoline(void* arg);
  void Run();
  void PollOnce();
  // Live-reads `settings/bitaxePollSec`, clamps to schema bounds, returns ms.
  uint32_t poll_interval_ms() const;

  std::string hostname_;
  DataHub* hub_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> stop_{false};
  // Given by Run() right before vTaskDelete(nullptr); waited on by
  // Stop() with a timeout so the OTA pre-flash hook can guarantee the
  // poll task has fully exited before flash erase begins. Without
  // this, a poll mid-HTTP-request could still be alive (and holding
  // mbedtls / lwIP buffers) when esp_ota_write disables cache.
  SemaphoreHandle_t done_ = nullptr;
};

// Factory — returns nullptr when settings/bitaxeEnabled=false or
// settings/bitaxeHostname is empty. Otherwise returns a fresh source
// configured from those prefs.
std::unique_ptr<DataSource> MakeBitaxeSource();

}  // namespace bitaxe
}  // namespace btclock
