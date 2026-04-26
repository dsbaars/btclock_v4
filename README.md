# BTClock v4

Firmware for the BTClock — an ESP32-S3 device that displays Bitcoin
network data (block height, price, fee rate, halving countdown,
sats-per-currency, market cap, supply, mining-pool stats, Bitaxe
metrics) on e-paper panels with NeoPixel and frontlight feedback.

Three hardware variants share one codebase:

| Variant | Flash | PSRAM | Default panel | Notes                               |
|---------|-------|-------|---------------|-------------------------------------|
| Rev A   | 4 MB  | 2 MB  | 2.13"         | no BH1750, no frontlight            |
| Rev B   | 8 MB  | 2 MB  | 2.13"         | BH1750 ambient sensor, frontlight   |
| V8      | 16 MB | 8 MB  | 2.13"         | 8 panels                            |

Extracted from `btclock_v3_fci/idf_cpp_proto/`; Arduino-era history
lives in the [old repo](https://git.btclock.dev/btclock/btclock_v3).

## Build

Source the IDF environment, then build per variant. `BTCLOCK_BOARD`
picks the pin map; `BTCLOCK_PANEL` picks the EPD geometry — the two
are independent, so any board × any panel combo configures (default
panel is `2_13` for every board). `BTCLOCK_PANEL=2_9` swaps to the
2.9" GDEY029T94; `BTCLOCK_PANEL=7_5` (GDEY075T7, 800×480) is
scaffolded but un-flashed today. Each variant keeps its own
`sdkconfig` so they don't poison each other:

```bash
source ~/esp/v6.0/esp-idf/export.sh

idf.py -B build-rev-a    -D BTCLOCK_BOARD=REV_A -D BTCLOCK_PANEL=2_13 -D SDKCONFIG=build-rev-a/sdkconfig    build
idf.py -B build-rev-a-29 -D BTCLOCK_BOARD=REV_A -D BTCLOCK_PANEL=2_9  -D SDKCONFIG=build-rev-a-29/sdkconfig build
idf.py -B build-rev-b    -D BTCLOCK_BOARD=REV_B -D BTCLOCK_PANEL=2_13 -D SDKCONFIG=build-rev-b/sdkconfig    build
idf.py -B build-v8       -D BTCLOCK_BOARD=V8    -D BTCLOCK_PANEL=2_13 -D SDKCONFIG=build-v8/sdkconfig       build
```

Required toolchain: ESP-IDF v6.0 (v5.5.4 still works as a fallback).

## Flash

Identify your device's serial port (typically `/dev/ttyUSB*` or
`/dev/ttyACM*` on Linux, `/dev/cu.usbmodem*` on macOS, `COM*` on
Windows) — ports are not stable across sessions, so re-check each time.

Then flash with the build's `flash_args` file:

```bash
cd build-rev-a && \
  esptool.py --chip esp32s3 --port <PORT> -b 460800 \
    --before default_reset --after hard_reset write_flash "@flash_args"
```

OTA is also supported as a fallback when USB-JTAG is contested by the
running firmware:

```bash
curl -X POST -H "Content-Type: application/octet-stream" \
  --data-binary @build-rev-b/btclock_idf_proto.bin \
  http://<IP>/upload/firmware
```

OTA respects `httpAuthEnabled` — pass `-u user:pass` when auth is on.

## WebUI (LittleFS image)

The WebUI ships as a separate LittleFS partition. Source assets live
under `data/build_gz/www/`; the firmware serves them from
`/lfs/www`. Pack and flash per variant:

```bash
MKLFS=tools/mklittlefs/mklittlefs

# Rev A (4 MB)
$MKLFS --create data/build_gz --size 0x67000  --block 4096 --page 256 build-rev-a/storage.bin
# Rev B (8 MB)
$MKLFS --create data/build_gz --size 0xCD000  --block 4096 --page 256 build-rev-b/storage.bin
# V8 (16 MB)
$MKLFS --create data/build_gz --size 0x200000 --block 4096 --page 256 build-v8/storage.bin

# Flash at the per-variant offset (Rev A shown):
python -m esptool --chip esp32s3 --port <PORT> -b 460800 \
  write_flash 0x370000 build-rev-a/storage.bin
```

Per-variant offsets: Rev A `0x370000`, Rev B `0x6F0000`, V8 `0xDF0000`.

If the vendored `mklittlefs` binary is missing on a fresh clone, run
`tools/mklittlefs/fetch.sh` to fetch it.

## Host tests

A subset of the codebase (rendering layout, fee-rate parsing,
panel-text formatting, settings PATCH validation, LED prefs migration,
partition-table sanity) runs on the host without the IDF toolchain:

```bash
cmake -S test_host -B build-host && cmake --build build-host && \
  ./build-host/btclock_host_tests
```

Do **not** source the IDF env for these — they use the system
toolchain.

## Layout

- `main/`               — application entry, screen renderers, board headers
- `components/`         — reusable subsystems (data sources, EPD driver, LEDs, settings, web server, Nostr, etc.)
- `data/`               — WebUI submodule (Svelte; built into `data/build_gz/`)
- `tools/`              — flash helpers, WASM preview, font/timezone/NVS generators, pool-logo converter, EPD bring-up sketch, Nostr zap watcher, `mklittlefs` wrapper
- `test_host/`          — host-side regression suite
- `partitions_*mb.csv`  — partition tables per flash size

## CI

[Forgejo Actions](.forgejo/workflows/) drive two pipelines:

- `host_tests.yaml` — runs the host regression suite on every push and PR.
- `release.yaml` — tag-triggered (`v*`); gates on host tests, builds the
  WebUI once, packs per-size LittleFS images, then matrix-builds firmware
  for `rev-a`, `rev-a-29`, `rev-b`, `v8` and attaches per-variant zips
  to the Forgejo release. CI currently pins `espressif/idf:v6.0`.

## Documentation

- [docs/SETTINGS.md](docs/SETTINGS.md) — settings reference
- [docs/WEBUI_MINING_POOL_FIELDS.md](docs/WEBUI_MINING_POOL_FIELDS.md) — mining-pool credential field guide for WebUI authors
- [docs/HARDCODED_AUDIT.md](docs/HARDCODED_AUDIT.md) — inventory of hardcoded values
- [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md) — parity matrix vs. the Arduino btclock_v3 firmware
- [CLAUDE.md](CLAUDE.md) — agent-facing build/flash/conventions
