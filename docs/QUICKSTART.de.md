# BTClock — Schnellstart

Eine zehnminütige Anleitung, mit der du eine eingeschaltete BTClock
ins WLAN bringst, auf deine Zeitzone einstellst und durch die
Bitcoin-Bildschirme rotieren lässt.

Wenn du jede Einstellung im Detail verstehen willst, lies
[`HANDBOOK.md`](HANDBOOK.md), sobald das Gerät im Netzwerk ist.

Diese Anleitung deckt die zwei Produktions­varianten ab — **Rev A**
(4 MB, 2,13"-Panels) und **Rev B** (8 MB, 2,13"-Panels, mit
Frontlicht und Umgebungs­licht­sensor). Die V8-Platine mit acht
Panels und der 2,9"-Build der Rev A sind hier außen vor; behandle sie
als Prototypen.

## Was du brauchst

- Die BTClock selbst.
- Ein USB-C-Kabel und eine 5-V-Stromquelle. Die BTClock zieht im
  Normalbetrieb < 250 mA — jedes moderne Handy-Netzteil reicht.
- Ein 2,4-GHz-WLAN. Der ESP32-S3 unterstützt kein 5 GHz.
- Ein Smartphone oder Laptop im selben Netzwerk für das WebUI.

## 1. Einschalten

Steck das USB-C-Kabel ein. Beim ersten Start siehst du die LEDs einen
kurzen Regenbogen-Selbsttest fahren; danach malen die Panels den
Provisioning-Bildschirm:

![Provisioning-Bildschirm beim ersten Start](img/screens/provisioning_first_boot.png)

- Den Hotspot-Namen, z. B. **`BTClock-XXXX`** (die letzten beiden
  Bytes der Geräte-MAC, in Hex).
- Ein zufällig erzeugtes 8-Zeichen-WPA2-Passwort (gemischte
  Groß-/Kleinschreibung, ohne visuell ähnliche Zeichen wie
  `0` / `O` / `1` / `l`).
- Einen QR-Code, der `WIFI:T:WPA;S:<ssid>;P:<passwort>;;` codiert.

Das Passwort wird einmal erzeugt, im NVS gespeichert und über
Neustarts hinweg wiederverwendet — schreib es auf oder scanne den QR.

## 2. Mit dem BTClock-Hotspot verbinden

Scanne den QR mit deinem Handy (es verbindet sich automatisch) oder
verbinde dich manuell:

- SSID: `BTClock-XXXX`
- Sicherheit: WPA2
- Passwort: die 8 Zeichen auf dem Panel

Die meisten Smartphones erkennen das Captive Portal und öffnen den
Anmeldedialog automatisch. Falls nicht, öffne `http://192.168.4.1` im
Browser.

## 3. Wähle dein heimisches WLAN

Das Captive Portal listet die nahegelegenen SSIDs. Wähle dein
Netzwerk, gib das Passwort ein, speichere. Das Gerät persistiert die
Zugangsdaten, startet neu und meldet sich als ganz normaler
STA-Client an. Der Provisioning-Hotspot verschwindet beim nächsten
Boot.

Hast du das Passwort falsch getippt? Warte `wpTimeout` Sekunden
(Standard: 15 Minuten) — das Gerät startet zurück in den
Provisioning-Modus, damit du es noch mal probieren kannst.

## 4. Die BTClock im Netzwerk finden

Sobald das Gerät im WLAN ist, zeigen die Panels die rotierenden
Bildschirme. Das WebUI erreichst du so:

- **mDNS** — funktioniert auf den meisten Desktops und Linux-Hosts:
  `http://btclock-xxxxxx.local` (wobei `xxxxxx` das MAC-Suffix in
  Kleinbuchstaben ist; steht auch in der System-Info-Karte des WebUI).
- **Per IP** — dein Router listet das Gerät als `btclock-xxxxxx` in
  der DHCP-Tabelle; rufe diese IP direkt auf. Sobald verbunden, steht
  die IP auch unter `Status → System info → IP`.

## 5. Öffne das WebUI

Du siehst drei Spalten:

1. **Control** (links) — Text auf die Panels schicken, LED-Farbe
   setzen, Frontlicht steuern (Rev B), Firmware-Update oder
   Werks-Reset auslösen.
2. **Status** (Mitte) — Live-Vorschau dessen, was die Panels gerade
   zeigen, Bildschirm-Rotation-Timer, DND-Status, Signalstärke,
   Uptime.
3. **Settings** (rechts) — die ausführliche Form von allem, was unten
   steht.

## 6. Zeitzone und Währungen einstellen

Unter **Settings → Timezone**: wähle deine IANA-Zone (z. B.
`Europe/Berlin`). Die BTClock und zeitgesteuerte DND-Fenster folgen
automatisch der Sommerzeit. Wird live gespeichert, kein Neustart
nötig.

Unter **Settings → Data sources → Currencies**: ordne deine Ticker- /
Sats-pro-Währung-Rotation per Drag & Drop. Voreinstellung: USD, EUR,
JPY.

## 7. Wähle die gewünschten Bildschirme

Unter **Settings → Screens**: aktiviere oder deaktiviere jeden
Rotations-Bildschirm und ziehe sie in die gewünschte Reihenfolge. Der
Standardkatalog:

- Block height (Blockhöhe)
- Time (Uhrzeit)
- Halving countdown (Halving-Countdown)
- Block fee rate (Gebührenrate des letzten Blocks)
- Sats per dollar (Moscow time)
- BTC ticker
- Market cap (Marktkapitalisierung)
- Bitcoin supply (zirkulierende Menge)
- Mining pool hashrate / earnings
- Bitaxe hashrate / best difficulty

Die Auto-Rotation-Kadenz steht unter **Settings → Screen specific →
Time per screen** (Standard: 30 Minuten — ja, E-Paper ist absichtlich
träge; stelle es auf 1 Minute, wenn du lieber Bewegung sehen
willst). Das
[Handbook → Screen catalogue](HANDBOOK.md#screen-catalogue) zeigt,
wie jeder Bildschirm aussieht.

## 8. (Optional) Tweaks für den ersten Tag

- **LED-Farbe** (Control-Karte → LEDs) — Standard ist das
  BTClock-Orange `#E04300`. Der LED-Streifen auf der Rückseite blinkt
  bei jedem neuen Block in dieser Farbe.
- **Frontlicht** (nur Rev B — Control-Karte → Frontlight) — wähle
  immer an, umgebungslichtgesteuert oder aus.
- **Mining-Pool** (Settings → Mining pool) — trage Benutzername,
  Auszahlungs­adresse oder API-Key deines Pools ein. Voreinstellung
  ist `noderunners` Global Stats, sodass der Bildschirm auch ohne
  persönliche Zugangsdaten funktioniert. Siehe den
  [Pool-Feldführer](WEBUI_MINING_POOL_FIELDS.md) für den richtigen
  Eintrag pro Pool.
- **Bitaxe** (Settings → Bitaxe) — zeige auf den Hostnamen oder die
  IP deines Miners und die Bitaxe-Bildschirme werden aktiv.
- **Do Not Disturb** (Settings → Do Not Disturb) — plane ein
  Zeitfenster, in dem die LEDs aus bleiben; nützlich im Schlafzimmer.
  Das E-Paper selbst gibt kein Licht ab, der Panel-Inhalt bleibt
  sichtbar.

## Wie geht's weiter

- Die vollständige Referenz — jeder Bildschirm, jede Einstellung,
  jeder API-Endpunkt — steht in [`HANDBOOK.md`](HANDBOOK.md).
- Eine Community-Integration für Home Assistant gibt es unter
  https://github.com/dsbaars/homeassistant-btclock — Entities für
  jeden Daten-Bildschirm und Services für Text-Push und LED-Steuerung.
- Ein browserbasierter ESP-S3-Flasher (keine Toolchain, nur Chrome)
  läuft unter https://web-flasher.btclock.dev/ — der einfachste Weg,
  eine frische Platine zu flashen oder eine kaputte Firmware zu
  retten.
- Willst du die Firmware selbst bauen? Siehe
  [`BUILD_FROM_SOURCE.md`](BUILD_FROM_SOURCE.md).
