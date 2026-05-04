// Pre-flash quiesce ordering for the OTA flow.
//
// Pulled out of init_control_api.cpp's pre_flash_hook so the ordering
// invariant is host-testable. The order is non-obvious: in shared-WSS
// mode the zap listener borrows the Nostr data source's
// SubscriptionManager, and DataHub::StopAll() destroys that data
// source (and the SubscriptionManager with it). The listener MUST be
// stopped before the hub, or the next subs_.Unsubscribe() in the
// listener path will UAF on the freed std::mutex and the device
// hard-faults with an MMU entry error before esp_https_ota_begin even
// runs (Cache error / MMU entry fault, observed on Rev B beta-11).
//
// Stopping the listener first is also correct in dedicated-WSS mode
// — zap_relay doesn't depend on the listener, and Unsubscribe just
// sends NIP-01 CLOSE.
//
// Templated on duck-typed pointers so the host test can pass stubs
// without dragging FreeRTOS / esp_websocket_client into the host
// build.

#pragma once

namespace btclock {

template <class ZapListener, class ZapRelay, class Hub>
void QuiesceOtaPreFlash(ZapListener* zap_listener, ZapRelay* zap_relay,
                        Hub* hub) {
  // 1) Listener first — drops its borrow on the SubscriptionManager
  //    (whether shared or dedicated) and sends NIP-01 CLOSE if the
  //    socket is still up.
  if (zap_listener) zap_listener->Stop();
  // 2) Dedicated zap relay (no-op in shared-WSS mode where this is
  //    null and the data source owns the WSS).
  if (zap_relay) zap_relay->Stop();
  // 3) Data hub — safe to destroy NostrDataSource (and its
  //    SubscriptionManager) now that no listener observes it.
  if (hub) hub->StopAll();
}

}  // namespace btclock
