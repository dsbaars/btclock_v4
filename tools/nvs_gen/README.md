# NVS pre-seed images

Generates a 20 KiB NVS partition image (`0x5000` at offset `0x9000`) holding
two settings the user picks at flash time:

| Pref            | Effect                                          |
|-----------------|-------------------------------------------------|
| `flDisable`     | `0` = frontlight enabled, `1` = disabled        |
| `invertedColor` | `1` = white on black, `0` = black on white      |
| `fgColor`/`bgColor` | derived from `invertedColor` (kept in sync) |

`fgColor`/`bgColor` are written alongside `invertedColor` so a fresh boot
matches what `/api/settings { invertedColor: ... }` would persist (see
`components/settings/settings_api.cpp` ~line 499).

## Why u32 for booleans

`Prefs::SetBool` delegates to `nvs_set_u32` (see
`components/prefs/prefs.cpp`), so the pre-seed must use `u32` for any
boolean key — a `u8` would read back as the default at runtime.

## Build

```bash
source $HOME/esp/v5.5.4/esp-idf/export.sh
./tools/nvs_gen/build.sh                          # default: tools/nvs_gen/out/
./tools/nvs_gen/build.sh --outdir ../web-flasher-ng/public/firmware_v4/nvs
```

The script iterates every `variants/*.csv` and produces a matching `.bin`.
Variant filenames follow the convention used by web-flasher-ng's
`useManifest`:

```
${frontlight ? "frontlight_" : ""}${displayColors}.bin
```

## Adding a new variant

Drop a new CSV under `variants/`. Same format as ESP-IDF's
`nvs_partition_gen.py`:

```
key,type,encoding,value
settings,namespace,,
<key>,data,<u32|str|...>,<value>
```

Keys must match `components/settings/include/settings/pref_keys.hpp`
exactly (NVS caps key length at 15 chars). Encodings must match what
the runtime reader expects — for keys handled via `Prefs::GetBool` /
`Prefs::SetBool` that means `u32`, not `u8`.

## Wiring on the device

NVS lives at offset `0x9000` size `0x5000` on every variant
(`partitions_4mb.csv` / `partitions_8mb.csv` / `partitions_16mb.csv`),
so one image works on Rev A, Rev B and V8. The web flasher appends the
chosen `.bin` to its manifest at offset `0x9000` only when the user
opts into "Customize settings" — leaving the toggle off preserves any
existing NVS (WiFi creds included).
