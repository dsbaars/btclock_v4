#include "app/time_sync.hpp"

#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

namespace btclock {
namespace {
constexpr const char* kTag = "sntp";

void OnSync(struct timeval* tv) {
  ESP_LOGI(kTag, "synced: %lld.%03lds",
           static_cast<long long>(tv->tv_sec),
           static_cast<long>(tv->tv_usec / 1000));
}
}  // namespace

esp_err_t StartSntpSync(const char* server) {
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
  cfg.sync_cb = &OnSync;
  return esp_netif_sntp_init(&cfg);
}

}  // namespace btclock
