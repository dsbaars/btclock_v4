#include "app/network_coordinator.hpp"

#include "app/app_ctx.hpp"
#include "app/boot/init_network.hpp"
#include "app/time_sync.hpp"
#include "esp_log.h"
#include "io/led_controller.hpp"
#include "io/provisioning_fallback.hpp"
#include "sources/sources.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "net-coord";
// Backstop for "STA connected but data source never delivers": finish the
// boot anyway after this long so the device becomes usable (and the LED
// watchdog can then surface the data fault). Matches the old
// FinishWiringDataSources first-blockheight wait.
constexpr uint32_t kBootDataTimeoutMs = 30'000;
}  // namespace

NetworkCoordinator::NetworkCoordinator(AppCtx& ctx, uint32_t grace_ms)
    : ctx_(ctx), grace_ms_(grace_ms) {}

void NetworkCoordinator::Tick(Wifi& wifi, uint32_t now_ms) {
  if (!boot_stamped_) {
    boot_ms_ = now_ms;
    boot_stamped_ = true;
  }
  const bool connected = wifi.state() == Wifi::State::kConnected;

  // Teardown runs BEFORE the first-connect wiring so is_ap_mode() is
  // already false when the wiring (WireDataSources / InitNwc /
  // InitZapListener all bail in AP mode) runs in this same tick.
  if (ShouldTeardownAp(ap_up_, connected)) {
    ESP_LOGI(kTag, "STA reconnected; tearing down fallback portal");
    StopProvisioningPortal(ctx_);
    ap_up_ = false;
  }

  if (connected && !connected_once_) {
    connected_once_ = true;
    connect_ms_ = now_ms;
    // Green connect-success flash. The boot tail (spinner stop + first
    // render + buttons) is NOT run yet — the spinner keeps spinning until
    // the data source actually delivers data (below). The bits that needed
    // a live connection are done here: SNTP + the upstream currency fetch.
    PostLedEffect(LedEffect::kWifiConnectSuccess);
    StartSntpSync();
    RefreshUpstreamCurrencies(ctx_);
    ESP_LOGI(kTag, "first STA connect: SNTP + currency refresh; awaiting data");
  }

  // Finish the boot once the first data has landed — the spinner spins
  // until the data source is connected AND has pushed a snapshot. A
  // timeout backstops a device whose data source can't connect so it still
  // becomes usable (and the LED watchdog, gated on FinishBoot having run,
  // then surfaces the data fault). Mirrors the old FinishWiringDataSources
  // 30 s wait, now keyed on the real GOT_IP instead of blocking boot.
  if (connected_once_ && !boot_finalized_) {
    const bool have_data =
        ctx_.hub && ctx_.hub->GetSnapshot().block_height.has_value();
    const bool timed_out = (now_ms - connect_ms_) >= kBootDataTimeoutMs;
    if (have_data || timed_out) {
      boot_finalized_ = true;
      FinishBoot(ctx_);
      ESP_LOGI(kTag, "boot finalized (%s)", have_data ? "data" : "timeout");
    }
  }

  if (ShouldStartFallbackAp(connected_once_, ap_up_, connected, now_ms,
                            boot_ms_, grace_ms_)) {
    ESP_LOGW(kTag,
             "STA not connected within %u ms grace; bringing up concurrent "
             "provisioning portal (STA keeps retrying the saved network)",
             static_cast<unsigned>(grace_ms_));
    StartProvisioningPortal(ctx_, /*render=*/true);
    ap_up_ = true;
  }
}

}  // namespace btclock
