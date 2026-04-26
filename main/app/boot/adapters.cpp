#include "app/boot/adapters.hpp"

#include "app/screen_manager.hpp"
#include "dnd/dnd.hpp"
#include "io/frontlight_controller.hpp"
#include "io/led_controller.hpp"
#include "io/light_sensor.hpp"

namespace btclock {

// --- FrontlightAdapter ------------------------------------------------

FrontlightAdapter::FrontlightAdapter(FrontlightController* fl) : fl_(fl) {}

void FrontlightAdapter::On() {
  fl_->On();
}
void FrontlightAdapter::Off() {
  fl_->Off();
}
void FrontlightAdapter::Flash() {
  fl_->Flash();
}
void FrontlightAdapter::SetBrightness(uint16_t duty) {
  fl_->SetBrightness(duty);
}

FrontlightAdapter::Status FrontlightAdapter::GetStatus() const {
  const auto s = fl_->GetStatus();
  return Status{s.enabled,       s.current_duty,
                s.target_duty,   s.configured_brightness,
                s.lux_threshold, s.ambient_auto_off};
}

// --- LedsAdapter ------------------------------------------------------

LedsAdapter::Status LedsAdapter::GetStatus() const {
  const auto s = GetLedState();
  Status out{};
  out.brightness = s.brightness;
  out.block_flash_color = s.block_flash_color;
  out.disabled = s.disabled;
  out.flash_on_update = s.flash_on_update;
  out.pixel_count = s.pixel_count;
  const uint32_t cap = sizeof(out.pixels) / sizeof(out.pixels[0]);
  const uint32_t n = s.pixel_count < cap ? s.pixel_count : cap;
  for (uint32_t i = 0; i < n; ++i) out.pixels[i] = s.pixels[i];
  return out;
}

void LedsAdapter::SetSolidColor(uint32_t rgb) {
  SetLedSolidColor(rgb);
}
void LedsAdapter::SetPixels(const uint32_t* rgb_array, uint32_t count) {
  SetLedPixels(rgb_array, count);
}
void LedsAdapter::SetDisabled(bool disabled) {
  SetLedDisabled(disabled);
}
void LedsAdapter::SetBrightness(uint8_t value) {
  SetLedBrightness(value);
}
void LedsAdapter::SetBlockFlashColor(uint32_t rgb) {
  ::btclock::SetBlockFlashColor(rgb);
}
void LedsAdapter::TriggerIdentify() {
  PostLedEffect(LedEffect::kIdentify);
}

// --- DndAdapter -------------------------------------------------------

DndAdapter::Status DndAdapter::GetStatus() const {
  auto& d = dnd::Instance();
  const auto cfg = d.GetConfig();
  Status s{};
  s.enabled = cfg.enabled;
  s.time_enabled = cfg.time_enabled;
  s.start_hour = cfg.start_hour;
  s.start_minute = cfg.start_minute;
  s.end_hour = cfg.end_hour;
  s.end_minute = cfg.end_minute;
  s.active = d.IsActive();
  return s;
}

void DndAdapter::SetEnabled(bool enabled) {
  dnd::Instance().SetEnabled(enabled);
}

// --- LightSensorAdapter ----------------------------------------------

LightSensorAdapter::LightSensorAdapter(LightSensor* ls) : ls_(ls) {}

bool LightSensorAdapter::IsAvailable() const {
  return ls_ && ls_->IsAvailable();
}
float LightSensorAdapter::GetLux() const {
  return ls_ ? ls_->GetLux() : -1.0f;
}

// --- TimerAdapter -----------------------------------------------------

TimerAdapter::TimerAdapter(ScreenManager& sm_ref, int64_t (*now_fn)())
    : sm(sm_ref), now(now_fn) {}

bool TimerAdapter::IsPaused() const {
  return sm.IsPaused();
}
void TimerAdapter::SetPaused(bool paused) {
  sm.SetPaused(paused);
}
void TimerAdapter::Restart() {
  sm.RestartTimer(now());
}

}  // namespace btclock
