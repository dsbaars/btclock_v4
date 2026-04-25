#include "epd/drivers/gdey029t94.hpp"

#include "esp_check.h"

namespace btclock {
namespace epd {

namespace {
constexpr const char* kTag = "gdey029t94";
constexpr uint8_t kCmdDriverOutputCtl = 0x01;
}  // namespace

esp_err_t Gdey029T94::WriteDriverOutputControl() {
  // GxEPD2_290_GDEY029T94::_InitDisplay literally hard-codes
  //   _writeData(0x27); _writeData(0x01); _writeData(0x00);
  // (HEIGHT-1 = 295 = 0x0127, gate scan dir = 0x00). The base class
  // computes this dynamically from Height(); we override anyway so a
  // future Height() change can't silently mis-bring-up the panel —
  // the GxEPD2 reference is the source of truth for these three
  // bytes, not Height(). Use the literal values.
  const uint8_t dout[3] = {0x27, 0x01, 0x00};
  return cfg_.bus->SendCommand(cfg_.cs, kCmdDriverOutputCtl, dout, 3);
}

}  // namespace epd
}  // namespace btclock
