#include "data_core/hub.hpp"

#include <utility>

#include "data_core/block_height_validator.hpp"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "data-hub";
}  // namespace

bool DataSnapshot::Merge(const DataSnapshot& other) {
  bool changed = false;
  if (other.block_height &&
      (!block_height || *block_height != *other.block_height)) {
    // Reject corrupt frames (height=0, regression below current tip)
    // before they reach the canonical snapshot. Otherwise the next
    // ConsumeNewBlock would treat the bogus value as a real new-block
    // event and fire LED + frontlight + steal-focus side effects.
    // Mirrors v3 commit b435552. The wild-jump catch-up case (legit
    // ≥100-block deltas after a long offline window) is handled at
    // event-loop layer by BlockEventPolicy::IsCatchUpJump.
    const uint32_t prev = block_height.value_or(0);
    if (BlockHeightValidator::IsValidUpdate(prev, *other.block_height)) {
      block_height = other.block_height;
      changed = true;
    } else {
      ESP_LOGW(kTag,
               "rejected block_height update: prev=%u new=%u "
               "(zero/regression)",
               static_cast<unsigned>(prev),
               static_cast<unsigned>(*other.block_height));
    }
  }
  if (other.block_fee && (!block_fee || *block_fee != *other.block_fee)) {
    block_fee = other.block_fee;
    changed = true;
  }
  if (other.block_fee_precise &&
      (!block_fee_precise || *block_fee_precise != *other.block_fee_precise)) {
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
  // Pool stats: a non-empty name in `other` is the signal "this partial
  // carries a fresh sample". Merge the whole substruct as a unit — we
  // don't try to merge field-by-field because hashrate/workers/sats all
  // come from the same API response and should be consistent.
  if (!other.pool.name.empty()) {
    if (pool.name != other.pool.name || pool.hashrate != other.pool.hashrate ||
        pool.daily_sats != other.pool.daily_sats ||
        pool.workers != other.pool.workers ||
        pool.estimated_sats != other.pool.estimated_sats) {
      pool = other.pool;
      changed = true;
    }
  }
  // Bitaxe: a non-empty hostname in `other` is the "fresh sample"
  // signal. Like pool stats, we merge the whole substruct as a unit so
  // hashrate / diff / temp always come from the same poll tick.
  if (!other.bitaxe.hostname.empty()) {
    if (bitaxe.hostname != other.bitaxe.hostname ||
        bitaxe.hashrate_ghs != other.bitaxe.hashrate_ghs ||
        bitaxe.best_diff != other.bitaxe.best_diff ||
        bitaxe.temperature_c != other.bitaxe.temperature_c ||
        bitaxe.shares_accepted != other.bitaxe.shares_accepted) {
      bitaxe = other.bitaxe;
      changed = true;
    }
  }
  // Latest zap — only the most recent wins. received_ms == 0 means
  // "caller didn't populate the zap substruct"; skip so unrelated
  // partial snapshots can't clobber a real receipt. Equal timestamps
  // no-op so a retransmit of the same event doesn't spam update
  // callbacks.
  if (other.latest_zap.received_ms > latest_zap.received_ms) {
    latest_zap = other.latest_zap;
    changed = true;
  }
  // NWC balance — null in `other` means "not produced by this partial";
  // a populated value overwrites unconditionally because the NWC client
  // is the sole producer (no fan-in races between sources).
  if (other.nwc_balance_msat &&
      (!nwc_balance_msat || *nwc_balance_msat != *other.nwc_balance_msat)) {
    nwc_balance_msat = other.nwc_balance_msat;
    changed = true;
  }
  // NWC payment notification — same monotonic-stamp rule as latest_zap.
  // `received_ms == 0` means the producer didn't populate the substruct,
  // so don't clobber a real notification.
  if (other.nwc_last_payment.received_ms > nwc_last_payment.received_ms) {
    nwc_last_payment = other.nwc_last_payment;
    changed = true;
  }
  return changed;
}

const std::string* DataSnapshot::PriceOf(const std::string& ccy) const {
  const auto it = prices.find(ccy);
  return it == prices.end() ? nullptr : &it->second;
}

DataHub::~DataHub() {
  StopAll();
}

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
