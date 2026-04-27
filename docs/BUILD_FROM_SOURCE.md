# BTClock — Build from source

Three platforms are covered here: **macOS**, **Linux**, and **Windows**.
On each, you'll install Espressif's ESP-IDF v6.0.1 toolchain, fetch the
btclock_v4 repository (with the WebUI submodule), build firmware for
your variant, optionally rebuild the WebUI bundle, and flash the
result over USB or OTA.

> If you don't need a custom build, the **web flasher** at
> https://web-flasher.btclock.dev/ flashes any tagged release in one
> click — no toolchain. Use this guide when you want to develop, debug,
> or run an unreleased branch.

---

## Table of contents

- [1. Prerequisites](#1-prerequisites)
- [2. Install ESP-IDF on macOS](#2-install-esp-idf-on-macos)
- [3. Install ESP-IDF on Linux](#3-install-esp-idf-on-linux)
- [4. Install ESP-IDF on Windows](#4-install-esp-idf-on-windows)
- [5. Clone the repository](#5-clone-the-repository)
- [6. Build firmware](#6-build-firmware)
- [7. Build the WebUI bundle](#7-build-the-webui-bundle)
- [8. Pack the LittleFS image](#8-pack-the-littlefs-image)
- [9. Flash via USB](#9-flash-via-usb)
- [10. Flash via OTA](#10-flash-via-ota)
- [11. Run host tests](#11-run-host-tests)
- [12. Troubleshooting](#12-troubleshooting)

---

## 1. Prerequisites

Common across all platforms:

- **Git** (for the repo + submodule).
- **Python 3.9 or newer** — ESP-IDF needs it for its build system.
- **A USB-C cable** that exposes data lines (most phone cables work,
  but some power-only cables don't).
- **Disk space** — ~2 GB for ESP-IDF, ~1 GB for build artifacts.

For the WebUI rebuild step (optional — pre-built bundles ship with
each release):

- **Node.js 20 or newer**.
- **pnpm** (`npm i -g pnpm`).

For the WASM screen previewer (optional — `tools/wasm/preview.html`):

- **Emscripten 3.1.0+** (`brew install emscripten` or `emsdk`).

## 2. Install ESP-IDF on macOS

```bash
# Tools that don't ship with macOS by default.
brew install cmake ninja dfu-util ccache

# Clone ESP-IDF v6.0.1 into ~/esp/v6.0/. Tag v6.0.1 (not v6.0-rc1,
# v6.0-beta2, etc.) is the supported release. Folder name stays at
# `v6.0` — tracking the major.minor in the path keeps it stable as
# patch releases land.
mkdir -p ~/esp/v6.0
git clone -b v6.0.1 --recurse-submodules \
  https://github.com/espressif/esp-idf.git ~/esp/v6.0/esp-idf

# Install the toolchain + Python deps. Picks Xtensa LX7 + RISC-V toolchains.
~/esp/v6.0/esp-idf/install.sh esp32s3
```

**Every shell that builds or flashes** must source the export script
first — sourced state does not persist across terminal sessions:

```bash
source ~/esp/v6.0/esp-idf/export.sh
```

You should see `Detecting the Python interpreter ... Done`. Now `idf.py`,
`esptool.py`, and the toolchain are on your PATH for that shell only.

Apple Silicon: install.sh detects `arm64` automatically — no Rosetta
needed. Intel macOS works the same way.

## 3. Install ESP-IDF on Linux

Tested on Ubuntu 22.04+ / Debian 12+. Other distros are similar; map
the package names to your distro's equivalents (apt → dnf / pacman /
zypper).

```bash
sudo apt install -y git wget flex bison gperf python3 python3-pip \
                    python3-venv cmake ninja-build ccache libffi-dev \
                    libssl-dev dfu-util libusb-1.0-0

mkdir -p ~/esp/v6.0
git clone -b v6.0.1 --recurse-submodules \
  https://github.com/espressif/esp-idf.git ~/esp/v6.0/esp-idf

~/esp/v6.0/esp-idf/install.sh esp32s3
```

Source the export script in every shell:

```bash
. ~/esp/v6.0/esp-idf/export.sh
```

USB serial access on Linux requires either root or membership in the
`dialout` group:

```bash
sudo usermod -aG dialout $USER
# log out and back in for the group change to take effect
```

If your distro uses `uucp` or another group, swap that in instead.

## 4. Install ESP-IDF on Windows

Two paths — pick one. The native installer is simpler; WSL2 gets you
the same setup as Linux (and is what CI uses).

### 4a. Native ESP-IDF Windows installer (recommended for Windows-only)

1. Download the **ESP-IDF v6.0.1 installer** for Windows from
   https://dl.espressif.com/dl/esp-idf/.
2. Run it; pick the v6.0.1 release and the install path
   `C:\Espressif\frameworks\esp-idf-v6.0`.
3. The installer creates a Start-menu entry **"ESP-IDF v6.0 PowerShell"**.
   Open that — it's a PowerShell session with the IDF env pre-sourced.
4. Every build/flash command in this guide must run from that
   pre-sourced PowerShell window. A plain PowerShell or cmd.exe does
   not have the toolchain on PATH.

USB serial: Windows enumerates the BTClock as `COM<n>`. Check Device
Manager → Ports for the number (e.g. `COM3`). The Windows USB-CDC
driver works out of the box — no Zadig swap needed.

### 4b. WSL2 (recommended for Linux-style workflow)

1. Install WSL2 + an Ubuntu 22.04 distro from the Microsoft Store.
2. Inside WSL, follow the [Linux instructions](#3-install-esp-idf-on-linux)
   exactly.
3. USB serial under WSL2 needs the
   [usbipd-win](https://github.com/dorssel/usbipd-win) bridge. Install
   it on the Windows host, then attach the BTClock to WSL with:

   ```powershell
   usbipd list
   usbipd bind --busid <id>
   usbipd attach --wsl --busid <id>
   ```

   Inside WSL, the device appears as `/dev/ttyACM0` or similar.

## 5. Clone the repository

The WebUI lives in a Git submodule under `data/`. Clone with
`--recurse-submodules` to fetch it in one go:

```bash
git clone --recurse-submodules https://git.btclock.dev/btclock/btclock_v4.git
cd btclock_v4
```

Already cloned without submodules? Update them now:

```bash
git submodule update --init --recursive
```

## 6. Build firmware

`BTCLOCK_BOARD` picks the pin map (`REV_A`, `REV_B`, `V8`).
`BTCLOCK_PANEL` picks the EPD geometry (`2_13`, `2_9`, `7_5`). Each
variant uses its own `sdkconfig` file so they don't poison each other.

Source the IDF env first (every shell — it's not persistent):

```bash
source ~/esp/v6.0/esp-idf/export.sh
```

Then pick your build:

```bash
# Rev A, 2.13" (production):
idf.py -B build-rev-a    -D BTCLOCK_BOARD=REV_A -D BTCLOCK_PANEL=2_13 \
       -D SDKCONFIG=build-rev-a/sdkconfig    build

# Rev B, 2.13" (production):
idf.py -B build-rev-b    -D BTCLOCK_BOARD=REV_B -D BTCLOCK_PANEL=2_13 \
       -D SDKCONFIG=build-rev-b/sdkconfig    build

# Rev A, 2.9" (un-supported but builds clean):
idf.py -B build-rev-a-29 -D BTCLOCK_BOARD=REV_A -D BTCLOCK_PANEL=2_9  \
       -D SDKCONFIG=build-rev-a-29/sdkconfig build

# V8, 8 panels (prototype):
idf.py -B build-v8       -D BTCLOCK_BOARD=V8    -D BTCLOCK_PANEL=2_13 \
       -D SDKCONFIG=build-v8/sdkconfig       build
```

Outputs land in the matching `build-*/` directory. The two artifacts
that matter are:

- `build-<variant>/btclock_v4.bin` — the firmware app image.
- `build-<variant>/storage.bin` — the LittleFS WebUI image (rebuilt by
  step 8 below; not produced by `idf.py build`).

The first build pulls + builds the IDF managed components — expect 5–
10 minutes on a fast machine. Subsequent builds are ~30 s thanks to
ccache.

## 7. Build the WebUI bundle

Skip this step if you're happy with the WebUI version pinned in the
`data/` submodule. Otherwise:

```bash
cd data
pnpm install
pnpm build:test           # builds into data/dist/
# Optional:
# pnpm build               # production bundle
cd ..
```

The firmware serves WebUI assets from `data/build_gz/www/` (gzipped
production bundle). To regenerate that from the freshly-built `dist/`:

```bash
python3 data/gzip_build.py
```

This runs the same compress + bundle step CI uses.

## 8. Pack the LittleFS image

The WebUI ships as a separate LittleFS partition image flashed at a
per-variant offset:

```bash
MKLFS=tools/mklittlefs/mklittlefs

# Rev A (4 MB flash):
$MKLFS --create data/build_gz --size 0x67000  --block 4096 --page 256 \
       build-rev-a/storage.bin

# Rev B (8 MB flash):
$MKLFS --create data/build_gz --size 0xCD000  --block 4096 --page 256 \
       build-rev-b/storage.bin

# V8 (16 MB flash):
$MKLFS --create data/build_gz --size 0x200000 --block 4096 --page 256 \
       build-v8/storage.bin
```

If `tools/mklittlefs/mklittlefs` is missing on a fresh clone (the
binary is platform-specific and only one flavour ships), run the
fetch helper:

```bash
tools/mklittlefs/fetch.sh
```

It pulls the right pre-built `mklittlefs` 4.1.0 binary for your host
OS/arch.

## 9. Flash via USB

Identify the port:

- **macOS**: `/dev/cu.usbmodem*` or `/dev/cu.usbserial-*`.
- **Linux**: `/dev/ttyUSB0` or `/dev/ttyACM0`.
- **Windows**: `COM3` (or whatever Device Manager shows).

Ports are not stable across boots — re-check each session. On macOS
you can match by MAC:

```bash
for p in /dev/cu.usbmodem* /dev/cu.usbserial*; do
  [ -e "$p" ] || continue
  esptool.py --port "$p" flash_id 2>&1 | grep -E 'MAC|Chip is|flash_size' \
    | sed "s|^|$p: |"
done
```

Flash the firmware (uses the build's `flash_args` file, which lists
all the partitions to write):

```bash
cd build-rev-a   # or build-rev-b / build-v8
esptool.py --chip esp32s3 --port <PORT> -b 460800 \
  --before default_reset --after hard_reset write_flash "@flash_args"
```

Flash the WebUI image at the per-variant offset:

| Variant | Offset |
|---|---|
| Rev A | `0x370000` |
| Rev B | `0x6F0000` |
| V8 | `0xDF0000` |

```bash
# Example for Rev A:
python -m esptool --chip esp32s3 --port <PORT> -b 460800 \
  write_flash 0x370000 build-rev-a/storage.bin
```

If you only changed firmware code (not WebUI assets), re-flashing the
LittleFS image is unnecessary on subsequent builds.

## 10. Flash via OTA

Once the device is on Wi-Fi, the OTA path needs no USB cable:

```bash
# Firmware:
curl -X POST -H 'Content-Type: application/octet-stream' \
  --data-binary @build-rev-b/btclock_v4.bin \
  http://btclock-xxxxxx.local/upload/firmware

# WebUI:
curl -X POST -H 'Content-Type: application/octet-stream' \
  --data-binary @build-rev-b/storage.bin \
  http://btclock-xxxxxx.local/upload/webui
```

When `httpAuthEnabled=true`, add `-u user:pass`. When `otaPass` is set,
the OTA path takes that password instead of `httpAuthPass` — same
syntax.

The device reboots automatically after a successful firmware OTA;
WebUI OTA reboots too so the new bundle gets picked up by LittleFS.

## 11. Run host tests

A subset of the codebase (rendering, fee-rate parsing, panel-text
formatting, settings PATCH validation, partition-table sanity) builds
on the host with stock CMake. The IDF env should NOT be sourced for
these — they use the system toolchain:

```bash
cmake -S test_host -B build-host
cmake --build build-host
./build-host/btclock_host_tests
```

CI runs the same suite plus an ASan + UBSan variant; see the README
for the sanitizer build instructions.

## 12. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `idf.py: command not found` | IDF env not sourced in this shell | `source ~/esp/v6.0/esp-idf/export.sh` |
| `ImportError: No module named 'click'` | Wrong Python being picked up by CMake | Source the IDF env first; that puts the IDF Python venv on PATH |
| `esptool.py ... Failed to connect` | USB-JTAG contended by running firmware | Hold BOOT, tap RESET, release BOOT, retry. Or add `--connect-attempts 5`. |
| Build fails on a fresh clone with "managed component … not found" | `dependencies.lock` references a managed component that wasn't fetched | `idf.py reconfigure` once will fetch them |
| LittleFS pack fails on Windows | Vendored `mklittlefs.exe` missing | `tools/mklittlefs/fetch.sh` (run from Git Bash or WSL) |
| WebUI version warning after OTA | Firmware OTA succeeded, WebUI OTA didn't run | Flash `build-<variant>/storage.bin` at the per-variant offset to bring them back into sync |
| `clang-format` complaints in CI | Local LLVM differs from CI's LLVM 22 | `brew install llvm` or run `tools/lint/format.sh` from inside CI's Docker image |
| `Failed to load WASM module` on `tools/wasm/preview.html` | Bundle not built, or opened via `file://` | `tools/wasm/build.sh` then `python3 -m http.server 8000 --directory tools/wasm` |

If you hit something not on the list, the in-tree feature-parity
matrix at
[`docs/FEATURE_MATRIX.md`](https://git.btclock.dev/btclock/btclock_v4/src/branch/main/docs/FEATURE_MATRIX.md)
has end-to-end pointers to every subsystem's source files — most "why
isn't X working" questions are one grep away from an answer.

## 13. Documentation site

The user-facing documentation site (the one published at
[docs.btclock.dev](https://docs.btclock.dev)) is built with
[MkDocs Material](https://squidfunk.github.io/mkdocs-material/) from
the same Markdown files in `/docs/` that this guide lives in. The
config is at the repo root (`mkdocs.yml`), pinned dependencies are in
`mkdocs-requirements.txt`, and the local-preview workflow is:

```bash
python3 -m venv .venv-docs
source .venv-docs/bin/activate
pip install -r mkdocs-requirements.txt
mkdocs serve          # http://127.0.0.1:8000
# …or, build the static site into ./site/
mkdocs build
```

Or use the `make` shortcuts (see [`Makefile`](../Makefile) at the
repo root): `make docs-deps`, `make docs-serve`, `make docs-build`.

### Documentation translations

Pages translate via the **filename suffix** convention used by the
[`mkdocs-static-i18n`](https://github.com/ultrabug/mkdocs-static-i18n)
plugin — the same convention `QUICKSTART.{de,es,nl}.md` already uses:

- `HANDBOOK.md` — the canonical English source.
- `HANDBOOK.nl.md` — Dutch translation (would be served at
  `/nl/handbook/`). Drop the file in `/docs/` next to its English
  sibling and the i18n plugin picks it up on the next build.
- Pages **without** a translation automatically fall back to the
  English version under `/<lang>/`. So creating an empty `nl/` tree
  is unnecessary — only translate the pages you actually have words
  for.

Languages currently configured: English (default), Nederlands, Deutsch,
Español. Adding a fifth (e.g. Français) means a new entry under
`plugins.i18n.languages` in `mkdocs.yml` plus a matching entry in the
Material theme's `extra.alternate` block — no other config wiring.

Section labels in the side navigation translate via the
`nav_translations` map under each locale in `mkdocs.yml`. New top-level
nav entries need an entry there for every non-English locale; missing
entries fall back to the English label, which is benign but reads
oddly in mixed-language navs.

### Printable booklet (PDF)

The same Markdown source can be rendered as a stapled-booklet PDF for
offline reading or print. Pandoc + xelatex do the conversion; the
script and per-language editions live under
[`tools/docs/`](../tools/docs/README.md):

```bash
tools/docs/make_booklet.sh        # English — docs/build/btclock-booklet.pdf
tools/docs/make_booklet.sh nl     # Dutch quickstart edition
tools/docs/make_booklet.sh de     # German
tools/docs/make_booklet.sh es     # Spanish
```

Outputs are A5 by default (148 × 210 mm). When `pdfjam` is installed,
an A4-landscape `…-impose.pdf` is also produced — print double-sided,
fold, and staple for a true pocket booklet.

Toolchain install on macOS:

```bash
brew install pandoc poppler
brew install --cask basictex font-inter
sudo tlmgr install pdfjam fancyhdr titlesec xcolor xurl newunicodechar
```

See [`tools/docs/README.md`](../tools/docs/README.md) for the full
toolchain table and design notes.
