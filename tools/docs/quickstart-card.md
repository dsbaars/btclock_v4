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

```{=latex}
\clearpage
```

# Updating the firmware

Two no-toolchain ways to move to a newer release once the device is up.

```{=latex}
\begin{multicols}{2}
```

## Via the WebUI (over Wi-Fi)

The **Firmware update** card at the bottom of the **Control** column
shows the latest release and offers two paths:

- **Auto-update** — if a newer version is offered, click **Install
  update (experimental)** and the clock fetches and flashes it itself.
  It's experimental: if it fails, retry; if it still won't take, use the
  web flasher (right).
- **Manual upload** — download the release for your board, then
  **Firmware file** → **Update firmware** (and **WebUI file** → **Update
  WebUI**). Each field's label shows the exact filename for your board
  (e.g. `btclock_rev_b_ota.bin`); a mismatched file is rejected.

A progress overlay shows on the panels; the clock reboots into the new
version once the checksum verifies.

```{=latex}
\columnbreak
```

## Via the web flasher (over USB)

Use it whenever you like — for a first flash, if a WebUI update won't go
through, or to recover a board whose WebUI is unreachable. It talks to
the device over USB, so it needs no network.

1.  Connect the BTClock to your computer with a USB-C cable. **On Rev B
    the USB-C port is on the back; on Rev A it's on the side.**
2.  Open `web-flasher-v4.btclock.dev` in **Chrome, Edge, or Brave**
    (WebSerial is required — Firefox and Safari won't work).
3.  Click **Connect** and pick the latest release — the flasher
    auto-detects your board variant (Rev A / Rev B / V8) and installs
    firmware + WebUI in one shot.

```{=latex}
\end{multicols}
```

```{=latex}
\begin{multicols}{2}
```

# On-device buttons

Numbered left-to-right looking at the back; actions fire on release.

- **Button 1** — pause / resume rotation · *hold:* toggle Do Not Disturb.
- **Button 2** — next screen.
- **Button 3** — previous screen.
- **Button 4** — toggle the debug overlay (its first cell shows the IP).

**Wi-Fi reset:** hold **button 1 while powering on** until the LED ring
turns red, then keep holding ~3 s. Wipes only the Wi-Fi credentials and
reboots into the setup hotspot — every other setting is kept.

```{=latex}
\columnbreak
```

# Integrations at a glance

- **Nostr zaps** — the LEDs flash on every zap to your npub.
- **Lightning wallet** — pair one over Nostr Wallet Connect (NWC) for
  balance + payment notifications.
- **Mining pool** — many pools supported; defaults to `noderunners`
  global stats.
- **Bitaxe** — point at your miner's host/IP for its hashrate and best
  difficulty.
- **Home Assistant** — a dedicated Home Assistant integration adds
  entities and services for the clock (handbook QR on page 1).

```{=latex}
\end{multicols}
```

# Troubleshooting

```{=latex}
\begin{multicols}{2}
```

## The setup hotspot doesn't appear

- It runs only *before* Wi-Fi is configured and vanishes once the device
  joins your network. Already set up? Do the **Wi-Fi reset** above.
- After a wrong password the device retries for `wpTimeout` (15 min)
  before falling back to the hotspot — wait it out, or boot-hold
  button 1.
- The radio is **2.4 GHz only**; a 5 GHz-only phone hotspot won't be
  offered.

```{=latex}
\columnbreak
```

## You can't reach the WebUI

- Press **button 4** for the debug overlay — its first cell shows the
  **IP**; open `http://<that-ip>` directly.
- `btclock-xxxxxx.local` (mDNS) is flaky on some routers and phones;
  prefer the IP from the overlay or your router's DHCP list.
- Make sure your phone or laptop is on the **same** network — not a
  guest VLAN, and not isolated on the 5 GHz band.
- Turn off **Wi-Fi client isolation** (a.k.a. AP/client isolation) — it
  stops devices on the same network from reaching each other, the
  BTClock included.

```{=latex}
\end{multicols}
```
