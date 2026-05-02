# BTClock — Snelstart

Een walkthrough van tien minuten waarmee je een ingeschakelde BTClock
op je wifi zet, op je tijdzone instelt, en door de Bitcoin-schermen
laat roteren.

Wil je de diepe duik door alle instellingen? Lees dan
[`HANDBOOK.md`](HANDBOOK.md) zodra het apparaat op het netwerk staat.

Deze gids richt zich op de twee productievarianten — **Rev A** (4 MB,
2,13"-panelen) en **Rev B** (8 MB, 2,13"-panelen, met frontlight +
omgevingssensor). De V8 met 8 panelen en de 2,9" Rev A vallen buiten
de scope; behandel die als prototypes.

## Wat je nodig hebt

- De BTClock zelf.
- Een USB-C-kabel en een 5 V-voeding. De BTClock trekt < 250 mA in
  rust — elke moderne telefoonlader voldoet.
- Een 2,4 GHz wifi-netwerk. De ESP32-S3-radio ondersteunt geen 5 GHz.
- Een telefoon of laptop op datzelfde netwerk voor de WebUI.

## 1. Aanzetten

Steek de USB-C-kabel erin. Bij de eerste boot zie je de leds eerst een
korte regenboog-zelftest doen, daarna tekenen de panelen het
provisioning-scherm:

![Provisioning-scherm bij eerste boot](img/screens/provisioning_first_boot.png)

- De naam van het hotspot-netwerk, bv. **`BTClock-XXXX`** (de laatste
  twee bytes van de MAC, in hex).
- Een willekeurig gegenereerd wachtwoord van 8 tekens (gemengde case,
  zonder visueel verwarrende tekens als `0` / `O` / `1` / `l`).
- Een QR-code die `WIFI:T:WPA;S:<ssid>;P:<wachtwoord>;;` codeert.

Het wachtwoord wordt eenmalig gegenereerd, opgeslagen in NVS en
hergebruikt bij volgende boots — noteer het of scan de QR.

## 2. Verbinden met de hotspot van de BTClock

Scan de QR met je telefoon (die maakt automatisch verbinding) of doe
het handmatig:

- SSID: `BTClock-XXXX`
- Beveiliging: WPA2
- Wachtwoord: de 8 tekens op het paneel

De meeste telefoons herkennen het captive portal en openen het
inlogvenster automatisch. Gebeurt dat niet, open dan
`http://192.168.4.1` in een browser.

## 3. Kies je eigen wifi

Het captive portal toont de wifi-netwerken in de buurt. Selecteer het
jouwe, voer het wachtwoord in en sla op. Het apparaat slaat de
gegevens op, herstart en logt in als gewone STA-client. De
provisioning-hotspot verdwijnt na de volgende boot.

Wachtwoord verkeerd ingetypt? Wacht `wpTimeout` seconden (standaard
15 minuten) — het apparaat herstart terug naar provisioning-modus
zodat je het opnieuw kunt proberen.

## 4. Vind de BTClock op je netwerk

Zodra het apparaat op wifi zit, tonen de panelen de roterende
schermen. De WebUI open je via:

- **mDNS** — werkt op de meeste desktops en Linux-systemen: bezoek
  `http://btclock-xxxxxx.local` (waar `xxxxxx` het kleine-letter
  MAC-suffix is, ook zichtbaar in de WebUI bij System info).
- **Via IP** — je router toont het apparaat in zijn DHCP-tabel als
  `btclock-xxxxxx`. Bezoek dat IP rechtstreeks. Hetzelfde IP staat
  ook onder `Status → System info → IP` zodra je verbonden bent.

## 5. Open de WebUI

Je ziet drie kolommen:

1. **Control** (links) — tekst naar de panelen sturen, ledkleur
   instellen, frontlight bedienen (Rev B), firmware-update of factory
   reset starten.
2. **Status** (midden) — live preview van wat de panelen nu tonen,
   schermcyclus-timer, DND-status, signaalsterkte, uptime.
3. **Settings** (rechts) — de uitgebreide vorm van alles hieronder.

## 6. Tijdzone en valuta instellen

Onder **Settings → Timezone**: kies je IANA-zone (bv.
`Europe/Amsterdam`). De BTClock en geplande DND-vensters volgen
zomertijd automatisch. Live opgeslagen, geen herstart nodig.

Onder **Settings → Data sources → Currencies**: sleep de volgorde van
je ticker- en sats-per-valuta-rotatie. Standaard zijn dat USD, EUR en
JPY.

## 7. Kies de schermen die je wilt zien

Onder **Settings → Screens**: zet elk roterend scherm aan of uit, en
sleep om de volgorde te bepalen. De standaardcatalogus is:

- Block height (blokhoogte)
- Time (tijd)
- Halving countdown (halving-aftelling)
- Block fee rate (vergoedingstarief van het laatste blok)
- Sats per dollar (Moscow time)
- BTC ticker
- Market cap (marktkapitalisatie)
- Bitcoin supply (totale uitgegeven BTC)
- Mining pool hashrate / earnings
- Bitaxe hashrate / best difficulty

De auto-rotatie-cadans staat onder **Settings → Screen specific →
Time per screen** (standaard 30 minuten — ja, e-paper is bewust
langzaam; zet hem op 1 minuut als je liever beweging ziet). De
[Handbook → Screen catalogue](HANDBOOK.md#5-screen-catalogue) laat zien
hoe elk scherm eruit ziet.

## 8. (Optioneel) tweaks voor dag één

- **Ledkleur** (Control card → LEDs) — standaard is BTClock-oranje
  `#E04300`. De ledstrip aan de achterkant flitst in deze kleur bij
  elk nieuw blok.
- **Frontlight** (alleen Rev B — Control card → Frontlight) — kies
  altijd-aan, omgevingsgestuurd of uit.
- **Mining pool** (Settings → Mining pool) — vul de gebruiker /
  uitbetalingsadres / API-key van je pool in. Standaard staat
  `noderunners` global stats ingesteld, dus het scherm rendert ook
  zonder gebruikersgegevens. Zie de
  [pool-veldgids](WEBUI_MINING_POOL_FIELDS.md) voor wat per pool moet.
- **Bitaxe** (Settings → Bitaxe) — wijs naar de hostname of het IP
  van je miner en de Bitaxe-schermen worden actief.
- **Do Not Disturb** (Settings → Do Not Disturb) — plan een venster
  waarin de leds donker blijven; handig in een slaapkamer. Het
  e-paper zelf geeft geen licht, dus de paneelinhoud blijft zichtbaar.

## Hoe nu verder

- De volledige referentie — elk scherm, elke instelling, elk
  API-endpoint — staat in [`HANDBOOK.md`](HANDBOOK.md).
- Een community-integratie voor Home Assistant staat op
  [github.com/dsbaars/homeassistant-btclock](https://github.com/dsbaars/homeassistant-btclock)
  — entities voor elk datascherm en services voor scherm-push en
  led-besturing.
- Een browser-gebaseerde ESP-S3-flasher (geen toolchain nodig, alleen
  Chrome) staat op [web-flasher.btclock.dev](https://web-flasher.btclock.dev/)
  — de makkelijkste manier om een nieuwe printplaat te flashen of een
  kapotte firmware te herstellen.
- Wil je zelf firmware bouwen? Zie
  [`BUILD_FROM_SOURCE.md`](BUILD_FROM_SOURCE.md).
