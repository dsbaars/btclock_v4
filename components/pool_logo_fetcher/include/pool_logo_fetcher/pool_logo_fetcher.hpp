// On-demand mining-pool logo fetcher.
//
// Restores the v3 download-into-LittleFS model that v4 originally
// dropped in favour of a ~45 KiB compile-time bitmap registry. Triggered
// by the pool selector each time the active pool changes (or at boot).
// Fires the actual download in the background — the renderer keeps
// painting the text-split fallback until the cache file lands, then
// the next paint picks the bitmap up via
// `pool_logos::LookupResolved()`.
//
// Concurrency: at most one fetch in flight per pool name. A second
// `EnqueueFetch("ocean")` while ocean is already downloading is a
// silent no-op (cheap atomic check). Distinct pools can fetch in
// parallel — each queues its own one-shot esp_timer task.
//
// Failure handling: any HTTPS error / non-200 / short read / size
// mismatch leaves no on-disk file. The next pool-selection event is
// the only retry trigger — the fetcher does NOT loop.
//
// Format on disk: header-less raw 1-bpp bitmap bytes, exactly as
// shipped by the upstream `mining-pool-logos` repo. The dimensions
// for parsing come from `PoolBase::logo_width()` / `logo_height()` on
// the active source AND from the static `pool_logos::LookupMeta`
// table — both must agree. The fetcher refuses to write a file whose
// HTTP body length differs from `width*height/8`, so a mis-pinned URL
// (HTML 404 page, redirect to login, etc.) can't poison the cache.

#pragma once

#include <string>

#include "esp_err.h"

namespace btclock {
namespace pool_logos {

// Kick off a background fetch for `pool_name` if no cache file exists
// AND the pool has logo metadata in `LookupMeta`. Returns ESP_OK if
// the fetch was queued, ESP_ERR_NOT_FOUND for unknown pools,
// ESP_ERR_INVALID_STATE if a fetch is already in flight for this
// name. Cheap to call from the rendering path — never blocks.
esp_err_t EnqueueFetch(const std::string& pool_name);

// Synchronous variant — runs on the caller's task. Used by the
// `POST /api/action/clear_pool_logos` follow-up so the user can ask
// for a forced re-download. Most callers should prefer EnqueueFetch.
esp_err_t FetchNow(const std::string& pool_name);

// Delete every cached `.bin` under `/lfs/pool_logos/`. Called by
// `POST /api/action/clear_pool_logos`. Returns the number of files
// removed (0 on a clean dir is fine; negative on a fatal FS error).
int ClearAllCached();

}  // namespace pool_logos
}  // namespace btclock
