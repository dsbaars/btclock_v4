#include "epd/drivers/gdey0213b74.hpp"

// All behaviour for this panel lives in Ssd1680Base — the GDEY0213B74
// datasheet is what the base class is calibrated against.
// Width()/Height() in the .hpp are the only specialisations needed;
// everything else (border 0x05, DUC2 0xF7/0xFC, slow-full path,
// shadow→0x26 priming, no fast-full) carries through from the base.
//
// Keep this TU around so the link graph has an actual definition
// to reach (the hpp is header-only otherwise) and so any
// behavioural drift can land in one obvious place.

namespace btclock {
namespace epd {

// Anchor symbol so the linker pulls this TU when the factory
// references Gdey0213B74. (The base destructor is virtual; the
// out-of-line dtor is the conventional anchor, but the using-
// declaration in the header inherits the base ctors so there's
// no body-bearing virtual to define here. A no-op TU is enough.)

}  // namespace epd
}  // namespace btclock
