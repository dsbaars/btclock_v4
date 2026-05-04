// Thin adapters translating webserver-component interfaces into main-
// side handles.
//
// The webserver component speaks only in its own Iface abstract bases
// (FrontlightIface, LedsIface, DndIface, LightSensorIface, TimerIface)
// so it never has to link against FrontlightController, LightSensor,
// ScreenManager, the dnd singleton, or the namespace-level
// led_controller API. These adapters close that gap: each one holds a
// raw pointer (or reference) to the main-side handle and forwards the
// Iface calls to it.
//
// Every adapter lives as long as its backing handle — AppCtx owns
// both. ControlServer holds raw Iface* pointers; ControlServer itself
// is destroyed before the adapters in AppCtx because unique_ptrs
// destroy in reverse declaration order in the struct, and `ctrl` is
// declared before the adapter members.

#pragma once

#include <cstdint>

#include "control_server.hpp"

namespace btclock {

class FrontlightController;
class LightSensor;
class ScreenManager;

struct FrontlightAdapter : FrontlightIface {
  explicit FrontlightAdapter(FrontlightController* fl);
  void On() override;
  void Off() override;
  void Flash() override;
  void SetBrightness(uint16_t duty) override;
  Status GetStatus() const override;
  FrontlightController* fl_;
};

struct LedsAdapter : LedsIface {
  Status GetStatus() const override;
  void SetSolidColor(uint32_t rgb) override;
  void SetPixels(const uint32_t* rgb_array, uint32_t count) override;
  void SetDisabled(bool disabled) override;
  void SetBrightness(uint8_t value) override;
  void SetBlockFlashColor(uint32_t rgb) override;
  void TriggerIdentify() override;
  bool PostEffectByName(const char* name) override;
};

struct DndAdapter : DndIface {
  Status GetStatus() const override;
  void SetEnabled(bool enabled) override;
};

struct LightSensorAdapter : LightSensorIface {
  explicit LightSensorAdapter(LightSensor* ls);
  bool IsAvailable() const override;
  float GetLux() const override;
  LightSensor* ls_;
};

struct TimerAdapter : TimerIface {
  TimerAdapter(ScreenManager& sm_ref, int64_t (*now_fn)());
  bool IsPaused() const override;
  void SetPaused(bool paused) override;
  void Restart() override;
  ScreenManager& sm;
  int64_t (*now)();
};

}  // namespace btclock
