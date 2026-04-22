// Abstract base for BTClock data sources.
//
// A source is anything that produces DataSnapshot fields: the btclock
// WS v2 client, a direct mempool.space WebSocket, a Kraken v2 ticker
// subscription, a Nostr ephemeral-event pool, a mining-pool or Bitaxe
// HTTP poller, a mock for tests. Each source owns its own transport
// (WS client, HTTP task, polling timer, …) — the hub just bookkeeps
// them and merges their outputs.
//
// Lifecycle: main constructs sources, registers them with the hub, then
// calls hub.StartAll(). Each source gets Start(DataHub&) where it opens
// its transport and retains the hub reference for Report() calls as new
// frames arrive. Stop() must be idempotent.

#pragma once

#include "esp_err.h"

namespace btclock {

class DataHub;  // defined in hub.hpp

class DataSource {
 public:
  virtual ~DataSource() = default;

  // Human-readable identifier for logs; not required to be unique.
  virtual const char* name() const = 0;

  // Open transport and begin reporting. Retain `hub` for Report() calls.
  virtual esp_err_t Start(DataHub& hub) = 0;

  // Tear down transport. Must be safe to call multiple times and safe
  // to call when Start() was never invoked.
  virtual esp_err_t Stop() = 0;

  DataSource() = default;
  DataSource(const DataSource&) = delete;
  DataSource& operator=(const DataSource&) = delete;
};

}  // namespace btclock
