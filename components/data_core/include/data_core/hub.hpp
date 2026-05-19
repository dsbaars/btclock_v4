// Data hub — registry of DataSources, merge of partial snapshots.
//
// The hub owns a vector of DataSources and a single live DataSnapshot.
// Sources call hub.Report(partial) from their own task whenever new
// data arrives; the hub merges non-empty fields under a mutex and fires
// an application-supplied callback iff anything actually changed.
//
// Thread model:
//   * Report() is called from the reporting source's task (WS event
//     task, HTTP poll task, Nostr relay task, …).
//   * OnUpdate fires on that same task, but with the mutex released and
//     a *copy* of the merged snapshot — so the callback can run real
//     work without risking reentrancy on Report(). Keep it fast anyway;
//     post to a queue or task-notify if heavy lifting is needed.
//   * GetSnapshot() copies under the mutex; safe from any task.
//
// Conflict policy: if two sources publish the same field (e.g. both
// btclock WS and mempool.space push block_height), last writer wins.
// Main selects which sources run based on user prefs — the hub has no
// opinion and never arbitrates between sources.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "data_core/snapshot.hpp"
#include "data_core/source.hpp"
#include "esp_err.h"

namespace btclock {

class DataHub {
 public:
  using UpdateCallback = std::function<void(const DataSnapshot&)>;

  DataHub() = default;
  ~DataHub();

  DataHub(const DataHub&) = delete;
  DataHub& operator=(const DataHub&) = delete;

  // Register a source. Order is not significant (no conflict arbitration).
  void AddSource(std::unique_ptr<DataSource> src);

  // Start all registered sources. Returns the first non-OK result seen;
  // already-started sources are left running. Call StopAll() before
  // retrying on failure.
  esp_err_t StartAll();

  // Stop all sources, best-effort (continues past per-source errors).
  void StopAll();

  DataSnapshot GetSnapshot() const;

  // Set / clear the update callback (pass `{}` to clear).
  void SetOnUpdate(UpdateCallback cb);

  // Called by sources. `partial` carries only the fields/currencies
  // that changed — unset optionals and absent map keys are ignored.
  void Report(const DataSnapshot& partial);

  // Force the update callback to fire with the current snapshot, even
  // when nothing changed. Used by source WS CONNECTED handlers after a
  // (re-)connect: the next inbound frame may carry the same height as
  // the cached one (no new block during the gap), in which case
  // Report()'s Merge() short-circuits and the renderer never sees a
  // notify — leaving a stale display behind a "healthy" connection.
  // ForceNotify() bypasses the dedupe so reconnect always re-engages
  // the screen pipeline.
  void ForceNotify();

 private:
  mutable std::mutex mu_;
  DataSnapshot snapshot_;
  UpdateCallback on_update_;
  std::vector<std::unique_ptr<DataSource>> sources_;
};

}  // namespace btclock
