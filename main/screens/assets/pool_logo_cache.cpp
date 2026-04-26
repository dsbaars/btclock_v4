// Pool-logo cache resolver — target-only.
//
// On the device the renderer goes through `LookupResolved(name)` (below)
// which prefers a fetched copy under `/lfs/pool_logos/<file>` over the
// vendored `Lookup()` and falls back to text when neither hits. The
// fetched buffer is held in a tiny one-slot LRU keyed by pool name —
// only the active pool needs to be in memory, and the renderer asks
// for the same name on every paint, so the slot is essentially never
// evicted in steady state.
//
// Memory: cached bitmaps live in PSRAM (`heap_caps_malloc_prefer`
// SPIRAM-first, internal fallback). Worst-case footprint is one
// 1-bpp 250x122 page = ~3.8 KiB; the actual ocean/noderunners/gobrrr
// 122x122 logos are ~1.9 KiB each. The slot is owned by a static
// unique_ptr with a custom deleter so cold paths never leak.
//
// Host tests get the simpler `HasResolvedLogo` stub from
// `pool_logo_cache_host.cpp` instead.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "screens/assets/pool_logos.hpp"

namespace btclock {
namespace pool_logos {

namespace {

constexpr const char* kTag = "pool.logos.cache";
constexpr const char* kCacheDir = "/lfs/pool_logos";

// Build the on-disk path. Filename comes from the metadata table —
// callers must check for nullptr before invoking.
std::string CachePath(const char* filename) {
  std::string out = kCacheDir;
  out.push_back('/');
  out.append(filename);
  return out;
}

bool FileExists(const std::string& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return false;
  return S_ISREG(st.st_mode) && st.st_size > 0;
}

// PSRAM-friendly buffer. The destructor frees through heap_caps_free
// because heap_caps_malloc_prefer / heap_caps_malloc allocations can
// land in either internal or external heap depending on availability.
struct PsramFreeDeleter {
  void operator()(std::uint8_t* p) const noexcept {
    if (p) heap_caps_free(p);
  }
};
using PsramBuffer = std::unique_ptr<std::uint8_t, PsramFreeDeleter>;

// One-slot cache. Holds at most one pool's bitmap in memory because
// the renderer re-resolves the same active pool on every paint and
// the screen-rotation path won't toggle pools mid-frame. Keyed by
// pool name (lowercased ASCII to match the rest of the registry).
struct CacheSlot {
  std::string key;
  PsramBuffer bytes;
  std::size_t size = 0;
  int width = 0;
  int height = 0;
  PoolLogo view{};
};

std::mutex& SlotMutex() {
  static std::mutex m;
  return m;
}

CacheSlot& Slot() {
  static CacheSlot s;
  return s;
}

// Lower-case fold — same shape as IEquals in pool_logos.cpp.
std::string FoldKey(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    out.push_back(c);
  }
  return out;
}

// Read the entire file into a freshly-allocated PSRAM buffer.
// Returns nullptr on any error — caller falls back to vendored.
PsramBuffer ReadIntoPsram(const std::string& path, std::size_t expected,
                          std::size_t* read_out) {
  *read_out = 0;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    ESP_LOGW(kTag, "fopen('%s') failed", path.c_str());
    return {};
  }
  // Allocate exactly the file size. PSRAM is preferred — the bitmap
  // is touched once per panel paint and never written, so PSRAM
  // latency is irrelevant. If PSRAM is exhausted (unlikely, ~2 KiB
  // per logo) we fall back to internal heap.
  PsramBuffer buf(static_cast<std::uint8_t*>(heap_caps_malloc_prefer(
      expected, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT)));
  if (!buf) {
    ESP_LOGW(kTag, "alloc %u bytes failed", static_cast<unsigned>(expected));
    std::fclose(f);
    return {};
  }
  const std::size_t n = std::fread(buf.get(), 1, expected, f);
  std::fclose(f);
  if (n != expected) {
    ESP_LOGW(kTag, "short read on '%s': got %u expected %u", path.c_str(),
             static_cast<unsigned>(n), static_cast<unsigned>(expected));
    return {};
  }
  *read_out = n;
  return buf;
}

// Try to (re)populate the cache slot from disk for `meta`. Returns
// the PoolLogo view on success, nullptr on miss / error.
const PoolLogo* TryLoadFromCache(const std::string& folded,
                                 const PoolLogoMeta& meta) {
  const std::string path = CachePath(meta.filename);
  if (!FileExists(path)) return nullptr;
  // Upstream `.bin` files are header-less raw bytes:
  // expected = stride * height where stride = ceil(width/8).
  const std::size_t stride = static_cast<std::size_t>((meta.width + 7) / 8);
  const std::size_t expected = stride * static_cast<std::size_t>(meta.height);

  std::lock_guard<std::mutex> lk(SlotMutex());
  CacheSlot& s = Slot();
  if (s.key == folded && s.bytes && s.size == expected) {
    return &s.view;
  }
  std::size_t got = 0;
  PsramBuffer buf = ReadIntoPsram(path, expected, &got);
  if (!buf) return nullptr;

  s.key = folded;
  s.bytes = std::move(buf);
  s.size = got;
  s.width = meta.width;
  s.height = meta.height;
  s.view.key = meta.key;
  s.view.bitmap = s.bytes.get();
  s.view.bitmap_size = s.size;
  s.view.width = s.width;
  s.view.height = s.height;
  ESP_LOGI(kTag, "cache hit '%s': %dx%d (%u bytes)", meta.key, meta.width,
           meta.height, static_cast<unsigned>(s.size));
  return &s.view;
}

}  // namespace

bool HasResolvedLogo(const std::string& pool_name) {
  if (pool_name.empty()) return false;
  if (Lookup(pool_name) != nullptr) return true;
  if (const PoolLogoMeta* m = LookupMeta(pool_name)) {
    const std::string path = CachePath(m->filename);
    return FileExists(path);
  }
  return false;
}

// Cache-aware resolver: prefer LittleFS cache → vendored → nullptr.
// Defined here (not in the header) so host tests don't need it; the
// only target caller is mining_pool.cpp (renderer hot path).
const PoolLogo* LookupResolved(const std::string& pool_name) {
  if (pool_name.empty()) return nullptr;
  if (const PoolLogoMeta* m = LookupMeta(pool_name)) {
    const std::string folded = FoldKey(pool_name);
    if (const PoolLogo* hit = TryLoadFromCache(folded, *m)) return hit;
  }
  return Lookup(pool_name);
}

// Drop the cached bitmap (if any). Used by the
// `POST /api/action/clear_pool_logos` endpoint after it `unlink`s the
// on-disk files; otherwise the next paint would still see the stale
// in-memory copy until a different pool is selected.
void EvictCacheSlot() {
  std::lock_guard<std::mutex> lk(SlotMutex());
  Slot() = CacheSlot{};
}

}  // namespace pool_logos
}  // namespace btclock
