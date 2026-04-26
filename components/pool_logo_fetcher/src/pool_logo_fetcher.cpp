#include "pool_logo_fetcher/pool_logo_fetcher.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "tls_gate/tls_gate.hpp"

// Forward-declared from main/screens/assets/pool_logos.{hpp,cpp}.
// Linked at the final binary stage — components can't include from
// main/, but the linker resolves these C++ symbols at the same step
// that ties main + components together. Keeping the declarations
// here local makes the fetcher consume only the narrow metadata
// surface (struct + lookup + evict) rather than dragging in the
// renderer's full PoolLogo / Lookup pair.
//
// IMPORTANT: PoolLogoMeta MUST stay byte-identical to the canonical
// definition at main/screens/assets/pool_logos.hpp:64. The linker
// won't catch a struct-shape mismatch — fetcher reads will silently
// corrupt. Promote pool_logos to a real component once the metadata
// surface grows beyond these four fields.
namespace btclock {
namespace pool_logos {

struct PoolLogoMeta {
  const char* key;
  const char* filename;
  int width;
  int height;
};

const PoolLogoMeta* LookupMeta(const std::string& pool_name);
void EvictCacheSlot();

}  // namespace pool_logos
}  // namespace btclock

namespace btclock {
namespace pool_logos {

namespace {

constexpr const char* kTag = "pool.logos.fetch";
constexpr const char* kCacheDir = "/lfs/pool_logos";
constexpr const char* kDefaultLogosUrl =
    "https://git.btclock.dev/btclock/mining-pool-logos/raw/branch/main";

// In-flight set keyed by pool name. Plain mutex-guarded set is enough
// — the contention is lookup-rate (a few per minute), and the entries
// are short-lived strings.
std::mutex& InFlightMutex() {
  static std::mutex m;
  return m;
}
std::unordered_set<std::string>& InFlightSet() {
  static std::unordered_set<std::string> s;
  return s;
}

bool TryClaimSlot(const std::string& key) {
  std::lock_guard<std::mutex> lk(InFlightMutex());
  return InFlightSet().insert(key).second;
}

void ReleaseSlot(const std::string& key) {
  std::lock_guard<std::mutex> lk(InFlightMutex());
  InFlightSet().erase(key);
}

bool EnsureCacheDir() {
  struct stat st {};
  if (::stat(kCacheDir, &st) == 0) {
    return S_ISDIR(st.st_mode);
  }
  if (::mkdir(kCacheDir, 0755) == 0) return true;
  ESP_LOGW(kTag, "mkdir('%s') failed: errno=%d", kCacheDir, errno);
  return false;
}

std::string CachePath(const char* filename) {
  std::string out = kCacheDir;
  out.push_back('/');
  out.append(filename);
  return out;
}

bool FileExists(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  return S_ISREG(st.st_mode);
}

std::string LogosBaseUrl() {
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  std::string url =
      settings.GetString(btclock::prefs::kPoolLogosUrl, kDefaultLogosUrl);
  // Trim trailing '/' so concatenation doesn't yield "//<file>".
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

// HTTP body accumulator backed by PSRAM. Allocates on first byte so
// 0-length responses don't touch heap. Mirrors the OTA / pool_base
// allocation pattern.
struct FetchCtx {
  std::uint8_t* body = nullptr;
  std::size_t size = 0;
  std::size_t cap = 0;
  bool truncated = false;
  bool alloc_failed = false;

  ~FetchCtx() {
    if (body) heap_caps_free(body);
  }
};

esp_err_t HttpEvent(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = static_cast<FetchCtx*>(evt->user_data);
  if (!ctx || !evt->data || evt->data_len <= 0) return ESP_OK;
  if (ctx->truncated || ctx->alloc_failed) return ESP_OK;
  const std::size_t n = static_cast<std::size_t>(evt->data_len);
  if (ctx->size + n > ctx->cap) {
    ctx->truncated = true;
    return ESP_OK;
  }
  if (!ctx->body) {
    ctx->body = static_cast<std::uint8_t*>(heap_caps_malloc_prefer(
        ctx->cap, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_8BIT));
    if (!ctx->body) {
      ctx->alloc_failed = true;
      return ESP_OK;
    }
  }
  std::memcpy(ctx->body + ctx->size, evt->data, n);
  ctx->size += n;
  return ESP_OK;
}

// Atomically write `body` to `path` via a `<path>.tmp` rename so a
// crash mid-write can't leave a half-file the renderer would mis-parse.
bool WriteAtomic(const std::string& path, const std::uint8_t* body,
                 std::size_t n) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    ESP_LOGW(kTag, "fopen('%s') failed: errno=%d", tmp.c_str(), errno);
    return false;
  }
  const std::size_t w = std::fwrite(body, 1, n, f);
  std::fclose(f);
  if (w != n) {
    ESP_LOGW(kTag, "short write %u/%u", static_cast<unsigned>(w),
             static_cast<unsigned>(n));
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ESP_LOGW(kTag, "rename '%s' -> '%s' failed: errno=%d", tmp.c_str(),
             path.c_str(), errno);
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

esp_err_t DoFetch(const PoolLogoMeta& meta) {
  const std::size_t stride = static_cast<std::size_t>((meta.width + 7) / 8);
  const std::size_t expected = stride * static_cast<std::size_t>(meta.height);
  if (expected == 0 || expected > 32 * 1024) {
    ESP_LOGW(kTag, "%s: nonsensical expected size %u", meta.key,
             static_cast<unsigned>(expected));
    return ESP_ERR_INVALID_SIZE;
  }
  if (!EnsureCacheDir()) return ESP_FAIL;

  const std::string url = LogosBaseUrl() + "/" + meta.filename;
  ESP_LOGI(kTag, "%s: fetching %s (%u bytes expected)", meta.key,
           url.c_str(), static_cast<unsigned>(expected));

  FetchCtx ctx;
  // Cap is `expected`. Any extra bytes (HTML 404, redirect body) trip
  // `truncated` and we discard the response — never write a too-big
  // file the renderer would mis-decode.
  ctx.cap = expected;

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = &HttpEvent;
  cfg.user_data = &ctx;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  cfg.disable_auto_redirect = false;
  cfg.max_redirection_count = 5;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) {
    ESP_LOGW(kTag, "%s: http_client_init failed", meta.key);
    return ESP_FAIL;
  }

  esp_err_t err;
  int status = 0;
  {
    // Hold the TLS gate only around `perform`. Same pattern as
    // pool_base / ota_manager — the gate serialises mbedtls handshake
    // bursts so two concurrent HTTPS clients don't both allocate the
    // ~16 KiB IN buffer at once.
    std::lock_guard<std::mutex> lk(btclock::tls_gate::mutex());
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
  }
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    ESP_LOGW(kTag, "%s: perform failed: %s", meta.key, esp_err_to_name(err));
    return err;
  }
  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "%s: HTTP %d", meta.key, status);
    return ESP_FAIL;
  }
  if (ctx.alloc_failed) {
    ESP_LOGW(kTag, "%s: alloc failed", meta.key);
    return ESP_ERR_NO_MEM;
  }
  if (ctx.truncated) {
    ESP_LOGW(kTag, "%s: response exceeded %u bytes", meta.key,
             static_cast<unsigned>(ctx.cap));
    return ESP_ERR_INVALID_SIZE;
  }
  if (ctx.size != expected) {
    ESP_LOGW(kTag, "%s: size mismatch (got %u expected %u)", meta.key,
             static_cast<unsigned>(ctx.size),
             static_cast<unsigned>(expected));
    return ESP_ERR_INVALID_SIZE;
  }
  const std::string path = CachePath(meta.filename);
  if (!WriteAtomic(path, ctx.body, ctx.size)) return ESP_FAIL;

  ESP_LOGI(kTag, "%s: cached %u bytes -> %s", meta.key,
           static_cast<unsigned>(ctx.size), path.c_str());
  // The next paint will pick this up via LookupResolved -> cache miss
  // -> fopen. There's nothing to invalidate in the in-memory cache
  // because the slot starts empty for this pool (we only get here
  // after FileExists returned false).
  return ESP_OK;
}

struct FetchTaskCtx {
  std::string pool_name;
};

void FetchTaskTrampoline(void* arg) {
  std::unique_ptr<FetchTaskCtx> ctx(static_cast<FetchTaskCtx*>(arg));
  if (const PoolLogoMeta* meta = LookupMeta(ctx->pool_name)) {
    (void)DoFetch(*meta);
  } else {
    ESP_LOGW(kTag, "no metadata for '%s'", ctx->pool_name.c_str());
  }
  ReleaseSlot(ctx->pool_name);
  vTaskDelete(nullptr);
}

}  // namespace

esp_err_t EnqueueFetch(const std::string& pool_name) {
  if (pool_name.empty()) return ESP_ERR_INVALID_ARG;
  const PoolLogoMeta* meta = LookupMeta(pool_name);
  if (!meta) return ESP_ERR_NOT_FOUND;
  if (FileExists(CachePath(meta->filename))) {
    ESP_LOGD(kTag, "%s: cache present, skipping fetch", meta->key);
    return ESP_OK;
  }
  if (!TryClaimSlot(pool_name)) {
    ESP_LOGD(kTag, "%s: fetch already in flight", meta->key);
    return ESP_ERR_INVALID_STATE;
  }
  auto ctx = std::make_unique<FetchTaskCtx>();
  ctx->pool_name = pool_name;
  // Dedicated short-lived task. 6 KiB stack covers esp_http_client +
  // mbedtls handshake + a small std::string copy. Lower priority
  // than the renderer so a paint isn't preempted mid-frame.
  TaskHandle_t handle = nullptr;
  const BaseType_t ok = xTaskCreate(&FetchTaskTrampoline, "pool_logo_fetch",
                                    6 * 1024, ctx.get(),
                                    tskIDLE_PRIORITY + 1, &handle);
  if (ok != pdPASS) {
    ReleaseSlot(pool_name);
    return ESP_FAIL;
  }
  // Ownership transferred to the task on success.
  (void)ctx.release();
  return ESP_OK;
}

esp_err_t FetchNow(const std::string& pool_name) {
  if (pool_name.empty()) return ESP_ERR_INVALID_ARG;
  const PoolLogoMeta* meta = LookupMeta(pool_name);
  if (!meta) return ESP_ERR_NOT_FOUND;
  if (!TryClaimSlot(pool_name)) return ESP_ERR_INVALID_STATE;
  const esp_err_t rc = DoFetch(*meta);
  ReleaseSlot(pool_name);
  return rc;
}

int ClearAllCached() {
  DIR* d = ::opendir(kCacheDir);
  if (!d) {
    if (errno == ENOENT) return 0;
    ESP_LOGW(kTag, "opendir failed: errno=%d", errno);
    return -1;
  }
  int removed = 0;
  while (struct dirent* ent = ::readdir(d)) {
    if (ent->d_name[0] == '.') continue;
    std::string path = CachePath(ent->d_name);
    if (::unlink(path.c_str()) == 0) {
      ++removed;
    } else {
      ESP_LOGW(kTag, "unlink('%s') failed: errno=%d", path.c_str(), errno);
    }
  }
  ::closedir(d);
  EvictCacheSlot();
  ESP_LOGI(kTag, "cleared %d cached logo file(s)", removed);
  return removed;
}

}  // namespace pool_logos
}  // namespace btclock
