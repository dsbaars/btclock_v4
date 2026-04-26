#include <cstdio>

#include "screens/common.hpp"
#include "screens/screens.hpp"

namespace btclock {
namespace {

constexpr const char* kRef =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.:";

// Atkinson Hyperlegible regular/bold pair (purpose-built for low-DPI
// legibility). 22 px reads cleanly on the 2.13" panel — the prior 18 px
// looked cramped on the bench, especially the Heap/PSRAM/Uptime values.
constexpr float kBodyPx = 22.0f;

// "12345678" → "12.3 MB" style. KB for < 1 MB, MB with one decimal
// otherwise. Good enough for debug readout; no call-site cares about
// kB vs KiB precision here.
void FormatBytes(uint32_t bytes, char* out, size_t n) {
  if (bytes < 1024u * 1024u) {
    std::snprintf(out, n, "%u KB",
                  static_cast<unsigned>((bytes + 512u) / 1024u));
  } else {
    const unsigned whole = bytes / (1024u * 1024u);
    const unsigned tenths = (bytes % (1024u * 1024u)) / (1024u * 100u);
    std::snprintf(out, n, "%u.%u MB", whole, tenths);
  }
}

void FormatUptime(uint32_t seconds, char* out, size_t n) {
  const uint32_t d = seconds / 86400u;
  const uint32_t h = (seconds % 86400u) / 3600u;
  const uint32_t m = (seconds % 3600u) / 60u;
  const uint32_t s = seconds % 60u;
  if (d > 0) {
    std::snprintf(out, n, "%ud %uh %um", static_cast<unsigned>(d),
                  static_cast<unsigned>(h), static_cast<unsigned>(m));
  } else if (h > 0) {
    std::snprintf(out, n, "%uh %um %us", static_cast<unsigned>(h),
                  static_cast<unsigned>(m), static_cast<unsigned>(s));
  } else {
    std::snprintf(out, n, "%um %us", static_cast<unsigned>(m),
                  static_cast<unsigned>(s));
  }
}

}  // namespace

template <size_t N>
void RenderDebugScreen(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                       uint8_t (&fb_storage)[N][16 * 296],
                       const AppFonts& fonts, const DebugScreenInfo& info,
                       bool full_refresh) {
  static_assert(N >= 7, "debug layout needs at least 7 panels");

  const Font& reg = fonts.atkinson();
  const Font& bold = fonts.atkinson_bold();

  char heap_str[32];
  char psram_str[32];
  char uptime_str[48];
  FormatBytes(info.free_heap, heap_str, sizeof(heap_str));
  FormatBytes(info.free_psram, psram_str, sizeof(psram_str));
  FormatUptime(info.uptime_s, uptime_str, sizeof(uptime_str));

  // Panel 0 — title. Full-width auto-fit so longer boards ("Rev B")
  // still render without clipping.
  {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    const float px =
        FitTextPx("DEBUG", bold, 48.0f, 20.0f, lfb.native_width - 6);
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, "DEBUG", kRef,
                     bold, px, /*white_text=*/false);
  }
  // Panel 1 — IP.
  {
    auto lfb = PrepFb(panels, fb_storage, 1);
    ClearFb(lfb, true);
    char body[96];
    std::snprintf(body, sizeof(body), "*IP:*\n%s",
                  info.ip.empty() ? "-" : info.ip.c_str());
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kBodyPx, false);
  }
  // Panel 2 — SSID.
  {
    auto lfb = PrepFb(panels, fb_storage, 2);
    ClearFb(lfb, true);
    char body[96];
    std::snprintf(body, sizeof(body), "*SSID:*\n%s",
                  info.ssid.empty() ? "-" : info.ssid.c_str());
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kBodyPx, false);
  }
  // Panel 3 — free heap (internal DRAM).
  {
    auto lfb = PrepFb(panels, fb_storage, 3);
    ClearFb(lfb, true);
    char body[96];
    std::snprintf(body, sizeof(body), "*Heap:*\n%s", heap_str);
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kBodyPx, false);
  }
  // Panel 4 — free PSRAM.
  {
    auto lfb = PrepFb(panels, fb_storage, 4);
    ClearFb(lfb, true);
    char body[96];
    std::snprintf(body, sizeof(body), "*PSRAM:*\n%s", psram_str);
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kBodyPx, false);
  }
  // Panel 5 — HW variant + firmware build date.
  {
    auto lfb = PrepFb(panels, fb_storage, 5);
    ClearFb(lfb, true);
    char body[128];
    std::snprintf(body, sizeof(body), "*HW:*\n%s\n\n*Built:*\n%s", info.hw_name,
                  info.built);
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kBodyPx, false);
  }
  // Panel 6 — uptime.
  {
    auto lfb = PrepFb(panels, fb_storage, 6);
    ClearFb(lfb, true);
    char body[96];
    std::snprintf(body, sizeof(body), "*Uptime:*\n%s", uptime_str);
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kBodyPx, false);
  }
  // Panel 7 (V8 only) — URL hint on how to exit.
  if constexpr (N >= 8) {
    auto lfb = PrepFb(panels, fb_storage, 7);
    ClearFb(lfb, true);
    const char* body = "*Exit:*\npress\nbutton 4\nagain";
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kBodyPx, false);
  }
  const RefreshKind kind =
      full_refresh ? RefreshKind::kFull : RefreshKind::kPartial;
  for (size_t i = 0; i < N; ++i) {
    panels[i]->DrawFramebufferStart(fb_storage[i], kind);
  }
  for (size_t i = 0; i < N; ++i) {
    panels[i]->WaitForRefresh();
  }
}

template void RenderDebugScreen<7>(std::array<std::unique_ptr<EpdPanel>, 7>&,
                                   uint8_t (&)[7][16 * 296], const AppFonts&,
                                   const DebugScreenInfo&, bool);
template void RenderDebugScreen<8>(std::array<std::unique_ptr<EpdPanel>, 8>&,
                                   uint8_t (&)[8][16 * 296], const AppFonts&,
                                   const DebugScreenInfo&, bool);

}  // namespace btclock
