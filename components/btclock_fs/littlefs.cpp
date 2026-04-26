#include "littlefs.hpp"

#include <cstring>
#include <string>

#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "sdkconfig.h"

namespace btclock {
namespace {

constexpr const char* kTag = "lfs";

// Tracks the currently-mounted base path so re-mount attempts with
// matching paths are idempotent and mismatching paths are rejected.
std::string g_mounted_base_path;

}  // namespace

esp_err_t MountLittleFs(const char* base_path) {
  const char* path =
      (base_path && *base_path) ? base_path : kLittleFsDefaultBasePath;

  if (!g_mounted_base_path.empty()) {
    if (g_mounted_base_path == path) return ESP_OK;
    ESP_LOGE(kTag, "already mounted at %s; refuse to mount at %s",
             g_mounted_base_path.c_str(), path);
    return ESP_ERR_INVALID_STATE;
  }

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = path;
  conf.partition_label = kLittleFsPartitionLabel;
  // `format_if_mount_failed = true` is also the recovery path for a
  // botched /upload/webui: if FlashWebuiImage wrote a partial or
  // corrupt image before the reboot, this flag auto-formats the
  // partition on the next boot so the device comes back up blank
  // rather than wedged. See FlashWebuiImage() in this file.
  conf.format_if_mount_failed = true;
  conf.dont_mount = false;
  conf.read_only = false;
  conf.grow_on_mount = false;

  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "mount label='%s' base='%s' failed: %s",
             kLittleFsPartitionLabel, path, esp_err_to_name(err));
    return err;
  }

  size_t total = 0, used = 0;
  const esp_err_t info_err =
      esp_littlefs_info(kLittleFsPartitionLabel, &total, &used);
  if (info_err == ESP_OK) {
    ESP_LOGI(kTag, "mounted %s at %s: used=%uB total=%uB",
             kLittleFsPartitionLabel, path, static_cast<unsigned>(used),
             static_cast<unsigned>(total));
  } else {
    ESP_LOGW(kTag, "mounted %s at %s; info unavailable: %s",
             kLittleFsPartitionLabel, path, esp_err_to_name(info_err));
  }

  g_mounted_base_path = path;
  return ESP_OK;
}

esp_err_t UnmountLittleFs() {
  if (g_mounted_base_path.empty()) return ESP_OK;
  const esp_err_t err = esp_vfs_littlefs_unregister(kLittleFsPartitionLabel);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "unmount failed: %s", esp_err_to_name(err));
    return err;
  }
  g_mounted_base_path.clear();
  return ESP_OK;
}

esp_err_t GetLittleFsUsage(size_t* used_bytes, size_t* total_bytes) {
  if (g_mounted_base_path.empty()) return ESP_ERR_INVALID_STATE;
  size_t total = 0, used = 0;
  const esp_err_t err =
      esp_littlefs_info(kLittleFsPartitionLabel, &total, &used);
  if (err != ESP_OK) return err;
  if (used_bytes) *used_bytes = used;
  if (total_bytes) *total_bytes = total;
  return ESP_OK;
}

size_t GetLittleFsPartitionSize() {
  const esp_partition_t* p = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
      kLittleFsPartitionLabel);
  return p ? p->size : 0;
}

esp_err_t FlashWebuiImage(WebuiRecvFn recv, void* recv_ctx, size_t expected_len,
                          size_t* bytes_written) {
  if (bytes_written) *bytes_written = 0;
  if (!recv) return ESP_ERR_INVALID_ARG;

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS,
      kLittleFsPartitionLabel);
  if (!part) {
    ESP_LOGE(kTag, "FlashWebuiImage: storage partition not found");
    return ESP_ERR_NOT_FOUND;
  }

  if (expected_len > part->size) {
    ESP_LOGE(kTag, "FlashWebuiImage: expected %u > partition %u",
             static_cast<unsigned>(expected_len),
             static_cast<unsigned>(part->size));
    return ESP_ERR_INVALID_SIZE;
  }

  // Hand exclusive control of the flash region to this routine. If the
  // unmount fails we abort without touching the partition — a failed
  // unmount means another task still has file handles open and an
  // erase here would corrupt them.
  const esp_err_t umount_err = UnmountLittleFs();
  if (umount_err != ESP_OK) {
    ESP_LOGE(kTag, "FlashWebuiImage: unmount failed: %s",
             esp_err_to_name(umount_err));
    return umount_err;
  }

  // Erase the whole partition up-front. `esp_partition_erase_range`
  // requires both offset and size to be 4 KiB-aligned; all three
  // partition tables (partitions_*mb.csv) size storage on 4 KiB
  // boundaries so this is unconditional.
  const esp_err_t erase_err = esp_partition_erase_range(part, 0, part->size);
  if (erase_err != ESP_OK) {
    ESP_LOGE(kTag, "FlashWebuiImage: erase failed: %s",
             esp_err_to_name(erase_err));
    return erase_err;
  }

  // Streaming write. 4 KiB scratch matches the flash sector size so we
  // don't straddle sectors; the old Arduino Update library used the
  // same granularity. `cap` bounds the loop when the client omits
  // Content-Length (expected_len==0): never write past the partition.
  constexpr size_t kChunk = 4096;
  char buf[kChunk];
  size_t offset = 0;
  const size_t cap = expected_len > 0 ? expected_len : part->size;

  while (offset < cap) {
    const size_t want = (cap - offset) < kChunk ? (cap - offset) : kChunk;
    const int n = recv(recv_ctx, buf, want);
    if (n < 0) {
      ESP_LOGE(kTag, "FlashWebuiImage: recv error at offset %u",
               static_cast<unsigned>(offset));
      return ESP_FAIL;
    }
    if (n == 0)
      break;  // Clean EOF — only acceptable when content-length was unknown.

    const esp_err_t wr =
        esp_partition_write(part, offset, buf, static_cast<size_t>(n));
    if (wr != ESP_OK) {
      ESP_LOGE(kTag, "FlashWebuiImage: write failed at %u: %s",
               static_cast<unsigned>(offset), esp_err_to_name(wr));
      return wr;
    }
    offset += static_cast<size_t>(n);
  }

  if (bytes_written) *bytes_written = offset;

  // Content-Length enforcement: if the client promised N bytes we must
  // have received exactly N. Otherwise the partition holds a truncated
  // image. We still return success-with-short-count here and let the
  // caller decide the HTTP response — this keeps the helper usable for
  // length-unknown streams too.
  if (expected_len > 0 && offset != expected_len) {
    ESP_LOGE(kTag, "FlashWebuiImage: truncated: got %u want %u",
             static_cast<unsigned>(offset),
             static_cast<unsigned>(expected_len));
    return ESP_ERR_INVALID_SIZE;
  }

  // Intentionally do NOT remount here. The expected caller flow is
  // to respond to the HTTP client and reboot — a half-written image
  // mounted now would either trigger the auto-format (reverting the
  // upload) or present a broken filesystem to readers during the
  // short window before reboot. Either way, letting the boot path
  // remount on the fresh image is simpler and matches the old
  // firmware's behaviour (Update.end(true) → scheduleDelayedRestart).
  return ESP_OK;
}

}  // namespace btclock
