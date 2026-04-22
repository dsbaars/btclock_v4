#include "data_core/hub.hpp"

#include <utility>

#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "data-hub";
}  // namespace

bool DataSnapshot::Merge(const DataSnapshot& other) {
  bool changed = false;
  if (other.block_height &&
      (!block_height || *block_height != *other.block_height)) {
    block_height = other.block_height;
    changed = true;
  }
  if (other.block_fee &&
      (!block_fee || *block_fee != *other.block_fee)) {
    block_fee = other.block_fee;
    changed = true;
  }
  if (other.block_fee_precise &&
      (!block_fee_precise ||
       *block_fee_precise != *other.block_fee_precise)) {
    block_fee_precise = other.block_fee_precise;
    changed = true;
  }
  for (const auto& [ccy, price] : other.prices) {
    const auto it = prices.find(ccy);
    if (it == prices.end() || it->second != price) {
      prices[ccy] = price;
      changed = true;
    }
  }
  return changed;
}

const std::string* DataSnapshot::PriceOf(const std::string& ccy) const {
  const auto it = prices.find(ccy);
  return it == prices.end() ? nullptr : &it->second;
}

DataHub::~DataHub() { StopAll(); }

void DataHub::AddSource(std::unique_ptr<DataSource> src) {
  if (src) sources_.push_back(std::move(src));
}

esp_err_t DataHub::StartAll() {
  esp_err_t first_err = ESP_OK;
  for (auto& s : sources_) {
    ESP_LOGI(kTag, "start source: %s", s->name());
    const esp_err_t rc = s->Start(*this);
    if (rc != ESP_OK && first_err == ESP_OK) first_err = rc;
  }
  return first_err;
}

void DataHub::StopAll() {
  for (auto& s : sources_) {
    if (s) s->Stop();
  }
}

DataSnapshot DataHub::GetSnapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return snapshot_;
}

void DataHub::SetOnUpdate(UpdateCallback cb) {
  std::lock_guard<std::mutex> lk(mu_);
  on_update_ = std::move(cb);
}

void DataHub::Report(const DataSnapshot& partial) {
  UpdateCallback cb;
  DataSnapshot copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!snapshot_.Merge(partial)) return;
    copy = snapshot_;
    cb = on_update_;
  }
  if (cb) cb(copy);
}

}  // namespace btclock
