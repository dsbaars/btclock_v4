#include "app/boot/init_hardware.hpp"

#include "app/app_ctx.hpp"
#include "io/frontlight_controller.hpp"
#include "io/light_sensor.hpp"
#include "board/board.hpp"
#include "dnd/dnd.hpp"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";
}  // namespace

void InitHardware(AppCtx& ctx) {
  using namespace btclock::board;

  // --- I2C bus + peripherals ---
  ctx.i2c.emplace(I2C_NUM_0, kI2cSda, kI2cScl);

  // V8: drive the 6 MCP address-strap GPIOs BEFORE releasing RESET,
  // otherwise both MCP23017s latch to the default 000 → 0x20 and
  // collide on the I2C bus (symptom: bus scan finds one device only).
  // Matches the old Arduino firmware's setupMcp().
  if constexpr (kHasMcpAddressGpios) {
    uint64_t mask = 0;
    for (gpio_num_t pin : kMcpAddressGpios) {
      mask |= 1ULL << static_cast<int>(pin);
    }
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    for (size_t i = 0; i < kMcpAddressGpios.size(); ++i) {
      gpio_set_level(kMcpAddressGpios[i], kMcpAddressLevels[i] ? 1 : 0);
    }
  }

  // V8: pulse the shared MCP RESET line once before any I2C transaction.
  // >= 5 µs low is the datasheet minimum; 5 ms is conservative. Address
  // straps set just above must be stable before this pulse.
  if constexpr (kHasMcpResetGpio) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << static_cast<int>(kMcpResetGpio);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(kMcpResetGpio, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(kMcpResetGpio, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  ctx.mcp.emplace(*ctx.i2c, kMcp1Addr);
  ESP_ERROR_CHECK(ctx.mcp->SetDirectionPort(0xFFFF));
  // Buttons on pins 0..3 for every variant so far.
  for (uint8_t p = 0; p < 4; ++p) {
    ESP_ERROR_CHECK(
        ctx.mcp->SetDirection(p, Mcp23017::PinMode::kInputPullup));
  }
  // Rev A/B: RESET on pins 8..14, preload high for a known-good state.
  // V8 has RESET on MCP2, so skip.
  if constexpr (!kHasSecondMcp) {
    ESP_ERROR_CHECK(ctx.mcp->WritePort(0xFF00));
  }

  if constexpr (kHasSecondMcp) {
    ctx.mcp2.emplace(*ctx.i2c, kMcp2Addr);
    ESP_ERROR_CHECK(ctx.mcp2->SetDirectionPort(0x0000));
    ESP_ERROR_CHECK(ctx.mcp2->WritePort(0xFFFF));
  }

  if constexpr (kHasFrontlight) {
    ctx.pca.emplace(*ctx.i2c, kPcaAddr, kPcaOe);
    ESP_ERROR_CHECK(ctx.pca->Begin(1000));
    ctx.pca->SetOutputEnable(true);
    ctx.frontlight = std::make_unique<FrontlightController>(
        *ctx.pca, kFrontlightChannelFirst, kFrontlightChannelCount);

    // Pull the ambient-auto prefs out of NVS once at boot. Policy
    // setters poke plain scalar state in the controller — safe to call
    // before Start() because they don't go through the command queue.
    // Matches the v3 handleFrontlight() contract (src/main.cpp:31-47)
    // plus the flOffWhenDark branch on line 38.
    //
    // The boot-time read must stay in sync with the live PATCH path in
    // init_control_api.cpp's on_frontlight_changed hook — both read
    // the same NVS keys with the same defaults so a fresh install and
    // a runtime PATCH converge on identical controller state.
    {
      Prefs settings(prefs::kSettingsNs);
      const uint32_t lux_threshold = settings.GetU32(
          prefs::kLuxLightToggle, frontlight::kDefaultLuxThreshold);
      const bool off_when_dark =
          settings.GetBool(prefs::kFlOffWhenDark, true);
      ctx.frontlight->SetLuxThreshold(lux_threshold);
      ctx.frontlight->SetOffWhenDark(off_when_dark);
      // `luxLightToggle == 0` disables the whole feature in v3.
      ctx.frontlight->SetAmbientAutoOff(lux_threshold != 0);
      // flAlwaysOn / flDisable / flFlashOnUpd — schema defaults match
      // v3 (true / false / true). Without these the controller would
      // sit at its constructor defaults and three user-visible Rev B
      // toggles would be inert (btclock_v4-63p).
      ctx.frontlight->SetAlwaysOn(
          settings.GetBool(prefs::kFlAlwaysOn, true));
      ctx.frontlight->SetDisabled(
          settings.GetBool(prefs::kFlDisable, false));
      ctx.frontlight->SetFlashOnUpdate(
          settings.GetBool(prefs::kFlFlashOnUpd, true));
    }

    // Install DND gate before Start() so the boot fade-in is
    // suppressed when a schedule is already active at power-up.
    ctx.frontlight->SetActiveSuppressor(
        [] { return dnd::Instance().IsActive(); });
    ctx.frontlight->Start();

    // Brightness goes through the command queue, so wait until Start()
    // has created the queue before setting it. The initial kOn below
    // then fades us up to this new configured level.
    {
      Prefs settings(prefs::kSettingsNs);
      const uint32_t max_brightness =
          settings.GetU32(prefs::kFlMaxBrightness,
                          static_cast<uint32_t>(frontlight::kDefaultMaxDuty));
      if (max_brightness > 0 && max_brightness <= 0xFFFFu) {
        ctx.frontlight->SetConfiguredBrightness(
            static_cast<uint16_t>(max_brightness));
      }
      // flEffectDelay: drives staggered-flash cadence. Clamped to the
      // schema range (0..1000) implicitly — Prefs returns an unsigned
      // value and the stagger helper handles small/zero values by
      // flooring the per-LED delay at 1ms.
      const uint32_t effect_delay = settings.GetU32(
          prefs::kFlEffectDelay, frontlight::kDefaultEffectDelayMs);
      ctx.frontlight->SetEffectDelay(effect_delay);
    }
    // Fade up to the configured brightness at boot. Matches old
    // firmware, which powered the frontlight on once the panels were
    // initialised.
    ctx.frontlight->On();
  }

  if constexpr (kHasAmbientLight) {
    ctx.bh.emplace(*ctx.i2c, kBhAddr);
    // Init() probes the bus, so a depopulated/absent sensor reports
    // ESP_ERR_NOT_FOUND rather than aborting the boot — the downstream
    // LightSensor manager then stays in IsAvailable()==false mode and
    // /api/settings suppresses the lightLevel field.
    const esp_err_t ierr = ctx.bh->Init();
    if (ierr != ESP_OK) {
      ESP_LOGW(kTag, "BH1750 init failed: %s — continuing without ambient",
               esp_err_to_name(ierr));
    }
    ctx.light_sensor = std::make_unique<LightSensor>(*ctx.bh);
    ctx.light_sensor->Start();
  }
}

}  // namespace btclock
