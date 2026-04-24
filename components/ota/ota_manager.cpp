#include "ota_manager.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "tls_gate/tls_gate.hpp"

namespace btclock {
namespace {

constexpr const char* kTag = "ota";

// Release-JSON cap. The Arduino firmware parsed an ArduinoJson doc
// from the full body without a cap; the IDF port is stricter because
// esp_http_client's streaming reads happen on the calling task's
// stack and must fit in a bounded buffer. GitHub's /releases/latest
// JSON for this project is ~8 KB today.
constexpr size_t kReleaseJsonCap = 32 * 1024;

// Per-request context threaded through the esp_http_client event
// handler when we're just accumulating a small response body into a
// std::string (release JSON + .sha256 file).
struct FetchCtx {
  std::string body;
  size_t cap = 0;
  bool truncated = false;
};

esp_err_t FetchEventHandler(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* ctx = static_cast<FetchCtx*>(evt->user_data);
  if (!ctx) return ESP_OK;
  if (ctx->truncated) return ESP_OK;  // discard silently after cap
  if (ctx->body.size() + evt->data_len > ctx->cap) {
    ctx->truncated = true;
    return ESP_OK;
  }
  ctx->body.append(static_cast<const char*>(evt->data), evt->data_len);
  return ESP_OK;
}

// Fetch `url` into a caller-owned std::string. Returns ESP_OK on a
// 200 response that fit within `cap`; any transport or cap failure
// returns ESP_FAIL with an empty body.
esp_err_t HttpGetString(const std::string& url, size_t cap,
                        std::string* out) {
  out->clear();
  if (url.empty()) return ESP_ERR_INVALID_ARG;

  FetchCtx ctx;
  ctx.cap = cap;

  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.event_handler = &FetchEventHandler;
  cfg.user_data = &ctx;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 15000;
  // GitHub's release JSON is served via asset redirects; follow them.
  cfg.disable_auto_redirect = false;
  cfg.max_redirection_count = 5;

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (!client) return ESP_FAIL;

  esp_err_t err;
  int status = 0;
  {
    // Hold the TLS gate only around perform — matches the pattern in
    // mining_pool_common so two concurrent OTAs / pool polls don't
    // both allocate TLS IN buffers at once.
    std::lock_guard<std::mutex> lk(btclock::tls_gate::mutex());
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "http GET %s failed: %s", url.c_str(),
             esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return err;
  }
  esp_http_client_cleanup(client);

  if (status < 200 || status >= 300) {
    ESP_LOGW(kTag, "http GET %s status=%d", url.c_str(), status);
    return ESP_FAIL;
  }
  if (ctx.truncated) {
    ESP_LOGW(kTag, "http GET %s exceeded %u bytes", url.c_str(),
             static_cast<unsigned>(cap));
    return ESP_FAIL;
  }

  *out = std::move(ctx.body);
  return ESP_OK;
}

// Parse first 64 hex chars out of a sha256 response. The convention
// across sha256sum / GitHub release checksums is `<hex>  <filename>\n`
// or just `<hex>\n`; we only need the digest.
bool ExtractSha256Hex(const std::string& raw, std::string* out_hex) {
  out_hex->clear();
  out_hex->reserve(64);
  for (char c : raw) {
    if (out_hex->size() == 64) break;
    const char lc =
        (c >= 'A' && c <= 'F') ? static_cast<char>(c + ('a' - 'A')) : c;
    const bool hex = (lc >= '0' && lc <= '9') || (lc >= 'a' && lc <= 'f');
    if (!hex) {
      if (!out_hex->empty()) break;  // stop at first non-hex past digest
      continue;                       // tolerate leading whitespace
    }
    out_hex->push_back(lc);
  }
  return out_hex->size() == 64;
}

// Walk the GitHub release JSON `assets[]` array, pulling out the
// `browser_download_url` for `firmware_asset` and `<firmware_asset>.sha256`.
// Either URL may be empty on return.
struct ReleaseInfo {
  std::string file_url;
  std::string checksum_url;
};

ReleaseInfo ParseReleaseJson(const std::string& json,
                             const std::string& firmware_asset) {
  ReleaseInfo info;
  cJSON* root = cJSON_Parse(json.c_str());
  if (!root) return info;

  cJSON* assets = cJSON_GetObjectItem(root, "assets");
  if (!cJSON_IsArray(assets)) {
    cJSON_Delete(root);
    return info;
  }

  const std::string sha_name = firmware_asset + ".sha256";
  cJSON* asset = nullptr;
  cJSON_ArrayForEach(asset, assets) {
    cJSON* name = cJSON_GetObjectItem(asset, "name");
    cJSON* url = cJSON_GetObjectItem(asset, "browser_download_url");
    if (!cJSON_IsString(name) || !cJSON_IsString(url)) continue;
    const std::string n = name->valuestring;
    if (n == firmware_asset) {
      info.file_url = url->valuestring;
    } else if (n == sha_name) {
      info.checksum_url = url->valuestring;
    }
    if (!info.file_url.empty() && !info.checksum_url.empty()) break;
  }

  cJSON_Delete(root);
  return info;
}

// Hash the freshly-written OTA partition so we can compare against
// the downloaded .sha256. `esp_https_ota` doesn't currently expose a
// running digest in v5.5, so we read back the bytes we just wrote
// (flash is fast; reading 1.5 MB takes ~200 ms) and hash them here.
bool HashOtaPartition(const esp_partition_t* part, size_t image_bytes,
                      std::string* out_hex) {
  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  const mbedtls_md_info_t* info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_setup(&md, info, 0) != 0) {
    mbedtls_md_free(&md);
    return false;
  }
  mbedtls_md_starts(&md);

  constexpr size_t kChunk = 4096;
  std::vector<uint8_t> buf(kChunk);
  size_t offset = 0;
  while (offset < image_bytes) {
    const size_t want = std::min(kChunk, image_bytes - offset);
    const esp_err_t rc =
        esp_partition_read(part, offset, buf.data(), want);
    if (rc != ESP_OK) {
      ESP_LOGE(kTag, "partition_read@%u: %s",
               static_cast<unsigned>(offset), esp_err_to_name(rc));
      mbedtls_md_free(&md);
      return false;
    }
    mbedtls_md_update(&md, buf.data(), want);
    offset += want;
  }

  uint8_t digest[32];
  mbedtls_md_finish(&md, digest);
  mbedtls_md_free(&md);

  out_hex->resize(64);
  for (int i = 0; i < 32; ++i) {
    std::snprintf(&(*out_hex)[i * 2], 3, "%02x", digest[i]);
  }
  return true;
}

}  // namespace

OtaManager& GetOtaManager() {
  static OtaManager s_instance;
  return s_instance;
}

esp_err_t OtaManager::Init(const Config& cfg) {
  cfg_ = cfg;
  return ESP_OK;
}

esp_err_t OtaManager::TriggerAutoUpdate() {
  if (cfg_.release_url.empty() || cfg_.firmware_asset.empty()) {
    ESP_LOGW(kTag, "auto-update not configured (release_url=%zu asset=%zu)",
             cfg_.release_url.size(), cfg_.firmware_asset.size());
    return ESP_ERR_INVALID_ARG;
  }
  bool expected = false;
  if (!is_updating_.compare_exchange_strong(expected, true)) {
    return ESP_ERR_INVALID_STATE;
  }
  // 8 KB stack is comfortable for cJSON + esp_https_ota_perform.
  const BaseType_t rc = xTaskCreate(&AutoUpdateTaskTrampoline, "ota_auto",
                                    8192, this, tskIDLE_PRIORITY + 3,
                                    nullptr);
  if (rc != pdPASS) {
    is_updating_.store(false);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void OtaManager::AutoUpdateTaskTrampoline(void* arg) {
  static_cast<OtaManager*>(arg)->RunAutoUpdate();
  vTaskDelete(nullptr);
}

void OtaManager::RunAutoUpdate() {
  ESP_LOGW(kTag, "auto-update starting: url=%s asset=%s",
           cfg_.release_url.c_str(), cfg_.firmware_asset.c_str());

  // Reset the is_updating flag on every exit path.
  struct Guard {
    OtaManager* self;
    ~Guard() { self->is_updating_.store(false); }
  } guard{this};

  std::string release_body;
  if (HttpGetString(cfg_.release_url, kReleaseJsonCap, &release_body) !=
      ESP_OK) {
    ESP_LOGE(kTag, "release manifest fetch failed");
    return;
  }

  ReleaseInfo info = ParseReleaseJson(release_body, cfg_.firmware_asset);
  if (info.file_url.empty() || info.checksum_url.empty()) {
    ESP_LOGE(kTag, "asset %s not found in release JSON",
             cfg_.firmware_asset.c_str());
    return;
  }

  std::string sha_body;
  if (HttpGetString(info.checksum_url, 1024, &sha_body) != ESP_OK) {
    ESP_LOGE(kTag, "checksum fetch failed: %s", info.checksum_url.c_str());
    return;
  }
  std::string expected_hex;
  if (!ExtractSha256Hex(sha_body, &expected_hex)) {
    ESP_LOGE(kTag, "checksum parse failed");
    return;
  }

  esp_http_client_config_t http_cfg = {};
  http_cfg.url = info.file_url.c_str();
  http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  http_cfg.timeout_ms = 15000;
  http_cfg.keep_alive_enable = true;

  esp_https_ota_config_t ota_cfg = {};
  ota_cfg.http_config = &http_cfg;

  esp_https_ota_handle_t handle = nullptr;
  esp_err_t rc = esp_https_ota_begin(&ota_cfg, &handle);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "esp_https_ota_begin: %s", esp_err_to_name(rc));
    return;
  }

  // Drive the state machine manually so the task can yield between
  // chunks; esp_https_ota_perform returns ESP_ERR_HTTPS_OTA_IN_PROGRESS
  // while there's more body to read.
  while ((rc = esp_https_ota_perform(handle)) ==
         ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "esp_https_ota_perform: %s", esp_err_to_name(rc));
    esp_https_ota_abort(handle);
    return;
  }

  if (!esp_https_ota_is_complete_data_received(handle)) {
    ESP_LOGE(kTag, "incomplete image body");
    esp_https_ota_abort(handle);
    return;
  }

  const int image_len = esp_https_ota_get_image_len_read(handle);
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);

  // Read back and hash. Comparing against the declared expected_hex
  // here means esp_https_ota_finish (which flips the boot partition)
  // only runs on a match.
  std::string actual_hex;
  bool hash_ok = target && image_len > 0 &&
                 HashOtaPartition(target, static_cast<size_t>(image_len),
                                  &actual_hex);
  if (!hash_ok) {
    ESP_LOGE(kTag, "partition rehash failed");
    esp_https_ota_abort(handle);
    return;
  }
  if (actual_hex != expected_hex) {
    ESP_LOGE(kTag, "sha256 mismatch: expected=%s actual=%s",
             expected_hex.c_str(), actual_hex.c_str());
    esp_https_ota_abort(handle);
    return;
  }

  rc = esp_https_ota_finish(handle);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "esp_https_ota_finish: %s", esp_err_to_name(rc));
    return;
  }

  ESP_LOGW(kTag, "auto-update ok: bytes=%d; rebooting in 1s", image_len);
  // Short pause so the triggering /api/firmware/auto_update response
  // has a chance to be consumed by the client if the background task
  // is fast. Handler also returns HTTP 200 immediately so this is
  // belt-and-braces.
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
}

esp_err_t OtaManager::WritePushImage(RecvFn recv, void* ctx,
                                     size_t expected_bytes,
                                     const char* expected_sha256_hex,
                                     size_t* out_written) {
  if (out_written) *out_written = 0;
  if (!recv) return ESP_ERR_INVALID_ARG;

  bool expected = false;
  if (!is_updating_.compare_exchange_strong(expected, true)) {
    return ESP_ERR_INVALID_STATE;
  }
  struct Guard {
    OtaManager* self;
    ~Guard() { self->is_updating_.store(false); }
  } guard{this};

  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part) return ESP_ERR_NOT_FOUND;
  if (expected_bytes > part->size) return ESP_ERR_INVALID_SIZE;

  esp_ota_handle_t handle = 0;
  const size_t begin_size =
      expected_bytes > 0 ? expected_bytes : OTA_WITH_SEQUENTIAL_WRITES;
  esp_err_t rc = esp_ota_begin(part, begin_size, &handle);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_begin: %s", esp_err_to_name(rc));
    return rc;
  }

  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  const mbedtls_md_info_t* info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info || mbedtls_md_setup(&md, info, 0) != 0) {
    mbedtls_md_free(&md);
    esp_ota_abort(handle);
    return ESP_ERR_NO_MEM;
  }
  mbedtls_md_starts(&md);

  constexpr size_t kChunk = 2048;
  std::vector<char> buf(kChunk);
  size_t written = 0;
  const size_t cap = expected_bytes > 0 ? expected_bytes : part->size;

  while (written < cap) {
    const size_t want = std::min(kChunk, cap - written);
    const int n = recv(ctx, buf.data(), want);
    if (n <= 0) {
      ESP_LOGE(kTag, "recv error at %u: %d",
               static_cast<unsigned>(written), n);
      mbedtls_md_free(&md);
      esp_ota_abort(handle);
      return ESP_FAIL;
    }
    mbedtls_md_update(&md, reinterpret_cast<uint8_t*>(buf.data()),
                      static_cast<size_t>(n));
    rc = esp_ota_write(handle, buf.data(), static_cast<size_t>(n));
    if (rc != ESP_OK) {
      ESP_LOGE(kTag, "esp_ota_write@%u: %s",
               static_cast<unsigned>(written), esp_err_to_name(rc));
      mbedtls_md_free(&md);
      esp_ota_abort(handle);
      return rc;
    }
    written += static_cast<size_t>(n);
  }

  uint8_t digest[32];
  mbedtls_md_finish(&md, digest);
  mbedtls_md_free(&md);

  if (expected_sha256_hex && *expected_sha256_hex) {
    char actual[65];
    for (int i = 0; i < 32; ++i) {
      std::snprintf(actual + i * 2, 3, "%02x", digest[i]);
    }
    actual[64] = 0;
    std::string expected_lc(expected_sha256_hex);
    for (char& c : expected_lc) {
      if (c >= 'A' && c <= 'F') c = static_cast<char>(c + ('a' - 'A'));
    }
    if (expected_lc.size() != 64 || expected_lc != actual) {
      ESP_LOGE(kTag, "push sha256 mismatch: expected=%s actual=%s",
               expected_sha256_hex, actual);
      esp_ota_abort(handle);
      return ESP_ERR_INVALID_CRC;
    }
  }

  rc = esp_ota_end(handle);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_end: %s", esp_err_to_name(rc));
    return rc;
  }
  rc = esp_ota_set_boot_partition(part);
  if (rc != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_set_boot_partition: %s", esp_err_to_name(rc));
    return rc;
  }

  if (out_written) *out_written = written;
  ESP_LOGW(kTag, "push OTA ok: bytes=%u", static_cast<unsigned>(written));
  return ESP_OK;
}

}  // namespace btclock
