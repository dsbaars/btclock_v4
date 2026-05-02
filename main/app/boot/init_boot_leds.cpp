#include "app/boot/init_boot_leds.hpp"

#include "board/board.hpp"
#include "dnd/dnd.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "io/led_controller.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";
}  // namespace

void InitBootLeds() {
  using namespace btclock::board;

  ESP_LOGI(kTag, "BTClock v4 — boot");
  // Label "heap" as the internal free bytes specifically — the default
  // allocator can pull from PSRAM on S3, which made the old print
  // report free > size when compared against the internal-only total
  // the status endpoints publish.
  ESP_LOGI(kTag, "psram=%uB heap_internal_free=%uB",
           static_cast<unsigned>(esp_psram_get_size()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

  InitLeds(kNeopixel, kNeopixelCount);
  // LED prefs (brightness, blockFlashColor, disableLeds, ledFlashOnUpd)
  // are loaded inside InitLeds() from the shared `settings` NVS
  // namespace, so there's no separate boot-time mirror to maintain.
  // DND predicate goes in before the first effect post so the boot
  // rainbow is gated too if the user has DND currently armed.
  SetLedActiveSuppressor([] { return dnd::Instance().IsActive(); });
  // ledTestOnPower: skip the rainbow self-test entirely when the user
  // disabled it. Default true matches v3 DEFAULT_LED_TEST_ON_POWER and
  // the schema default. Use kPowerTest (one-shot rainbow scan ~1.4 s
  // then idle) rather than kSetBoot (continuous rolling rainbow) so the
  // rainbow only plays during the boot window — once boot dispatch
  // resolves the network state, the LEDs hand off to either the
  // provisioning breathe (AP mode) or the data-driven idle/event mix
  // (STA mode).
  Prefs settings(prefs::kSettingsNs);
  if (btclock::settings::ReadBool(settings, prefs::kLedTestOnPower)) {
    PostLedEvent(LedEvent::kPowerTest);
  } else {
    PostLedEvent(LedEvent::kSetIdle);
  }
}

}  // namespace btclock
