// Bitaxe local-network HTTP data source.
//
// Polls `http://<bitaxeHostname>/api/system/info` every
// `poll_interval_ms` (default 10 s) and writes the parsed values into
// DataSnapshot::BitaxeStats. The source is NOT a mining-pool poller —
// it talks to the Bitaxe's own HTTP server on the LAN, not to an
// upstream pool, so it does not share the pool_base.cpp plumbing.
// Plain HTTP (no TLS) because the Bitaxe UI doesn't ship with a cert.
//
// Lifecycle (Start / Stop / factory):
//   - At boot main.cpp calls MakeBitaxeSource() which reads
//     `bitaxeEnabled` and `bitaxeHostname` from the settings namespace.
//     Disabled or empty hostname -> returns nullptr (no task created).
//   - A live PATCH to those prefs takes effect on the next reboot —
//     matches the mining-pool source factory's documented timing.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "data_core/source.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {
namespace bitaxe {

class BitaxeSource : public DataSource {
 public:
  explicit BitaxeSource(std::string hostname,
                         uint32_t poll_interval_ms = 10 * 1000);
  ~BitaxeSource() override;

  const char* name() const override { return "bitaxe"; }
  esp_err_t Start(DataHub& hub) override;
  esp_err_t Stop() override;

 private:
  static void TaskTrampoline(void* arg);
  void Run();
  void PollOnce();

  std::string hostname_;
  uint32_t poll_interval_ms_;
  DataHub* hub_ = nullptr;
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> stop_{false};
};

// Factory — returns nullptr when settings/bitaxeEnabled=false or
// settings/bitaxeHostname is empty. Otherwise returns a fresh source
// configured from those prefs.
std::unique_ptr<DataSource> MakeBitaxeSource();

}  // namespace bitaxe
}  // namespace btclock
