// Globals shared across every concrete driver: the inverted-color
// polarity flag. Lifted out of the old monolithic epd_ssd1680.cpp so
// the per-IC driver TUs stay narrow.

#include <atomic>

#include "epd/panel.hpp"

namespace btclock {
namespace epd {

// std::atomic so a PATCH-driven toggle on the webserver task is
// visible to the main task's next render. One flag is enough — every
// panel ships the same 1=white,0=black framebuffer convention so
// flipping at the flush layer gives every renderer the inverted look
// without any per-renderer change.
namespace {
std::atomic<bool> g_inverted{false};
}  // namespace

void SetGlobalInverted(bool inverted) {
  g_inverted.store(inverted);
}
bool GetGlobalInverted() {
  return g_inverted.load();
}

bool InvertedNow() {
  return g_inverted.load();
}

}  // namespace epd
}  // namespace btclock
