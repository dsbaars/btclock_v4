#include "epd/factory.hpp"

#include "epd/drivers/gdey0213b74.hpp"
#include "epd/drivers/gdey029t94.hpp"
#include "epd/drivers/gdey075t7.hpp"

namespace btclock {
namespace epd {

// Compile-time dispatch keyed off BTCLOCK_PANEL_<X>=1 from the top-
// level CMakeLists.txt. The active panel's TU is the only one
// instantiated here, but every driver TU still compiles
// unconditionally (cmake REQUIRES the same SRC list regardless of
// panel) so static analysis catches regressions across all three.
std::unique_ptr<IEpdPanel> CreatePanel(const PanelConfig& cfg) {
#if defined(BTCLOCK_PANEL_2_9)
  return std::make_unique<Gdey029T94>(cfg);
#elif defined(BTCLOCK_PANEL_7_5)
  return std::make_unique<Gdey075T7>(cfg);
#elif defined(BTCLOCK_PANEL_2_13)
  return std::make_unique<Gdey0213B74>(cfg);
#else
  // No-default branch — CMake should have failed configure before
  // reaching this. Keep the TU well-formed so the file compiles
  // even if a future build path forgets to plumb the macro.
# error "No BTCLOCK_PANEL_<X> macro defined; check top-level CMakeLists.txt"
#endif
}

}  // namespace epd
}  // namespace btclock
