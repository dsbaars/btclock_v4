#include "prefs.hpp"

#include <cstring>

#include "esp_log.h"
#include "nvs_flash.h"

namespace btclock {
namespace {
constexpr const char* kTag = "prefs";
bool g_nvs_inited = false;
}  // namespace

esp_err_t Prefs::InitOnce() {
  if (g_nvs_inited) return ESP_OK;
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kTag, "NVS truncated (%s); erasing", esp_err_to_name(err));
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err == ESP_OK) g_nvs_inited = true;
  return err;
}

Prefs::Prefs(const char* ns_name) {
  const esp_err_t err = nvs_open(ns_name, NVS_READWRITE, &handle_);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "nvs_open ns='%s' err=%s", ns_name, esp_err_to_name(err));
    return;
  }
  open_ = true;
}

Prefs::~Prefs() {
  if (open_) nvs_close(handle_);
}

std::string Prefs::GetString(const char* key, const char* default_value) const {
  if (!open_) return default_value ? default_value : "";
  size_t len = 0;
  esp_err_t err = nvs_get_str(handle_, key, nullptr, &len);
  if (err != ESP_OK || len == 0) return default_value ? default_value : "";
  std::string out;
  out.resize(len);
  err = nvs_get_str(handle_, key, out.data(), &len);
  if (err != ESP_OK) return default_value ? default_value : "";
  if (!out.empty() && out.back() == '\0') out.pop_back();  // strip trailing NUL
  return out;
}

esp_err_t Prefs::SetString(const char* key, const char* value) {
  if (!open_) return ESP_ERR_INVALID_STATE;
  return nvs_set_str(handle_, key, value);
}

uint32_t Prefs::GetU32(const char* key, uint32_t default_value) const {
  if (!open_) return default_value;
  uint32_t v = default_value;
  nvs_get_u32(handle_, key, &v);
  return v;
}

esp_err_t Prefs::SetU32(const char* key, uint32_t value) {
  if (!open_) return ESP_ERR_INVALID_STATE;
  return nvs_set_u32(handle_, key, value);
}

bool Prefs::GetBool(const char* key, bool default_value) const {
  return GetU32(key, default_value ? 1 : 0) != 0;
}

esp_err_t Prefs::SetBool(const char* key, bool value) {
  return SetU32(key, value ? 1 : 0);
}

esp_err_t Prefs::Remove(const char* key) {
  if (!open_) return ESP_ERR_INVALID_STATE;
  return nvs_erase_key(handle_, key);
}

esp_err_t Prefs::Commit() {
  if (!open_) return ESP_ERR_INVALID_STATE;
  return nvs_commit(handle_);
}

}  // namespace btclock
