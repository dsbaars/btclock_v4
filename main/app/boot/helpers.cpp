#include "app/boot/helpers.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "net_util.hpp"
#include "prefs.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "poc";
}  // namespace

int64_t MsNow() { return esp_timer_get_time() / 1000; }

std::string MakeApSsid() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  return btclock::FormatApSsid(mac);
}

std::string MakeOrLoadApPassword(btclock::Prefs& prefs) {
  std::string pw = prefs.GetString("app", "");
  if (pw.size() >= 8) return pw;
  pw = btclock::GenerateApPassword([] { return esp_random(); });
  prefs.SetString("app", pw.c_str());
  prefs.Commit();
  return pw;
}

#ifdef CONFIG_BTCLOCK_LITTLEFS_SELFTEST
void RunLittleFsSelfTest(const char* base_path) {
  const std::string path = std::string(base_path) + "/_bq0_selftest.txt";
  constexpr const char kPayload[] = "btclock-lfs-selftest-v1";

  FILE* wf = std::fopen(path.c_str(), "w");
  if (!wf) {
    ESP_LOGE(kTag, "selftest: fopen(w) '%s' failed", path.c_str());
    return;
  }
  const size_t wn = std::fwrite(kPayload, 1, sizeof(kPayload) - 1, wf);
  std::fclose(wf);
  if (wn != sizeof(kPayload) - 1) {
    ESP_LOGE(kTag, "selftest: short write %u/%u", static_cast<unsigned>(wn),
             static_cast<unsigned>(sizeof(kPayload) - 1));
    std::remove(path.c_str());
    return;
  }

  char buf[sizeof(kPayload)] = {};
  FILE* rf = std::fopen(path.c_str(), "r");
  if (!rf) {
    ESP_LOGE(kTag, "selftest: fopen(r) '%s' failed", path.c_str());
    std::remove(path.c_str());
    return;
  }
  const size_t rn = std::fread(buf, 1, sizeof(buf) - 1, rf);
  std::fclose(rf);

  const bool ok = (rn == sizeof(kPayload) - 1) &&
                  (std::memcmp(buf, kPayload, rn) == 0);
  if (ok) {
    ESP_LOGI(kTag, "selftest: OK (%u bytes round-tripped)",
             static_cast<unsigned>(rn));
  } else {
    ESP_LOGE(kTag, "selftest: MISMATCH rn=%u buf='%.*s'",
             static_cast<unsigned>(rn), static_cast<int>(rn), buf);
  }

  if (std::remove(path.c_str()) != 0) {
    ESP_LOGW(kTag, "selftest: remove '%s' failed", path.c_str());
  }
}
#endif  // CONFIG_BTCLOCK_LITTLEFS_SELFTEST

}  // namespace btclock
