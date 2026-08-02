#include "sources/mempool_parse.hpp"

#include <cstdint>

#include "cJSON.h"

namespace btclock {

bool TipHeightFromBlocksArray(const cJSON* blocks, std::uint32_t* out) {
  if (out == nullptr || !cJSON_IsArray(blocks)) return false;
  bool found = false;
  std::uint32_t tip = 0;
  const cJSON* entry = nullptr;
  cJSON_ArrayForEach(entry, blocks) {
    if (!cJSON_IsObject(entry)) continue;
    const cJSON* h = cJSON_GetObjectItemCaseSensitive(entry, "height");
    if (!cJSON_IsNumber(h) || h->valuedouble < 0) continue;
    const auto height = static_cast<std::uint32_t>(h->valuedouble);
    if (!found || height > tip) {
      tip = height;
      found = true;
    }
  }
  if (found) *out = tip;
  return found;
}

}  // namespace btclock
