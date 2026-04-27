// WASM-side stub for `pool_logos::LookupResolved`.
//
// On the device this lives in `main/screens/assets/pool_logo_cache.cpp`
// and consults `/lfs/pool_logos/`. The WASM bundle has no filesystem,
// no PSRAM, and no fetcher, so the resolver returns whatever bitmaps
// we vendor here directly.
//
// The docs renderer (tools/wasm/render_doc_screens.mjs) drives the
// WASM screen stack to produce docs/img/screens/*.png. Without at
// least one vendored logo, the mining-pool screens would always fall
// back to the text-split layout — which doesn't represent how the
// device actually paints those screens once the runtime fetcher has
// pulled a `<pool>.bin` into LittleFS. To keep the docs honest we
// vendor a single representative bitmap (Noderunners — also the
// firmware default after first boot) and return it for that key. All
// other pool keys still fall through to the empty `Lookup()` table.
//
// Bitmap format: 1-bit-per-pixel MSB-first, stride = ceil(W/8) bytes,
// 0 = ink (black), 1 = no ink. 122x122 → 16 bytes/row × 122 rows =
// 1952 bytes, matching the upstream `noderunners.bin` byte-for-byte
// (see tools/wasm/noderunners_logo.bin).
//
// Compiled only into the WASM target via tools/wasm/build.sh — gated
// on BTCLOCK_WASM_BUILD so the file is a no-op if accidentally
// included elsewhere.

#ifdef BTCLOCK_WASM_BUILD

#include "screens/assets/pool_logos.hpp"

namespace btclock {
namespace pool_logos {
namespace {

constexpr std::uint8_t kNoderunnersLogoData[] = {
#include "noderunners_logo_data.inc"
};

constexpr PoolLogo kNoderunnersLogo = {
    "noderunners",
    kNoderunnersLogoData,
    sizeof(kNoderunnersLogoData),
    122,
    122,
};

bool MatchesNoderunners(const std::string& name) {
  if (name.size() != 11) return false;
  for (std::size_t i = 0; i < 11; ++i) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    if (c != "noderunners"[i]) return false;
  }
  return true;
}

}  // namespace

const PoolLogo* LookupResolved(const std::string& pool_name) {
  if (MatchesNoderunners(pool_name)) return &kNoderunnersLogo;
  return Lookup(pool_name);
}

bool HasResolvedLogo(const std::string& pool_name) {
  return LookupResolved(pool_name) != nullptr;
}

}  // namespace pool_logos
}  // namespace btclock

#endif  // BTCLOCK_WASM_BUILD
