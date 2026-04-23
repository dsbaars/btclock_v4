# BTClock ESP-IDF C++ PoC

Proof-of-concept for the pure-ESP-IDF v5.5 C++ rewrite. Each round
validates one slice of the plan in [btclock_v3_fci-xyb](../idf_cpp_proto).

## Rounds

- **Round 1** (current): boot + serial banner + WS2812 ring animation on
  GPIO 15. Validates toolchain, USB flash, C++, RMT, `led_strip`.
- **Round 2**: I2C bring-up; MCP23017, PCA9685, BH1750 components.
- **Round 3**: SSD1680 e-paper via `espressif/esp_lcd_ssd1681`.

## Build + flash

```bash
. ~/esp/v5.5.4/esp-idf/export.sh
cd idf_cpp_proto
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem833301 flash monitor
```

Target hardware: Rev B (`btclock_rev_b`).
