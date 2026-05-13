#include "boot_spinner.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "board/board.hpp"
#include "epd/panel.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace btclock {
namespace {
constexpr const char* kTag = "boot_spinner";

// Programmatic 3/4-ring spinner. Four 90° steps per rotation, paired
// with epd::SetGlobalFastPartial() bypassing both the SSD1680's
// per-frame HardReset + Init re-init (~80 ms/frame) AND the
// post-refresh power-off cycle (~10-30 ms/frame). Each frame now
// lands in ~210 ms — basically the chip's physical partial-refresh
// waveform floor (~200 ms) plus a few ms of SPI command + DMA. Full
// rotation ≈ 0.84 s. 90° per snap reads as clean rotation without
// crossing into the choppy 120° regime.
constexpr int kSteps = 4;

// 50/38 px gives a 12 px ring stroke at 100 px diameter — heavy enough
// to be readable across the panel's ~1 mm pixel pitch.
constexpr int kRouter = 50;
constexpr int kRinner = 38;

// Per-step gap quadrant signs (screen-y points down, so positive dy
// = visually downward). With kSteps=4 and a 90° gap, the gap fills
// exactly one quadrant per frame:
//   step 0 → +dx +dy (bottom-right)
//   step 1 → -dx +dy (bottom-left)
//   step 2 → -dx -dy (top-left)
//   step 3 → +dx -dy (top-right)
// dx*gx>0 && dy*gy>0 thus reproduces the original cross-product
// wedge test for sin/cos∈{-1,0,1} without any float arithmetic —
// saves ~400 B of soft-float helper pull-in vs. the sinf/cosf path
// (the spinner only ever evaluates four discrete angles).
constexpr int kGapX[] = {+1, -1, -1, +1};
constexpr int kGapY[] = {+1, +1, -1, -1};
static_assert(sizeof(kGapX) / sizeof(kGapX[0]) == 4,
              "kGapX/kGapY assume kSteps == 4");

// Inter-frame yield. 0 ticks returns immediately if no stop-notify is
// pending — the surrounding driver path already calls vTaskDelay
// during BUSY polling, so other tasks still get scheduled even with
// no sleep here. Every ms saved off the cycle counts: the floor is
// the ~200 ms physical partial-refresh waveform we can't bypass.
constexpr TickType_t kFrameDelay = 0;

constexpr uint8_t kWhiteByte = 0xFF;

struct SpinnerState {
  epd::IEpdPanel* panel = nullptr;
  uint8_t* fb = nullptr;
  int native_stride = 0;
  int native_width = 0;
  int native_height = 0;
};

SpinnerState g_state;
std::atomic<bool> g_stop_requested{false};
SemaphoreHandle_t g_stopped_sem = nullptr;
TaskHandle_t g_task = nullptr;

// Paint one frame of the ring spinner into `fb` (native coords). The
// gap rotates clockwise as `step` increases. Inks bits to 0 (black);
// caller is responsible for clearing `fb` to 0xFF first.
//
// Strict inequalities (dx*gx > 0, not >=) match the original cross-
// product wedge test: axis pixels (dx==0 or dy==0) lie on the gap
// boundary rays themselves and paint regardless of `step`.
void PaintSpinnerFrame(uint8_t* fb, int stride, int native_w, int native_h,
                       int step) {
  const int cx = native_w / 2;
  const int cy = native_h / 2;
  const int r_outer_sq = kRouter * kRouter;
  const int r_inner_sq = kRinner * kRinner;
  const int gx = kGapX[step & 3];
  const int gy = kGapY[step & 3];

  for (int dy = -kRouter; dy <= kRouter; ++dy) {
    for (int dx = -kRouter; dx <= kRouter; ++dx) {
      const int rsq = dx * dx + dy * dy;
      if (rsq > r_outer_sq || rsq < r_inner_sq) continue;
      if (dx * gx > 0 && dy * gy > 0) continue;  // in gap
      const int nx = cx + dx;
      const int ny = cy + dy;
      if (nx < 0 || nx >= native_w) continue;
      if (ny < 0 || ny >= native_h) continue;
      const int byte_idx = ny * stride + (nx >> 3);
      const uint8_t bit = static_cast<uint8_t>(1U << (7 - (nx & 7)));
      fb[byte_idx] &= static_cast<uint8_t>(~bit);
    }
  }
}

void SpinnerTask(void* /*arg*/) {
  // Main task already painted step 0 with a parallel kFull during
  // StartBootSpinner — pick up at step 1 and partial-refresh from there.
  int step = 1;
  while (!g_stop_requested.load(std::memory_order_acquire)) {
    std::memset(
        g_state.fb, kWhiteByte,
        static_cast<size_t>(g_state.native_stride) * g_state.native_height);
    PaintSpinnerFrame(g_state.fb, g_state.native_stride, g_state.native_width,
                      g_state.native_height, step);
    g_state.panel->DrawFramebufferStart(g_state.fb, RefreshKind::kPartial);
    g_state.panel->WaitForRefresh();
    step = (step + 1) % kSteps;
    ulTaskNotifyTake(pdTRUE, kFrameDelay);
  }
  xSemaphoreGive(g_stopped_sem);
  g_task = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

void StartBootSpinner(AppCtx& ctx) {
  if (g_task != nullptr) return;
  if (g_stopped_sem == nullptr) {
    g_stopped_sem = xSemaphoreCreateBinary();
    if (g_stopped_sem == nullptr) {
      ESP_LOGW(kTag, "stopped-sem alloc failed; spinner disabled");
      return;
    }
  }
  g_stop_requested.store(false, std::memory_order_release);

  // 7-panel boards (Rev A/B) → idx 3; 8-panel V8 → idx 4. Either way
  // visually central enough that the splash → spinner transition reads
  // as "the middle panel is the busy one".
  const std::size_t middle = btclock::board::kNumPanels / 2;
  g_state.panel = ctx.panels[middle].get();
  g_state.fb = AppCtx::fb_storage()[middle];
  g_state.native_stride = g_state.panel->Stride();
  g_state.native_width = g_state.panel->Width();
  g_state.native_height = g_state.panel->Height();

  // Enable the SSD1680 fast-partial bypass for the duration of the
  // spinner. Applies to every panel — that's fine because we're the
  // only paint path active in this window, and the first post-spinner
  // sm->Render is forced kFull which always re-inits.
  epd::SetGlobalFastPartial(true);

  // Clear every panel framebuffer to white and paint the spinner's
  // first frame on the middle one. kPartial here saves ~1.4 s vs
  // kFull at the cost of a faint splash-letter ghost on the outer
  // panels for a few seconds — acceptable: the splash → spinner
  // transition needs to feel snappy, and the subsequent first
  // sm->Render uses kFull (refresh_policy::kForceFull) which scrubs
  // those panels clean anyway.
  for (std::size_t i = 0; i < btclock::board::kNumPanels; ++i) {
    auto* p = ctx.panels[i].get();
    std::memset(AppCtx::fb_storage()[i], kWhiteByte,
                static_cast<size_t>(p->Stride()) * p->Height());
  }
  PaintSpinnerFrame(g_state.fb, g_state.native_stride, g_state.native_width,
                    g_state.native_height, /*step=*/0);
  for (std::size_t i = 0; i < btclock::board::kNumPanels; ++i) {
    ctx.panels[i]->DrawFramebufferStart(AppCtx::fb_storage()[i],
                                        RefreshKind::kPartial);
  }
  for (std::size_t i = 0; i < btclock::board::kNumPanels; ++i) {
    ctx.panels[i]->WaitForRefresh();
  }

  // Priority 5 stays well below main (20) / WiFi (23) / esp_timer (22)
  // so the spinner never starves init steps or RF servicing. 4 KiB
  // stack is comfortably above the bit-twiddle + SSD1680 SPI command
  // path — no font.cpp scratch usage on this task.
  if (xTaskCreatePinnedToCore(SpinnerTask, "boot_spinner", 4096, nullptr,
                              /*priority=*/5, &g_task, 0) != pdPASS) {
    ESP_LOGW(kTag, "task create failed; spinner disabled");
    g_task = nullptr;
  }
}

void StopBootSpinner() {
  if (g_task == nullptr) {
    // Defensive: clear the flag even if Start never ran or already
    // shut down — data renders MUST go through the stock re-init path.
    epd::SetGlobalFastPartial(false);
    return;
  }
  TaskHandle_t to_wake = g_task;
  g_stop_requested.store(true, std::memory_order_release);
  xTaskNotifyGive(to_wake);
  xSemaphoreTake(g_stopped_sem, portMAX_DELAY);
  epd::SetGlobalFastPartial(false);
}

}  // namespace btclock
