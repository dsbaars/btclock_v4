// SNTP wall-clock sync. Non-blocking — fires in the background so boot
// doesn't wait on it. The data source doesn't need wall-clock time, but
// TLS cert validation and log timestamps do.

#pragma once

#include "esp_err.h"

namespace btclock {

// Initialise SNTP and begin syncing against the given pool. Returns
// ESP_OK on successful init; the actual sync completes asynchronously
// and logs on success.
esp_err_t StartSntpSync(const char* server = "pool.ntp.org");

}  // namespace btclock
