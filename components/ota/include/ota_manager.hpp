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
#include <functional>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "ota_progress.hpp"

namespace btclock {

class OtaManager {
 public:
  // Progress callback signature — invoked from WritePushImage on
  // milestone boundaries (start, every ~16 KiB written, on verify /
  // reboot / failure). May be called from any task; the callback must
  // be cheap (a few LED posts + a status mirror). Use SetProgressCallback
  // to install. Pass `{}` to clear.
  using ProgressCallback = std::function<void(const OtaProgress&)>;

  // Pre-flash hook — fires once from WritePushImage BEFORE the first
  // esp_ota_begin call. Used to stop data sources / nostr relay / pollers
  // so the write path has more internal heap to work with, and to paint
  // the "UPDATE!" overlay on every EPD panel. Not called for pull-OTA
  // today (the auto-update flow already pauses naturally between HTTPS
  // reads).
  using PreFlashHook = std::function<void()>;

  struct Config {
    // Resolves to a GitHub-style releases JSON URL (with `assets[]`
    // containing `browser_download_url`). Invoked once per
    // TriggerAutoUpdate call and once at the top of RunAutoUpdate so a
    // settings PATCH that rewrites `gitReleaseUrl` takes effect on the
    // next attempt without a reboot. A null pointer or a callback that
    // returns an empty string disables pull-OTA. Plain function pointer
    // (not std::function) to avoid the type-erasure code-size hit;
    // captureless lambdas decay to it automatically.
    using ReleaseUrlFn = std::string (*)();
    ReleaseUrlFn release_url = nullptr;
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
  // returns ESP_ERR_INVALID_STATE. Returns ESP_ERR_INVALID_ARG when
  // firmware_asset is empty or release_url() resolves to an empty
  // string.
  esp_err_t TriggerAutoUpdate();

  // Push-OTA body handler. Streams `expected_bytes` from `recv(ctx, …)`
  // into the next OTA partition, computing SHA-256 alongside. If
  // `expected_sha256_hex` is non-null it must be a 64-char lowercase
  // hex string; on mismatch the OTA is aborted and the partition is
  // NOT activated. On success the next boot partition is set; the
  // caller is responsible for rebooting (mirrors HandleUploadWebui).
  esp_err_t WritePushImage(RecvFn recv, void* ctx, size_t expected_bytes,
                           const char* expected_sha256_hex,
                           size_t* out_written);

  // Install / clear the progress callback. Thread-safe. The callback is
  // invoked on the WritePushImage caller's thread (the httpd worker),
  // so callers that want to paint the EPD panels should either do so
  // directly here (the main render loop short-circuits while the OTA
  // is active) or hop the work to a task that owns the panels.
  void SetProgressCallback(ProgressCallback cb);

  // Install / clear the pre-flash hook. See the PreFlashHook doc above.
  void SetPreFlashHook(PreFlashHook hook);

 private:
  static void AutoUpdateTaskTrampoline(void* arg);
  void RunAutoUpdate();

  // Internal: fire `progress_cb_` under the mutex. Copies the callback
  // out before invocation so a concurrent SetProgressCallback doesn't
  // mutate the functor while it's on the stack.
  void EmitProgress(const OtaProgress& p);

  Config cfg_;
  std::atomic<bool> is_updating_{false};
  std::mutex cb_mu_;
  ProgressCallback progress_cb_;
  PreFlashHook pre_flash_hook_;
};

// Process-wide singleton. Safe to call before Init — methods that
// depend on Config validate internally.
OtaManager& GetOtaManager();

}  // namespace btclock
