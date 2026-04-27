#include "epd/drivers/gdey029t94.hpp"

#include "esp_check.h"

namespace btclock {
namespace epd {

namespace {
constexpr const char* kTag = "gdey029t94";
constexpr uint8_t kCmdDriverOutputCtl = 0x01;
}  // namespace

esp_err_t Gdey029T94::WriteDriverOutputControl() {
  // The GDEY029T94 datasheet specifies:
  //   _writeData(0x27); _writeData(0x01); _writeData(0x00);
  // (HEIGHT-1 = 295 = 0x0127, gate scan dir = 0x00). The base class
  // computes this dynamically from Height(); we override anyway so a
  // future Height() change can't silently mis-bring-up the panel —
  // the datasheet values are the source of truth, not Height().
  const uint8_t dout[3] = {0x27, 0x01, 0x00};
  return cfg_.bus->SendCommand(cfg_.cs, kCmdDriverOutputCtl, dout, 3);
}

}  // namespace epd
}  // namespace btclock
