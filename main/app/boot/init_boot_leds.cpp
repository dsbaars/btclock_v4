#include "app/boot/init_boot_leds.hpp"

#include "board/board.hpp"
#include "dnd/dnd.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "io/led_controller.hpp"

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
  // DND predicate goes in before the first effect post so the boot
  // rainbow is gated too if the user has DND currently armed.
  SetLedActiveSuppressor([] { return dnd::Instance().IsActive(); });
  PostLedEvent(LedEvent::kSetBoot);
}

}  // namespace btclock
