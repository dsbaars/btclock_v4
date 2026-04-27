#include "app/boot/init_mdns.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "app/app_ctx.hpp"
#include "board/board.hpp"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "net_util/hostname.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "mdns";

// Tracks whether mdns_init() succeeded previously, so ReinitMdns knows to
// call mdns_free() before re-publishing. Without this guard the second
// call leaks the responder task and the new advert silently fails. The
// boot-time entry leaves it false on early-return paths (AP mode, pref
// disabled) so a later PATCH still takes the cold-init branch.
std::atomic<bool> g_mdns_started{false};

// Build the hostname advertised over mDNS. Delegates to the shared
// helper in components/net_util so the /api/settings emitter and this
// init path stay in lockstep — drift between them meant the WebUI
// reported a name nobody could actually ping.
std::string BuildHostname() {
  btclock::Prefs p(prefs::kSettingsNs);
  std::string prefix = btclock::settings::ReadString(p, prefs::kHostnamePrefix);
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  return btclock::net_util::ComputeHostname(prefix, mac);
}

// Which HW variant this firmware is built for. Surfaced as a TXT
// record so an avahi-browse pass can tell Rev A from Rev B from V8.
// Composed orthogonally: board token (REV_A/REV_B/V8) + optional panel
// suffix when panel ≠ default(2.13"). Keeps the bare-board tags stable
// for the common case while still letting the WebUI distinguish e.g.
// Rev B 7.5" from Rev B 2.13" (same MAC strap, so the board alone
// wouldn't disambiguate).
const char* BoardTxtValue() {
#if defined(BTCLOCK_BOARD_REV_A)
#if defined(BTCLOCK_PANEL_2_9)
  return "REV_A_29";
#elif defined(BTCLOCK_PANEL_7_5)
  return "REV_A_75";
#else
  return "REV_A";
#endif
#elif defined(BTCLOCK_BOARD_REV_B)
#if defined(BTCLOCK_PANEL_2_9)
  return "REV_B_29";
#elif defined(BTCLOCK_PANEL_7_5)
  return "REV_B_75";
#else
  return "REV_B";
#endif
#elif defined(BTCLOCK_BOARD_V8)
#if defined(BTCLOCK_PANEL_2_9)
  return "V8_29";
#elif defined(BTCLOCK_PANEL_7_5)
  return "V8_75";
#else
  return "V8";
#endif
#else
  return "unknown";
#endif
}

// Spin up the responder + register both service adverts. Reads
// `mdnsEnabled` / `hostnamePrefix` fresh from NVS so a PATCH-driven
// re-init picks up whatever the settings handler just persisted.
// Marks `g_mdns_started` true on success so the next call knows to free
// the previous responder first. Logs-and-returns on failure — a stale
// advert is preferable to crashing boot or the httpd worker.
void StartMdnsAdvertisement() {
  btclock::Prefs p(prefs::kSettingsNs);
  // Default-true to match DEFAULT_MDNS_ENABLED in the v3 firmware.
  if (!btclock::settings::ReadBool(p, prefs::kMdnsEnabled)) {
    ESP_LOGI(kTag, "mdnsEnabled=false; skipping advertisement");
    return;
  }

  const std::string hostname = BuildHostname();

  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "mdns_init failed: %s", esp_err_to_name(err));
    return;
  }
  // hostname is the left-hand label of "<name>.local" — any existing
  // entry with the same name would cause Bonjour to publish a
  // numbered variant, so set ours explicitly before adding services.
  if ((err = mdns_hostname_set(hostname.c_str())) != ESP_OK) {
    ESP_LOGW(kTag, "mdns_hostname_set('%s') failed: %s", hostname.c_str(),
             esp_err_to_name(err));
    mdns_free();
    return;
  }
  // Instance name is the human-readable string shown in Bonjour
  // browsers. Keep it identical to the hostname so the two columns
  // match — the WebUI scan helper searches on this.
  if ((err = mdns_instance_name_set(hostname.c_str())) != ESP_OK) {
    ESP_LOGW(kTag, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
    // Non-fatal — continue with service registration.
  }

  // Compose TXT records. Kept compatible with the v3 firmware's shape
  // (model/version/rev/hw_rev) and extended with the v4-era keys
  // (path/board) called out in the task brief.
  const char* git_rev = "";
  const esp_app_desc_t* desc = esp_app_get_description();
  if (desc && desc->version[0] != '\0') git_rev = desc->version;
  const char* board = BoardTxtValue();

  const mdns_txt_item_t txt[] = {
      {"path", "/"},    {"version", "4.0"},
      {"board", board}, {"model", "BTClock"},
      {"rev", git_rev}, {"hw_rev", board::kHardwareName},
  };
  constexpr size_t kTxtCount = sizeof(txt) / sizeof(txt[0]);

  // http._tcp matches the v3 firmware. The webserver listens on port 80
  // (esp_http_server default) — keep that literal until the port
  // becomes configurable.
  constexpr uint16_t kHttpPort = 80;
  if ((err = mdns_service_add(hostname.c_str(), "_http", "_tcp", kHttpPort,
                              const_cast<mdns_txt_item_t*>(txt), kTxtCount)) !=
      ESP_OK) {
    ESP_LOGW(kTag, "mdns_service_add _http failed: %s", esp_err_to_name(err));
    mdns_free();
    return;
  }
  // Parallel advert under a BTClock-specific service type so
  // dedicated tooling can filter our devices without walking every
  // HTTP host on the LAN.
  if ((err = mdns_service_add(hostname.c_str(), "_btclock", "_tcp", kHttpPort,
                              const_cast<mdns_txt_item_t*>(txt), kTxtCount)) !=
      ESP_OK) {
    ESP_LOGW(kTag, "mdns_service_add _btclock failed: %s",
             esp_err_to_name(err));
    // Not fatal — primary _http advert already succeeded.
  }

  g_mdns_started.store(true, std::memory_order_release);
  ESP_LOGI(kTag, "advertising %s.local board=%s", hostname.c_str(), board);
}

}  // namespace

void InitMdns(AppCtx& ctx) {
  // SoftAP (provisioning) mode owns the captive-portal DNS hijack on
  // 53/udp and the WebUI runs locally — there's no LAN to advertise
  // into and starting mDNS would stall on netif discovery.
  if (!ctx.wifi || ctx.wifi->is_ap_mode()) return;
  StartMdnsAdvertisement();
}

void ReinitMdns() {
  // PATCH /api/settings on `mdnsEnabled` / `hostnamePrefix` lands here.
  // Tear down any previous responder first — IDF's mdns lib refuses a
  // second mdns_init without an intervening mdns_free, and the previous
  // advert would otherwise keep serving the stale hostname / TXT set
  // alongside the new one. Safe to call when nothing was started yet
  // (the AP-mode and disabled-pref boot paths leave the flag false).
  if (g_mdns_started.exchange(false, std::memory_order_acq_rel)) {
    mdns_free();
  }
  StartMdnsAdvertisement();
}

}  // namespace btclock
