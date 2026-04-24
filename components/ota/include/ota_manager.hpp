// OTA firmware-update subsystem.
//
// Two entry points drive the same underlying partition write path:
//   * Pull-OTA (TriggerAutoUpdate): the device fetches a release
//     manifest + firmware asset + matching .sha256 over HTTPS, streams
//     the bytes through esp_https_ota_perform(), and on success reboots
//     into the new slot.
//   * Push-OTA (WritePushImage): an HTTP client POSTs the raw image
//     bytes into /upload/firmware; the caller pumps bytes via a recv()
//     callback, we feed them into esp_ota_write() plus a parallel
//     SHA-256 accumulator, and (if a hex digest was supplied) verify
//     before activating.
//
// Only one update runs at a time — IsUpdating() gates /api/status so
// the WebUI can surface the in-progress state, and a second
// TriggerAutoUpdate() call while updating returns
// ESP_ERR_INVALID_STATE.
//
// This component deliberately does NOT own the HTTP handlers; those
// live in components/webserver/control_server.cpp (mirroring the rest
// of the /api surface). Handlers call into this singleton.
//
// Auth: the /api OTA endpoints are gated by RequireHttpAuth() +
// RequireOtaEnabled() at the control_server layer; the manager itself
// assumes its caller has already enforced policy.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_err.h"

namespace btclock {

class OtaManager {
 public:
  struct Config {
    // GitHub-style releases JSON (with `assets[]` containing
    // `browser_download_url`). Empty disables pull-OTA.
    std::string release_url;
    // Firmware asset filename to match inside the releases JSON, e.g.
    // "btclock_rev_b_ota.bin". A sibling asset named
    // `<firmware_asset>.sha256` is also looked up for verification.
    std::string firmware_asset;
    // Optional WebUI asset filename (not flashed by pull-OTA yet; the
    // Arduino firmware shipped the WebUI alongside firmware but the
    // IDF port handles WebUI via /upload/webui). Stored for forward
    // compatibility with a future pull-webui flow.
    std::string webui_asset;
  };

  // Signature matches HttpdRecvTrampoline in control_server.cpp so the
  // HTTP handler can pass it through unchanged.
  using RecvFn = int (*)(void* ctx, char* buf, size_t want);

  esp_err_t Init(const Config& cfg);

  // Observed by /api/status and by TriggerAutoUpdate's re-entry guard.
  // Safe to read from any task.
  bool IsUpdating() const { return is_updating_.load(); }

  // Spawns a one-shot background task that drives the full pull-OTA
  // flow. Returns immediately. If another update is in progress
  // returns ESP_ERR_INVALID_STATE. Config must have a non-empty
  // release_url and firmware_asset or this returns ESP_ERR_INVALID_ARG.
  esp_err_t TriggerAutoUpdate();

  // Push-OTA body handler. Streams `expected_bytes` from `recv(ctx, …)`
  // into the next OTA partition, computing SHA-256 alongside. If
  // `expected_sha256_hex` is non-null it must be a 64-char lowercase
  // hex string; on mismatch the OTA is aborted and the partition is
  // NOT activated. On success the next boot partition is set; the
  // caller is responsible for rebooting (mirrors HandleUploadWebui).
  esp_err_t WritePushImage(RecvFn recv,
                           void* ctx,
                           size_t expected_bytes,
                           const char* expected_sha256_hex,
                           size_t* out_written);

 private:
  static void AutoUpdateTaskTrampoline(void* arg);
  void RunAutoUpdate();

  Config cfg_;
  std::atomic<bool> is_updating_{false};
};

// Process-wide singleton. Safe to call before Init — methods that
// depend on Config validate internally.
OtaManager& GetOtaManager();

}  // namespace btclock
