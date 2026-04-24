#include "app/boot/init_boot_leds.hpp"

#include "board/board.hpp"
#include "dnd/dnd.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "io/led_controller.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "poc";
}  // namespace

void InitBootLeds() {
  using namespace btclock::board;

  ESP_LOGI(kTag, "BTClock IDF C++ PoC — boot");
  // Label "heap" as the internal free bytes specifically — the default
  // allocator can pull from PSRAM on S3, which made the old print
  // report free > size when compared against the internal-only total
  // the status endpoints publish.
  ESP_LOGI(kTag, "psram=%uB heap_internal_free=%uB",
           static_cast<unsigned>(esp_psram_get_size()),
           static_cast<unsigned>(
               heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

  InitLeds(kNeopixel, kNeopixelCount);
  // Mirror the settings-namespace blockFlashColor into the LED
  // controller's own namespace. The WebUI PATCHes `blockFlashColor`
  // into `settings/blockFlashColor`, while the LED task reads its own
  // namespace (`led/blockFlashCol`, abbreviated for the 15-char NVS
  // cap). Without this sync, a newly-flashed device respects the
  // WebUI setting, but an existing install with a saved colour would
  // keep showing the default orange until the user re-saves — pull
  // once here so both match. Absence of a saved value keeps the
  // controller's own default.
  {
    Prefs settings_ns(prefs::kSettingsNs);
    // kDefaultBlockFlashColor duplicated here because led_controller.cpp
    // keeps it anonymous-namespace-local; keeping both in sync has
    // negligible cost since the value is only read at boot.
    constexpr uint32_t kDefaultBlockFlashColor = 0xE04300u;
    const uint32_t rgb = settings_ns.GetU32(prefs::kBlockFlashColor,
                                            kDefaultBlockFlashColor);
    SetBlockFlashColor(rgb & 0x00FFFFFFu);
  }
  // DND predicate goes in before the first effect post so the boot
  // rainbow is gated too if the user has DND currently armed.
  SetLedActiveSuppressor([] { return dnd::Instance().IsActive(); });
  PostLedEvent(LedEvent::kSetBoot);
}

}  // namespace btclock
