#include "app/boot/init_cjson_psram.hpp"

#include "cJSON.h"
#include "esp_heap_caps.h"

namespace btclock {
namespace {

// cJSON makes many small allocations per parse (one per node), every
// one well under the 16 KB CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// threshold — so without this hook every cJSON_Parse drains internal
// heap. There are 18+ cJSON_Parse call sites across mining-pool
// parsers, settings, control_server, OTA, and the Nostr/mempool
// source. Routing them to PSRAM moves the load to an 8 MB pool with
// gobs of headroom and removes a class of slow internal-heap drift
// that's invisible in fragmentation metrics (each alloc is small;
// largest_free_block stays put).
void* CjsonPsramMalloc(size_t sz) {
  return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void CjsonPsramFree(void* ptr) {
  heap_caps_free(ptr);
}

}  // namespace

void InitCjsonPsram() {
  cJSON_Hooks hooks{};
  hooks.malloc_fn = &CjsonPsramMalloc;
  hooks.free_fn = &CjsonPsramFree;
  cJSON_InitHooks(&hooks);
}

}  // namespace btclock
