// Shared helper that emits the four canonical heap/PSRAM fields on a
// cJSON status payload. Split out of control_server.cpp so both the
// REST handler and the SSE status builder route through the same code
// path, and so the shape can be pinned from host tests (no IDF heap
// APIs leak into this header — callers pass raw byte counts).
//
// Field contract (matches what `/api/status` and `/api/system_status`
// publish, and what the WebUI reads):
//
//   - "espFreeHeap"          : free bytes in INTERNAL SRAM only
//   - "espHeapSize"          : total bytes of INTERNAL SRAM
//   - "espMinFreeHeap"       : lifetime-minimum free bytes in INTERNAL SRAM
//                              (low-water mark; unmoving once seen).
//   - "espLargestFreeBlock"  : largest single contiguous block (bytes) that
//                              can satisfy a DMA-capable internal alloc
//                              right now — the practical ceiling on EPD
//                              SPI/DMA, WiFi/LWIP buffers, etc. Drops
//                              below `espFreeHeap` once internal SRAM
//                              fragments. Watch this to spot fragmentation
//                              that's silently breaking the EPD render.
//   - "espFreePsram"         : free bytes in PSRAM (0 if absent /
//   uninitialised)
//   - "espPsramSize"         : total bytes of PSRAM (0 if absent)
//
// Historical note: pre-fix, "espFreeHeap" was sourced from
// esp_get_free_heap_size(), which on ESP32-S3 includes PSRAM. That
// produced free > size, which is nonsense. This helper codifies the
// post-fix contract: "heap" means internal SRAM, PSRAM is a sibling.
// See bug/heap-metric-internal-vs-psram.

#pragma once

#include <cstddef>

#include "cJSON.h"

namespace btclock {

inline void AttachHeapMetricsJson(cJSON* root, std::size_t free_internal,
                                  std::size_t total_internal,
                                  std::size_t free_psram,
                                  std::size_t total_psram,
                                  std::size_t min_free_internal = 0,
                                  std::size_t largest_free_internal_dma = 0) {
  if (!root) return;
  cJSON_AddNumberToObject(root, "espFreeHeap",
                          static_cast<double>(free_internal));
  cJSON_AddNumberToObject(root, "espHeapSize",
                          static_cast<double>(total_internal));
  cJSON_AddNumberToObject(root, "espMinFreeHeap",
                          static_cast<double>(min_free_internal));
  cJSON_AddNumberToObject(root, "espLargestFreeBlock",
                          static_cast<double>(largest_free_internal_dma));
  cJSON_AddNumberToObject(root, "espFreePsram",
                          static_cast<double>(free_psram));
  cJSON_AddNumberToObject(root, "espPsramSize",
                          static_cast<double>(total_psram));
}

}  // namespace btclock
