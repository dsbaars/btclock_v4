#include "app/boot/init_boot_path.hpp"

#include "app/app_ctx.hpp"
#include "app/boot/init_nwc.hpp"
#include "app/boot/init_zap_listener.hpp"
#include "io/led_controller.hpp"
#include "provisioning_ui.hpp"
#include "sources/sources.hpp"
#include "wifi.hpp"

namespace btclock {

void DispatchBootPath(AppCtx& ctx) {
  if (ctx.wifi->is_ap_mode()) {
    // Pure-provisioning boot (empty creds): the portal/DNS were already
    // brought up in InitNetwork; paint the provisioning UI now that fonts
    // are loaded, and hand the LEDs to the provisioning breathe.
    RenderProvisioningScreen(ctx.panels, AppCtx::fb_storage(), ctx.fonts,
                             ctx.ap_ssid, ctx.ap_pw);
    PostLedEffect(LedEffect::kSetProvisioning);
    return;
  }
  // Have-creds boot: wire the data sources, zap listener and NWC now. They
  // spawn async clients that retry until the network is up — the boot is
  // non-blocking, so there may be no connection yet, but wiring them here
  // (before InitControlApi) keeps the control API's hub/source back-refs
  // and connection-status callbacks valid. The blocking pieces that truly
  // need a live connection (SNTP, the upstream currency-catalogue fetch)
  // are deferred to the first STA connect by NetworkCoordinator.
  WireDataSources(ctx);
  InitZapListener(ctx);
  InitNwc(ctx);
}

}  // namespace btclock
