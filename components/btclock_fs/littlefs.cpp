#include "littlefs.hpp"

#include <cstring>
#include <string>

#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace btclock {
namespace {

constexpr const char* kTag = "lfs";

// Tracks the currently-mounted base path so re-mount attempts with
// matching paths are idempotent and mismatching paths are rejected.
std::string g_mounted_base_path;

}  // namespace

esp_err_t MountLittleFs(const char* base_path) {
  const char* path = (base_path && *base_path) ? base_path
                                                : kLittleFsDefaultBasePath;

  if (!g_mounted_base_path.empty()) {
    if (g_mounted_base_path == path) return ESP_OK;
    ESP_LOGE(kTag, "already mounted at %s; refuse to mount at %s",
             g_mounted_base_path.c_str(), path);
    return ESP_ERR_INVALID_STATE;
  }

  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = path;
  conf.partition_label = kLittleFsPartitionLabel;
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
             kLittleFsPartitionLabel, path,
             static_cast<unsigned>(used),
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

}  // namespace btclock
