// METADATA-only: no vendored bitmaps — all logos fetched at runtime
// per bd btclock_v4-5yi. The hand-maintained `kLogoMetaTable` below
// must list every pool that ships an upstream `.bin` so the runtime
// fetcher can resolve the URL/dimensions for cache hits, but the
// bitmap blob is no longer baked into `.text`.
//
// First-boot UX before the first HTTPS fetch lands shows the pool
// name as text (renderer falls back via `LookupResolved` returning
// nullptr → text-split path). The default `miningPoolName` was moved
// to `noderunners` (with `poolGlobalStats=true`) so a fresh device
// fetches a real logo on first connect without requiring a PATCH.
//
// Pools that legitimately have no logo (public_pool, ckpool family,
// satoshi_radio, local_public_pool) are absent from the metadata
// table. The renderer paints the text-split fallback for them — same
// shape as v3.
//
// Each cached `/lfs/pool_logos/<file>` is a 1-bpp MSB-first byte
// array with stride=ceil(width/8) bytes/row. Semantics match the v3
// Arduino firmware's `drawInvertedBitmap`: a 0 bit is ink (black
// pixel), a 1 bit is background (no ink / white).

#include "screens/assets/pool_logos.hpp"

#include <cstdint>
#include <cstring>

namespace btclock {
namespace pool_logos {
namespace {

// Hand-maintained — every pool that ships a `<file>.bin` upstream
// must appear here so the runtime fetcher knows where to download
// from and the renderer knows the dimensions. Filenames mirror the
// upstream `mining-pool-logos` repo layout.
constexpr PoolLogoMeta kLogoMetaTable[] = {
    {"braiins", "braiins.bin", 37, 230},
    {"gobrrr_pool", "gobrrr.bin", 122, 122},
    {"noderunners", "noderunners.bin", 122, 122},
    {"ocean", "ocean.bin", 122, 122},
};

}  // namespace

// Case-insensitive equality restricted to ASCII letters — the pool
// keys above are all lowercase ASCII, so a 'Z'->'z' fold is all we
// need. Keeping the comparison local (rather than depending on
// <cctype>'s locale) keeps it safe for the EPD renderer hot path.
static bool IEquals(const char* a, const char* b) {
  while (*a || *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
    if (ca != cb) return false;
  }
  return true;
}

const PoolLogo* Lookup(const std::string& pool_name) {
  (void)pool_name;
  return nullptr;
}

const PoolLogoMeta* LookupMeta(const std::string& pool_name) {
  if (pool_name.empty()) return nullptr;
  for (const auto& entry : kLogoMetaTable) {
    if (IEquals(entry.key, pool_name.c_str())) return &entry;
  }
  return nullptr;
}

}  // namespace pool_logos
}  // namespace btclock
