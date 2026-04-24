// Boot-path dispatch: AP mode vs STA.
//
// After Wi-Fi + panels + screen-manager are up, one of two paths:
//   - AP / provisioning mode: render the portal overlay (QR + SSID +
//     pass) on the panels and skip all network data wiring. The
//     control API still starts after us so the user can reach the
//     device over the AP's own httpd.
//   - STA mode: wire data sources (btclock / nostr / pool / bitaxe)
//     and the zap-listener stack.
//
// Reads ctx.wifi->is_ap_mode() to pick the branch.

#pragma once

namespace btclock {

struct AppCtx;

void DispatchBootPath(AppCtx& ctx);

}  // namespace btclock
