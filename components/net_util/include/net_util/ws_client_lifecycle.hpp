// Safe shutdown for esp_websocket_client.
//
// `esp_websocket_client_stop()` returns as soon as the client task
// raises STOPPED_BIT, but the task then keeps reading two fields of
// the client struct (`state`, `selected_for_destroying`) before it
// calls `vTaskDelete(NULL)`. Calling `esp_websocket_client_destroy()`
// immediately after `_stop()` returns therefore frees the client
// struct while the task is still dereferencing it — a use-after-free
// that HEAP_POISONING_COMPREHENSIVE turns into a deterministic
// LoadProhibitedCause panic at `esp_event_loop_delete(0xfefefefe)`,
// because the freed-and-poisoned `client->event_handle` is read on
// the task's second pass through `destroy_and_free_resources()`.
//
// Reproduced 2026-05-23 on Rev B by hammering /api/restart_datasources
// — see .coredumps/2026-05-23-revb-ws-uaf/. The earlier flaky httpd
// crashes filed under bd btclock_v4-28n were almost certainly the
// same race; without comprehensive poisoning the freed memory
// occasionally still held its pre-free byte values, so the second
// destroy pass silently became a no-op.
//
// 50 ms gives the websocket_task ample time to retire its tail of
// reads + `vTaskDelete(NULL)` (the work between STOPPED_BIT and task
// deletion is two pointer reads + a kernel call — well under a
// millisecond), while still being short enough not to noticeably
// slow a Stop+Start bounce on /api/restart_datasources.

#pragma once

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace btclock {

inline void SafeShutdownWsClient(esp_websocket_client_handle_t client) {
  if (client == nullptr) return;
  esp_websocket_client_stop(client);
  vTaskDelay(pdMS_TO_TICKS(50));
  esp_websocket_client_destroy(client);
}

}  // namespace btclock
