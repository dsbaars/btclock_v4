# Home Assistant integration

A custom Home Assistant integration is maintained alongside the
firmware (same author) and published at
[git.btclock.dev/btclock/homeassistant-btclock](https://git.btclock.dev/btclock/homeassistant-btclock)
with a mirror on
[github.com/dsbaars/homeassistant-btclock](https://github.com/dsbaars/homeassistant-btclock).
Install via HACS ("Custom repositories" → add the repo URL → integration),
restart Home Assistant, then add the BTClock through **Settings → Devices
& Services → Add integration → BTClock**. Discovery is automatic when both
the BTClock and the HA host are on the same multicast domain — the device
appears under "Discovered" within ~5 s of boot via the `_http._tcp.local.`
mDNS service with the `btclock-*` instance name.

The integration auto-detects which firmware generation it's talking to
(legacy ≤3.3, v3.4, v4) and surfaces the appropriate entities. v4-only
fields (mining-pool selector, font selector, Bitaxe data source, pool
poll cadences, the `simulate_zap` / `clear_pool_logos` /
`restart_datasources` diagnostic actions) are gated on either the
detected firmware variant or the presence of the backing setting in
`/api/settings`, so they appear only on v4 devices.

Two update modes ship: **Server-Sent Events** (push, default — opens a
persistent connection to `/events` and updates entities as the device
broadcasts) and **Polling** (configurable interval, for environments
where long-lived HTTP isn't viable). Pick during the config flow; switch
later via the integration's Configure dialog.

Authentication piggy-backs on `httpAuthEnabled` — when the device has a
WebUI password set, the config flow asks for the credentials and stores
them in the entry.

![Integration overview](img/homeassistant/integration_overview.png)

> All screenshots in this document are reproducible. The
> [`scripts/screenshot`](https://git.btclock.dev/btclock/homeassistant-btclock/src/branch/main/scripts/screenshot)
> tool in the integration repo boots a temporary Home Assistant against
> a stub BTClock seeded from the v4 fixture, captures the device-card
> sections, and crops them.

## Device card layout

Once added, the BTClock surfaces as a single device with all entities
grouped into Controls / Sensors / Configuration / Diagnostic. The
screenshots below are taken from a Rev B device running v4 firmware
with the BH1750 light sensor and PCA9685 frontlight populated.

### Device info

Hardware revision, firmware version (`gitRev` from `git describe`), and
a deep-link to the device's WebUI. The "Visit" button opens
`http://<host>/` in a new tab — handy for the few features the
integration doesn't surface (firmware OTA staging, screen rotation
order).

![Device info](img/homeassistant/section_device_info.png)

### Controls

Day-to-day toggles, selectors, and momentary buttons. Currency picker
backs `actCurrencies`; Display font and Mining pool back `availableFonts`
/ `availablePools` (v4 only). LEDs 1-4 expose the per-pixel NeoPixel
state with RGB pickers. The DND time fields write back to
`/api/settings.dnd.{startHour,startMinute,endHour,endMinute}`.

![Controls](img/homeassistant/section_controls.png)

### Sensors

Read-only state. Connection-status sensors (`Price feed`, `Blocks feed`,
`V2 relay`) reflect the live `connectionStatus` block from
`/api/status`; `Ambient light level` only appears when `hasLightLevel`
is true (Rev B with a populated BH1750). `Nostr relay` exposes the
configured upstream as both state (the URL) and a `connected` attribute
so a single template can pick either.

![Sensors](img/homeassistant/section_sensors.png)

### Configuration

Settings-card entities and lifecycle actions. **LED brightness** writes
the global brightness multiplier; **Firmware** is HA's standard Update
entity, which polls `gitReleaseUrl` once a day and surfaces a one-click
install (or specific-version install for downgrades). **Clear cached
pool logos** is v4-only — fires `/api/action/clear_pool_logos` to wipe
the runtime logo cache so the next render re-fetches from
`poolLogosUrl`.

![Configuration](img/homeassistant/section_configuration.png)

### Diagnostic

Health metrics, identify, and the v4-only data-source actions.
**Restart data sources** fires `/api/restart_datasources` to reconnect
upstream feeds without rebooting the ESP. **Simulate Nostr Zap** fires
`/api/action/simulate_zap` — useful for triggering the LED + screen
overlay end-to-end without waiting for a real zap. **OTA state**
mirrors `isOTAUpdating` so automations can suppress noisy notifications
during a flash.

![Diagnostic](img/homeassistant/section_diagnostic.png)

## Firmware updates

The integration polls the device's `gitReleaseUrl` once a day and
surfaces any newer release as a standard Home Assistant **Update**
entity. When an update is available, the device's Configuration card
shows a `Firmware — Update available` row; clicking it opens the
familiar more-info dialog with the installed and latest versions side
by side, the release notes (or a synthesized changelog from a Forgejo
`compare/...` API call when the release `body` is empty), and an
**Install** button that POSTs to `/api/firmware/auto_update` to kick
the device's own OTA downloader.

![Firmware update dialog](img/homeassistant/update_dialog.png)

For downgrades or pinning to a specific version, the dialog's
overflow menu offers a version picker; selecting an older release
streams the matching `*_firmware.bin` and `littlefs_<size>.bin` assets
from Forgejo through Home Assistant's session and uploads them to
`/upload/firmware` and `/upload/webui` directly. After either install
path, the integration polls `/api/settings` once a minute for up to
20 minutes to detect the post-OTA reboot, clears the in-progress
indicator the moment the device reports a new version, and reloads the
config entry so the entity set matches the new firmware variant
(legacy ↔ v3.4 ↔ v4).

Dev / dirty builds (`gitTag` empty, or `gitRev` matching `-dirty` or
the `git describe` past-tag form `4.0.0-3-gabc1234`) are skipped — the
Update entity isn't created, so an actively-developed device doesn't
surface bogus "newer" releases. Tagged prerelease builds
(`4.0.0-beta.1`) are eligible.

## Services

Two domain-level services, callable on both v3.4 and v4:

- **`btclock.show_text`** — display arbitrary text across the panels,
  one character per panel. Truncated to `numScreens`, auto-uppercased.
  Equivalent to `POST /api/show/text?t=…`.
- **`btclock.show_custom`** — display one string per panel (array body).
  Equivalent to `POST /api/show/custom`.

Plus every `button` entity is callable as `button.press` from
automations.

## Authentication & discovery

When `httpAuthEnabled` is set on the device, the config flow picks up
the 401 from `/api/settings` on first contact and re-prompts for
credentials. Stored credentials are sent as HTTP Basic Auth on every
request; rotation is handled by HA's built-in re-auth flow.

mDNS auto-discovery uses the `_http._tcp.local.` service with an
instance name matching `btclock-*`. The config flow verifies any match
by hitting `/api/settings` before completing, so unrelated HTTP
advertisers on the same network don't yield false positives.
