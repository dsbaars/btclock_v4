// Control-server (/api/* + /events SSE) + OTA manager wire-up.
//
// Instantiates the five webserver-component adapters (bridging
// FrontlightController / led_controller API / dnd singleton /
// LightSensor / ScreenManager to the Iface types the webserver
// speaks), assembles ControlServer::Config from ctx + the
// catalogs.hpp constexpr arrays, and starts both the ControlServer
// and its attached SseServer.
//
// After the server is up, re-hooks DataHub::SetOnUpdate so fresh
// snapshots fan out to SSE subscribers as well as waking the main
// task. Also kicks the OtaManager with a read-through closure for
// the release URL, so PATCH /api/settings gitReleaseUrl takes effect
// on the next pull-OTA attempt without a reboot.
//
// No-op in AP mode — the provisioning portal owns its own httpd.

#pragma once

namespace btclock {

struct AppCtx;

void InitControlApi(AppCtx& ctx);

// Re-publish the cached LiveStatus snapshot to the control server.
// Called from the event loop whenever the render or timer state
// changes so /api/status readers (SSE + polling) see the update.
void PublishStatus(AppCtx& ctx);

}  // namespace btclock
