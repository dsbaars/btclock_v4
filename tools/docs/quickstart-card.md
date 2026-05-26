---
title: "BTClock — Quickstart"
---

**You'll need:**

- The BTClock.
- A USB-C cable and a 5 V power supply
  (Rev B with frontlight: **≥ 1 A**; Rev A / V8: 500 mA is plenty).
- A 2.4 GHz Wi-Fi network (the ESP32-S3 radio has no 5 GHz support).
- A phone or laptop on the same network for the WebUI.

```{=latex}
\begin{multicols}{2}
```

# Connect to your Wi-Fi

1.  **Power up.** USB-C in. LEDs rainbow-test, panels paint the
    **provisioning screen** with the hotspot name `BTClock-XXXX`, an
    8-char WPA2 password, and a Wi-Fi-config QR. The password is
    persisted to NVS — note it or scan the QR.
2.  **Join the BTClock's hotspot.** Scan the QR (auto-joins), or
    connect manually to `BTClock-XXXX` with the password shown. If the
    captive portal doesn't pop, open `http://192.168.4.1`.
3.  **Pick your home Wi-Fi.** Select your SSID in the portal, type
    the password, Save. Device reboots and rejoins as a normal STA;
    the hotspot disappears.
4.  **Find the BTClock.** mDNS: `http://btclock-xxxxxx.local`. On
    the device: press **button 4** for the **debug overlay** — the
    first cell shows the IP. Or look up `btclock-xxxxxx` in your
    router's DHCP table.

![First-boot provisioning screen — scan the QR to join the BTClock's hotspot.](img/screens/provisioning_first_boot.png)

```{=latex}
\columnbreak
```

# Setup via WebUI

1.  **Open the WebUI.** In a browser on the same network, go to
    `http://btclock-xxxxxx.local` (or the IP from step 4). Three
    columns:
    - **Control** — text, LED, OTA, factory reset.
    - **Status** — live panel preview, timer, DND, signal, uptime.
    - **Settings** — the long form.
2.  **Timezone & currencies.**
    - Settings → Timezone (IANA zone, DST auto-tracked).
    - Settings → Data sources → Currencies (drag-reorder; defaults
      USD, EUR, JPY).
3.  **Screens.** Settings → Screens (toggle + reorder). Settings →
    Screen specific → **Time per screen** sets auto-rotate (default
    30 min).
4.  **First-day tweaks.**
    - **LED colour** — Control → LEDs (default BTClock orange).
    - **Frontlight** (Rev B only) — Control → Frontlight: on /
      ambient / off.
    - **Mining pool** — Settings → Mining pool (defaults to
      `noderunners` global stats).
    - **Bitaxe** — Settings → Bitaxe (hostname or IP).
    - **Do-Not-Disturb** — Settings → DnD (LEDs-dark window).

```{=latex}
\end{multicols}
```

# Quick links *— trouble scanning? Cover the other three with your hand.*

|  ![Web flasher QR](img/qr/web-flasher-v4.png) | ![Handbook QR](img/qr/handbook.png) | ![Home Assistant QR](img/qr/homeassistant-github.png) | ![Telegram support QR](img/qr/telegram-support.png) |
|:---:|:---:|:---:|:---:|
| **Web flasher (v4)** | **Full handbook** | **Home Assistant** | **Telegram support** |
