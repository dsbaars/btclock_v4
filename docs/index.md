# BTClock

A Bitcoin block-height-and-price companion device. Open hardware, open
firmware, open data feeds. This site is the **user-facing**
documentation for the BTClock v4 firmware (ESP-IDF v6.0); developer
references live alongside the code in the [Forgejo repository][repo].

[repo]: https://git.btclock.dev/btclock/btclock_v4

![Block height screen](img/screens/block_height.png)

## What you'll find here

- **[Quickstart](QUICKSTART.md)** — get a powered-up BTClock onto
  Wi-Fi and rotating Bitcoin screens in about ten minutes. Covers
  Wi-Fi join, timezone, currency rotation, and the four-button
  on-device controls.
- **[Handbook](HANDBOOK.md)** — every WebUI section, every screen, and
  every toggle, with side-by-side renders showing the impact of each
  setting.
- **[Settings reference](SETTINGS.md)** — exhaustive table of every
  persisted preference, its type, default, validation range, and which
  endpoint accepts it.
- **[Architecture](ARCHITECTURE.md)** — task / data-flow / IPC overview
  for contributors who want to add a screen, port to new hardware, or
  modify a renderer.
- **[Build from source](BUILD_FROM_SOURCE.md)** — flashing, OTA,
  toolchain setup, and per-board sdkconfig fragments.
- **[Project story](STORY.md)** — how the BTClock came to be, told by
  the project's original developer.

## Other languages

The Quickstart is translated into **Nederlands**, **Deutsch**, and
**Español**. On the live site, the language picker in the top-right
switches the active locale. Pages without a translation fall back to
the English version.

## Hardware

The v4 firmware ships for three production boards:

| Variant | MCU            | Flash | PSRAM | Default panel | Notes |
|---------|----------------|-------|-------|---------------|-------|
| Rev A   | ESP32-S3       | 4 MB  | 2 MB  | 2.13"         | no frontlight, no BH1750 (Lolin S3 Mini) |
| Rev B   | ESP32-S3       | 8 MB  | 2 MB  | 2.13"         | BH1750 ambient sensor, frontlight |
| V8      | ESP32-S3       | 16 MB | 8 MB  | 2.13"         | 8 panels |

The 2.9" GDEY029T94 panel is supported on every board via
`BTCLOCK_PANEL=2_9`; the 7.5" GDEY075T7 is scaffolded but not yet
brought up.

## Source layout

The repo top-level groups responsibilities cleanly:

- `main/` — the firmware app: screens, screen-manager, button handlers,
  OTA, data ingest, provisioning UI.
- `components/` — reusable subsystems (fonts, EPD drivers, MCP23017,
  PCA9685, mining-pool clients, …).
- `data/` — the Svelte WebUI shipped on the device's LittleFS.
- `tools/` — host-side tooling (WASM doc renderer, mklittlefs wrapper,
  pool-logo converter, …).
- `docs/` — what you're reading.

## Contributing

Issues and patches: file via [Forgejo issues][issues]. Translation PRs
are very welcome; the structure is documented in
[Build from source](BUILD_FROM_SOURCE.md#documentation-translations).

[issues]: https://git.btclock.dev/btclock/btclock_v4/issues
