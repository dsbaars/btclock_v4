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

Arduino-era history lives in the
[old repo](https://git.btclock.dev/btclock/btclock_v3); v4 was a
clean ESP-IDF C++ rewrite of that codebase.

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

Required toolchain: ESP-IDF v6.0.1 (v5.5.4 still works as a fallback).

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
  --data-binary @build-rev-b/btclock_v4.bin \
  http://<IP>/upload/firmware
```

OTA respects `httpAuthEnabled` — pass `-u user:pass` when auth is on.

## Crash diagnostics (coredump)

Rev B and V8 capture panic backtraces to a dedicated coredump partition
(64 KiB at the end of flash). When a crash happens, the next boot logs
`coredump from previous run present (N bytes)`. Pull it off the device
as an ELF and decode with `espcoredump.py`:

```bash
curl -o dump.elf http://<IP>/api/coredump        # 404 if no dump
espcoredump.py info_corefile -c dump.elf build-rev-b/btclock_v4.elf
curl -X DELETE http://<IP>/api/coredump          # clear after decode
```

Both endpoints respect `httpAuthEnabled` — add `-u user:pass` when auth
is on. Rev A disables coredump capture (the 4 MB flash leaves no
headroom in the app partition); panics on Rev A still print to serial
but aren't persisted across reboots.

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

## Linting

Style is enforced by `clang-format` (config at `.clang-format`) and
`clang-tidy` (config at `.clang-tidy`):

```bash
tools/lint/format.sh           # format in place
tools/lint/format.sh --check   # CI-style verify
tools/lint/tidy.sh             # static analysis (advisory today)
```

`format.sh` covers `components/`, `main/`, `test_host/` (excluding
`vendor/` and `build*/`). `tidy.sh` runs against the host-test sources
because they're the only TU set with a tractable include graph; for
firmware-side TUs run `clang-tidy -p build-rev-b path/to/file.cpp`
locally. macOS users need `brew install clang-format llvm`; the LLVM
formula provides `clang-tidy`. **CI is pinned to LLVM 22** — output
between majors drifts (Include block grouping, line wrapping
heuristics), so use a matching version locally to avoid sweep churn.
Brew currently ships LLVM 22.

Both the format check and the tidy job run in CI (`lint.yaml`) and
gate merges. `WarningsAsErrors` is `*` in `.clang-tidy`, so any new
violation of the configured checks fails the build — fix or NOLINT
before merging.

## Sanitizers

ASan + UBSan catch use-after-free, OOB reads, signed overflow, and
misaligned loads that boot fine on macOS local but misbehave on the
ESP32-S3. The host-test suite has an opt-in sanitizer build (default
OFF, gated in CI):

```bash
cmake -S test_host -B build-host-san -DBTCLOCK_HOST_TESTS_SANITIZE=ON
cmake --build build-host-san
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ./build-host-san/btclock_host_tests
```

macOS ASan does not implement leak detection — drop `detect_leaks=1`
locally on macOS; CI runs Linux where leaks are flagged.

## Fuzzing

libFuzzer harnesses cover the hand-rolled parsers most exposed to
external bytes — the NIP-01 envelope parser
(`components/nostr/src/parser.cpp`) and the HTTP query-string decoder
(`components/webserver/url_decode.cpp`). Off by default; clang-only.
Apple clang ships without libFuzzer, so on macOS use Homebrew LLVM:

```bash
cmake -S test_host -B build-fuzz -DBTCLOCK_FUZZ=ON \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
cmake --build build-fuzz
mkdir -p build-fuzz/url_decode_workdir
./build-fuzz/url_decode_fuzzer build-fuzz/url_decode_workdir \
  test_host/fuzz_corpus/url_decode/ -max_total_time=300
```

Run with a workdir that is **not** the seed-corpus directory —
libFuzzer auto-saves discoveries into the first positional arg, and
pointing it at the version-controlled seed dir would pollute the
tree with thousands of hash-named files. Replace `url_decode_fuzzer`
+ `url_decode/` with `nostr_parser_fuzzer` + `nostr_parser/` to fuzz
the Nostr parser instead.

## Coverage

Optional gcov instrumentation produces an HTML coverage report. CI
runs this as an informational job and uploads the HTML as an artifact
(retention 14 days).

```bash
cmake -S test_host -B build-host-cov -DBTCLOCK_HOST_TESTS_COVERAGE=ON
cmake --build build-host-cov && ./build-host-cov/btclock_host_tests
gcovr --root . --filter 'components/' --filter 'main/' \
  --exclude 'test_host/' --exclude '.*vendor/' \
  --html-details build-host-cov/coverage.html --print-summary \
  build-host-cov/
open build-host-cov/coverage.html
```

macOS local needs `brew install gcovr`. Coverage flags work with both
gcc and clang.

## Layout

- `main/`               — application entry, screen renderers, board headers
- `components/`         — reusable subsystems (data sources, EPD driver, LEDs, settings, web server, Nostr, etc.)
- `data/`               — WebUI submodule (Svelte; built into `data/build_gz/`)
- `tools/`              — flash helpers, WASM preview, font/timezone/NVS generators, pool-logo converter, EPD bring-up sketch, Nostr zap watcher, `mklittlefs` wrapper
- `test_host/`          — host-side regression suite
- `partitions_*mb.csv`  — partition tables per flash size

## CI

[Forgejo Actions](.forgejo/workflows/) drive two pipelines:

- `host_tests.yaml` — runs the host regression suite on every push and
  PR; also runs an ASan + UBSan build (gating) and a gcov coverage
  build (informational, uploads HTML report as an artifact).
- `lint.yaml` — clang-format check (gating) + clang-tidy (advisory).
- `release.yaml` — tag-triggered (`[0-9]*`); gates on host tests,
  builds the WebUI once, packs per-size LittleFS images, then
  matrix-builds firmware for `rev-a`, `rev-a-29`, `rev-b`, `v8` and
  attaches the flat per-variant `btclock_<variant>_ota.bin`
  (+ `.sha256` + `.md5`), shared support binaries, and a top-level
  `manifest.json` (per-variant board / panel / firmware SHA / WebUI
  submodule SHA / IDF version / sha256s + md5s) to the Forgejo release. CI
  currently pins `espressif/idf:v6.0.1`.

## Documentation

User-facing:

- [docs/QUICKSTART.md](docs/QUICKSTART.md) — first-boot walkthrough for end users
  ([nl](docs/QUICKSTART.nl.md) · [es](docs/QUICKSTART.es.md) · [de](docs/QUICKSTART.de.md))
- [docs/HANDBOOK.md](docs/HANDBOOK.md) — comprehensive user reference (every screen, every setting, every API endpoint)
- [docs/BUILD_FROM_SOURCE.md](docs/BUILD_FROM_SOURCE.md) — compile yourself on macOS / Linux / Windows
- Web flasher: https://web-flasher.btclock.dev/
- Home Assistant integration: https://github.com/dsbaars/homeassistant-btclock

Developer-facing:

- [docs/SETTINGS.md](docs/SETTINGS.md) — settings reference (every NVS key)
- [docs/WEBUI_MINING_POOL_FIELDS.md](docs/WEBUI_MINING_POOL_FIELDS.md) — mining-pool credential field guide for WebUI authors
- [docs/HARDCODED_AUDIT.md](docs/HARDCODED_AUDIT.md) — inventory of hardcoded values
- [docs/FEATURE_MATRIX.md](docs/FEATURE_MATRIX.md) — parity matrix vs. the Arduino btclock_v3 firmware
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — component-level architecture overview
- [tools/wasm/README.md](tools/wasm/README.md) — off-device screen renderer + doc-image generator
- [CLAUDE.md](CLAUDE.md) — agent-facing build/flash/conventions
