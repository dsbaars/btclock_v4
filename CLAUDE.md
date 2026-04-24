# Project Instructions for AI Agents

This is **btclock_v4** — an ESP-IDF v5.5 C++ firmware for BTClock. The
project was extracted from `btclock_v3_fci/idf_cpp_proto/` on YYYY-MM-DD;
see the old repo at https://git.btclock.dev/btclock/btclock_v3 for
Arduino-era history and the original port's day-1 commits.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

## Session Completion

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd dolt push
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
<!-- END BEADS INTEGRATION -->

## ESP-IDF environment (REQUIRED for every idf.py AND esptool invocation)

**You must source the IDF env in every shell that runs `idf.py` OR
`esptool`**, otherwise CMake picks up the wrong Python, `idf.py` is
missing, and `esptool` runs from a stale PATH. Agents keep missing
this — don't.

```bash
source /Users/padjuri/esp/v5.5.4/esp-idf/export.sh
```

The `Bash` tool in Claude Code does NOT persist shell state between
calls, so source it inline (prefix with `&&`) on every build / flash
command, e.g.:

```bash
source /Users/padjuri/esp/v5.5.4/esp-idf/export.sh && \
  idf.py -B build-rev-b -D POC_BOARD=REV_B -D SDKCONFIG=build-rev-b/sdkconfig build
```

## Build & Test

```bash
# ALWAYS source first:
source /Users/padjuri/esp/v5.5.4/esp-idf/export.sh

# Variants select via POC_BOARD; keep per-build SDKCONFIG so variants
# don't poison each other
idf.py -B build-rev-a -D POC_BOARD=REV_A -D SDKCONFIG=build-rev-a/sdkconfig build
idf.py -B build-rev-b -D POC_BOARD=REV_B -D SDKCONFIG=build-rev-b/sdkconfig build
idf.py -B build-v8    -D POC_BOARD=V8    -D SDKCONFIG=build-v8/sdkconfig    build

# Host tests (no IDF dep, plain cmake — DO NOT source the IDF env for these)
cmake -S test_host -B build-host && cmake --build build-host && ./build-host/btclock_host_tests
```

### Host-test toolchain gotcha

Local (macOS/libc++) is more permissive than CI (Ubuntu/libstdc++).
When you add a new `test_host/*.cpp` file, explicitly `#include` every
STL header whose types you use, even if a transitive include works
locally. Common pitfalls that silently pass on macOS and fail on CI:

- `<string>` — for `std::string` declared out-of-line
- `<initializer_list>` — for `for (auto x : {a, b, c})`
- `<cstring>` — for `std::memcpy` / `std::strlen`
- `<cstdint>` — for `std::int64_t`

If CI fails with `"X in namespace 'std' does not name a type"` or
`"deducing from brace-enclosed initializer list requires #include"`,
add the header.

## Flashing the device

**Prerequisite: source the IDF env first.** Without it, `esptool` runs
from a wrong Python and port-enumeration silently fails:

```bash
source /Users/padjuri/esp/v5.5.4/esp-idf/export.sh
```

Port is NOT stable across sessions — re-enumerate every time. The
board's MAC is the only reliable identifier:

```bash
source /Users/padjuri/esp/v5.5.4/esp-idf/export.sh && \
  for p in /dev/cu.usbmodem*; do
    esptool.py --port "$p" flash_id 2>&1 | grep -E 'MAC|flash_size|Chip is' | \
      sed "s|^|$p: |"
  done
```

Board identification:

| Variant | MAC                   | Flash | PSRAM | Notes |
|---------|-----------------------|-------|-------|-------|
| Rev A   | `dc:54:75:d6:00:fc`   | 4 MB  | 2 MB  | no BH1750, no frontlight |
| Rev B   | `98:88:e0:9d:55:30`   | 8 MB  | 2 MB  | BH1750, frontlight |
| V8      | `30:30:f9:3e:d3:9c`   | 16 MB | 8 MB  | 8 panels |

Flash firmware (uses the `flash_args` file the build produced):

```bash
source /Users/padjuri/esp/v5.5.4/esp-idf/export.sh && \
  cd build-rev-a && \
  esptool.py --chip esp32s3 --port <PORT> -b 460800 \
    --before default_reset --after hard_reset write_flash "@flash_args"
```

**Prefer USB serial flashing**, but OTA works as a fallback when the
USB-JTAG is contested by the running firmware.

If esptool reports "Failed to connect":
1. Add `--connect-attempts 5` and retry.
2. If still failing, hold the BOOT button, tap RESET, release BOOT —
   this forces the bootloader to claim USB-JTAG. Then retry esptool.
3. If there's no physical access to press BOOT+RESET, use OTA:
   ```bash
   curl -v -X POST -H "Content-Type: application/octet-stream" \
     --data-binary @build-rev-b/btclock_idf_proto.bin \
     http://<IP>/upload/firmware
   ```
   Device reboots onto the new partition; typical upload ~15 s for
   1.5 MiB. OTA respects `httpAuthEnabled` — include `-u user:pass`
   when auth is on. The OTA path was fixed in `2aafc59` (timeout
   retry, PSRAM buffer, sequential writes).

## LittleFS WebUI image

**The WebUI assets ship via a LittleFS partition, not via the app
binary.** Source layout that matters:

- `data/build_gz/www/` — compressed WebUI assets (`index.html.gz`,
  `bundle.js.gz`, `build/*`, etc.). This is what gets baked into the
  image.
- The webserver (`components/webserver/control_server.cpp`) serves
  from `kWebRootBase = "/lfs/www"`, so the files must land under
  `/www/` inside the LittleFS image — **pack from `data/build_gz/`
  (the parent), NOT `data/build_gz/www/`**.

Partition offsets per variant (from `partitions_*.csv`):

| Variant | Partition CSV         | Offset     | Size       |
|---------|-----------------------|------------|------------|
| REV_A   | `partitions_4mb.csv`  | `0x370000` | `0x67000`  |
| REV_B   | `partitions_8mb.csv`  | `0x6F0000` | `0xCD000`  |
| V8      | `partitions_16mb.csv` | `0xDF0000` | `0x200000` |

Rebuild the image after any WebUI change. Use the vendored
`tools/mklittlefs/mklittlefs` wrapper — it dispatches to the right
platform-specific binary under `tools/mklittlefs/mklittlefs-<os>-<arch>`
(pinned to earlephilhower/mklittlefs **4.1.0**, LittleFS **v2.11.1** —
matches `joltwallet__littlefs` 1.21.1). The PlatformIO-bundled
`~/.platformio/packages/tool-mklittlefs/mklittlefs` (commit `05d49dc`,
Sep 2023) ships an older LittleFS and **must not** be used. See
`tools/mklittlefs/README.md` for how to bump the pin.

If the vendored binary is missing on a fresh clone / new host, fetch it
on demand:

```bash
tools/mklittlefs/fetch.sh
```

Then pack:

```bash
MKLFS=tools/mklittlefs/mklittlefs

# Rev A
$MKLFS --create data/build_gz --size 0x67000 --block 4096 --page 256 \
  build-rev-a/storage.bin
# Rev B
$MKLFS --create data/build_gz --size 0xCD000 --block 4096 --page 256 \
  build-rev-b/storage.bin
# V8
$MKLFS --create data/build_gz --size 0x200000 --block 4096 --page 256 \
  build-v8/storage.bin
```

Then write it to the device at the per-variant offset:

```bash
# Example for Rev A at 0x370000
python -m esptool --chip esp32s3 --port <PORT> -b 460800 \
  write_flash 0x370000 build-rev-a/storage.bin
```

**Verify after flash**: `curl -I http://<IP>/` → HTTP 200. If it's 404,
the image was packed from the wrong directory (`.../www/` instead of
`data/build_gz/`) and the files landed at `/*` instead of `/www/*`.

The firmware image alone is enough for code-only changes; re-flash the
LittleFS partition **only** when `data/build_gz/www/` content changed
or on a fresh board.

## Architecture Overview

ESP32-S3, three hardware variants (Rev A / Rev B / V8), shared IDF code
with per-variant sdkconfig fragments. See docs/FEATURE_MATRIX.md for the
full parity matrix against the Arduino btclock_v3 firmware (links there
are pinned at old-repo SHA eac3a28).

## Conventions & Patterns

- WHY-only comments; avoid WHAT comments (identifiers already tell what).
- Renderer screen code under main/screens/; pure-logic helpers under
  main/screens/ or components/<area>/ so they're host-testable.
- Every HTTP endpoint goes in components/webserver/control_server.cpp with
  a trampoline-to-method pattern; max_uri_handlers budget lives there too.
