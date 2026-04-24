// Host tests for the BH1750 raw->lux conversion. The full driver pulls
// in the ESP-IDF I2C master API, but the numeric helper lives in its
// own header with zero dependencies so we can exercise it offline.

#include "doctest.h"

#include <cstdint>

#include "bh1750_math.hpp"

using btclock::bh1750::RawToLux;

TEST_CASE("BH1750 raw counts convert to lux per datasheet formula") {
  // Zero in, zero out — no offset term in the datasheet equation.
  CHECK(RawToLux(0) == doctest::Approx(0.0f));

  // Datasheet example: 0x0BA6 == 2982 counts == 2485 lx (2982 / 1.2).
  CHECK(RawToLux(0x0BA6) == doctest::Approx(2485.0f).epsilon(0.01f));

  // 12 counts == exactly 10 lx — sanity-checks the /1.2 divisor.
  CHECK(RawToLux(12) == doctest::Approx(10.0f));

  // Full-scale 16-bit (65535) saturates at 54612.5 lx, matching the
  // datasheet's "Measurement range: 1 to 65535 lx" upper bound after
  // the /1.2 scaling.
  CHECK(RawToLux(0xFFFF) == doctest::Approx(54612.5f).epsilon(0.001f));
}
