#include "app/boot/init_boot_path.hpp"

#include "app/app_ctx.hpp"
#include "app/boot/init_zap_listener.hpp"
#include "provisioning_ui.hpp"
#include "sources/sources.hpp"
#include "wifi.hpp"

namespace btclock {

void DispatchBootPath(AppCtx& ctx) {
  if (ctx.wifi->is_ap_mode()) {
    RenderProvisioningScreen(ctx.panels, AppCtx::fb_storage(), ctx.fonts,
                             ctx.ap_ssid, ctx.ap_pw);
    return;
  }
  WireDataSources(ctx);
  InitZapListener(ctx);
}

}  // namespace btclock
