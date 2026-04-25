// GDEY075T7 — UNTESTED scaffold. See gdey075t7.hpp for the caveat.

#include "epd/drivers/gdey075t7.hpp"

namespace btclock {
namespace epd {

// All behaviour lives in Uc8179Base. The .hpp specialises Width()
// (800), Height() (480) and Stride() (100). Future bring-up may
// need to override Init() / DrawFramebufferStart() if the GDEY075T7
// silicon needs tweaks beyond the GxEPD2 reference baseline.

}  // namespace epd
}  // namespace btclock
