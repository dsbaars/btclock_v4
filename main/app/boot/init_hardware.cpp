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

namespace btclock {
namespace {
constexpr const char* kTag = "poc";
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
    // Install DND gate before Start() so the boot fade-in is
    // suppressed when a schedule is already active at power-up.
    ctx.frontlight->SetActiveSuppressor(
        [] { return dnd::Instance().IsActive(); });
    ctx.frontlight->Start();
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
