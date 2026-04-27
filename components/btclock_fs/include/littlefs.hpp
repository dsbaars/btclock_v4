#pragma once

// Thin C++ wrapper around joltwallet/esp_littlefs. Keeps the API surface
// small — three functions — so the rest of the firmware doesn't have to know
// which LittleFS binding we're using. Semantics:
//
//   * `MountLittleFs` is idempotent: calling twice with the same base
//     path is a no-op (returns ESP_OK). Calling with a different base
//     path while already mounted returns ESP_ERR_INVALID_STATE.
//   * Uses `format_if_mount_failed = true`, so a blank partition
//     (first boot, or after OTA-erase) auto-formats. If the auto-format
//     itself fails the function returns the underlying error and the
//     caller is expected to continue booting without a filesystem —
//     the old Arduino firmware does the same (see `config.cpp` line
//     518: `if (!LittleFS.begin(true)) { /* swallow */ }`).
//   * Partition is located by label ("storage"); see
//     `partitions_*.csv` at repo root.
//
// `GetLittleFsUsage` is the hook the `/api/system_status` endpoint
// uses to fill in `fsUsedBytes` and `fsTotalBytes` (see
// components/webserver/control_server.cpp).

#include <cstddef>

#include "esp_err.h"

namespace btclock {

// Default partition label. Must match the entry name in the three
// partitions_*mb.csv tables. Kept as a constexpr so the control-server
// can avoid a magic string when it wires up the FS usage fields.
inline constexpr const char* kLittleFsPartitionLabel = "storage";
inline constexpr const char* kLittleFsDefaultBasePath = "/lfs";

// Mounts the LittleFS partition at `base_path`. First-boot-fresh case
// is handled: the underlying library formats and re-mounts.
esp_err_t MountLittleFs(const char* base_path = kLittleFsDefaultBasePath);

// Unmount the previously-mounted partition. Safe to call when nothing
// is mounted (returns ESP_OK).
esp_err_t UnmountLittleFs();

// Populate `used_bytes` and `total_bytes` with the current usage.
// Either pointer may be null if the caller only wants one of the two.
// Returns an error if the filesystem isn't mounted.
esp_err_t GetLittleFsUsage(size_t* used_bytes, size_t* total_bytes);

// Return the total byte-size of the LittleFS partition as declared in
// the partition table. Does not require the FS to be mounted — it reads
// partition metadata via `esp_partition_find_first`. Returns 0 if the
// partition can't be located (board misconfiguration).
size_t GetLittleFsPartitionSize();

// The pure-logic bounds check for /upload/webui lives in
// `webui_upload_bounds.hpp` so the host-test suite can include it
// without pulling in esp_err.h.

// OTA-mode helper: stream a new LittleFS image into the `storage`
// partition.
//
// The filesystem MUST be mounted before calling (this function calls
// `UnmountLittleFs` internally so it has exclusive access to the
// partition during the erase/write cycle). On success the partition is
// NOT remounted — the caller is expected to reboot; `MountLittleFs` on
// the next boot will either succeed against the fresh image or, if the
// image is corrupt, auto-format under the library's
// `format_if_mount_failed=true` flag.
//
// `recv` is invoked in a loop to fill a 4 KiB scratch buffer. It must
// return the number of bytes read (0 on clean EOF, negative on error).
// The partition write offset advances by the returned count each call.
//
// `expected_len` is the total number of bytes to stream. If the caller
// doesn't know the length (no Content-Length header), pass 0 and the
// loop will stream until `recv` returns 0, capped at partition size.
//
// `bytes_written` is populated with the final byte-count on return.
//
// Returns ESP_OK on success, ESP_ERR_NOT_FOUND if the `storage`
// partition is missing, ESP_ERR_INVALID_SIZE if `expected_len` exceeds
// the partition, ESP_ERR_INVALID_ARG on null `recv`, or a forwarded
// esp_partition_* error on flash failure. After any failure mode the
// partition contents are undefined; callers should reboot so the next
// mount triggers a fresh format.
using WebuiRecvFn = int (*)(void* ctx, char* buf, size_t buf_len);
esp_err_t FlashWebuiImage(WebuiRecvFn recv, void* recv_ctx, size_t expected_len,
                          size_t* bytes_written);

}  // namespace btclock
