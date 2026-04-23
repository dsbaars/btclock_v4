#pragma once

// Thin C++ wrapper around joltwallet/esp_littlefs. Keeps the API surface
// small — three functions — so the rest of the PoC doesn't have to know
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
// TODO(btclock_v3_fci-bq0): `GetLittleFsUsage` is the hook the
// `/api/system_status` endpoint should use to fill in `fsUsedBytes` and
// `fsTotalBytes`. That endpoint lives in `components/webserver/
// control_server.cpp` on `worktree-agent-aa1d0cd8` — not on this
// branch. Once the control-server work merges, replace the zero
// placeholders there with a call to this function.

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

}  // namespace btclock
