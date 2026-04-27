# BTClock — Inicio rápido

Una guía de diez minutos para conectar un BTClock ya encendido a tu
Wi-Fi, configurar tu zona horaria y dejarlo rotando entre las
pantallas de Bitcoin.

Si quieres profundizar en cada ajuste, lee
[`HANDBOOK.md`](HANDBOOK.md) cuando el dispositivo esté en la red.

Esta guía cubre las dos variantes de producción — **Rev A** (4 MB,
paneles de 2,13") y **Rev B** (8 MB, paneles de 2,13", con frontlight
y sensor de luz ambiental). La V8 de 8 paneles y la Rev A con panel
2,9" están fuera del alcance; trátalas como prototipos.

## Lo que necesitas

- El propio BTClock.
- Un cable USB-C y una fuente de 5 V. El BTClock consume < 250 mA en
  régimen estable — cualquier cargador de móvil moderno sirve.
- Una red Wi-Fi de 2,4 GHz. La radio del ESP32-S3 no soporta 5 GHz.
- Un móvil o portátil en la misma red para el WebUI.

## 1. Encender

Conecta el cable USB-C. En el primer arranque verás los LED hacer un
breve auto-test arcoíris; luego los paneles dibujan la pantalla de
provisión:

![Pantalla de provisión en el primer arranque](img/screens/provisioning_first_boot.png)

- El nombre del punto de acceso, p. ej. **`BTClock-XXXX`** (los dos
  últimos bytes de la MAC, en hex).
- Una contraseña WPA2 de 8 caracteres generada aleatoriamente
  (mayúsculas y minúsculas, sin caracteres visualmente ambiguos como
  `0` / `O` / `1` / `l`).
- Un código QR que codifica `WIFI:T:WPA;S:<ssid>;P:<contraseña>;;`.

La contraseña se genera una sola vez, se guarda en NVS y se reutiliza
en arranques posteriores — anótala o escanea el QR.

## 2. Conéctate al hotspot del BTClock

Escanea el QR con tu móvil (se conectará automáticamente) o hazlo
manualmente:

- SSID: `BTClock-XXXX`
- Seguridad: WPA2
- Contraseña: los 8 caracteres mostrados en el panel

La mayoría de móviles detectan el captive portal y abren la hoja de
inicio de sesión automáticamente. Si el tuyo no, abre
`http://192.168.4.1` en el navegador.

## 3. Elige tu Wi-Fi doméstica

El captive portal lista los SSID cercanos. Selecciona el tuyo, escribe
la contraseña y guarda. El dispositivo persiste las credenciales,
reinicia y se reconecta como cliente STA normal. El hotspot de
provisión desaparece en el siguiente arranque.

Si tecleaste mal la contraseña, espera `wpTimeout` segundos (15
minutos por defecto) — el dispositivo reiniciará a modo provisión
para que vuelvas a intentarlo.

## 4. Encuentra el BTClock en tu red

Una vez que el dispositivo esté en Wi-Fi, los paneles muestran las
pantallas en rotación. Para abrir el WebUI:

- **mDNS** — funciona en la mayoría de escritorios y máquinas Linux:
  visita `http://btclock-xxxxxx.local` (donde `xxxxxx` es el sufijo
  de la MAC en minúsculas, también visible en la tarjeta System info
  del WebUI).
- **Por IP** — tu router listará el dispositivo como `btclock-xxxxxx`
  en su tabla DHCP; visita esa IP directamente. La IP también aparece
  en `Status → System info → IP` cuando estés conectado.

## 5. Abre el WebUI

Verás tres columnas:

1. **Control** (izquierda) — empuja texto a los paneles, fija el color
   de los LED, controla el frontlight (Rev B), lanza una actualización
   de firmware o un reset de fábrica.
2. **Status** (centro) — vista previa en vivo de lo que muestran los
   paneles ahora mismo, temporizador del ciclo de pantallas, estado
   DND, intensidad de señal, uptime.
3. **Settings** (derecha) — la versión extendida de todo lo de abajo.

## 6. Configura la zona horaria y las divisas

En **Settings → Timezone**: elige tu zona IANA (p. ej.
`Europe/Madrid`). El BTClock y las ventanas DND programadas seguirán
DST automáticamente. Se guarda en vivo, sin reinicio.

En **Settings → Data sources → Currencies**: arrastra para reordenar
la rotación del ticker / sats-por-divisa. Por defecto: USD, EUR, JPY.

## 7. Elige las pantallas que quieras

En **Settings → Screens**: activa o desactiva cada pantalla en
rotación, y arrástralas para fijar el orden. El catálogo por defecto
es:

- Block height (altura de bloque)
- Time (hora)
- Halving countdown (cuenta atrás del halving)
- Block fee rate (tasa de comisión del último bloque)
- Sats per dollar (Moscow time)
- BTC ticker
- Market cap (capitalización de mercado)
- Bitcoin supply (suministro circulante)
- Mining pool hashrate / earnings
- Bitaxe hashrate / best difficulty

La cadencia de auto-rotación está en **Settings → Screen specific →
Time per screen** (por defecto 30 minutos — sí, el e-paper es lento a
propósito; bájalo a 1 minuto si prefieres ver movimiento). El
[Handbook → Screen catalogue](HANDBOOK.md#screen-catalogue) muestra
cómo se ve cada pantalla.

## 8. (Opcional) ajustes para el primer día

- **Color de LED** (tarjeta Control → LEDs) — el valor por defecto es
  el naranja BTClock `#E04300`. La tira trasera de LED parpadea en
  ese color con cada bloque nuevo.
- **Frontlight** (sólo Rev B — tarjeta Control → Frontlight) — elige
  siempre encendido, controlado por luz ambiental, o apagado.
- **Mining pool** (Settings → Mining pool) — introduce el usuario, la
  dirección de pago o la clave API de tu pool. Por defecto está
  `noderunners` con estadísticas globales, así que la pantalla
  funciona sin credenciales propias. Consulta la
  [guía de campos de pool](WEBUI_MINING_POOL_FIELDS.md) para qué
  rellenar en cada pool.
- **Bitaxe** (Settings → Bitaxe) — apunta al hostname o IP de tu
  miner y las pantallas de Bitaxe se activan.
- **Do Not Disturb** (Settings → Do Not Disturb) — programa una
  ventana en la que los LED queden apagados; útil en un dormitorio.
  El e-paper en sí no emite luz, así que el contenido de los paneles
  sigue visible.

## Qué viene después

- La referencia completa — cada pantalla, cada ajuste, cada endpoint
  de la API — está en [`HANDBOOK.md`](HANDBOOK.md).
- Una integración comunitaria para Home Assistant en
  [github.com/dsbaars/homeassistant-btclock](https://github.com/dsbaars/homeassistant-btclock)
  — entidades para cada pantalla de datos y servicios para empujar
  texto y controlar los LED.
- Un flasher de ESP-S3 basado en el navegador (sin toolchain, sólo
  Chrome) en [web-flasher.btclock.dev](https://web-flasher.btclock.dev/)
  — la forma más fácil de flashear una placa nueva o recuperar de un
  firmware roto.
- ¿Quieres compilar el firmware tú mismo? Mira
  [`BUILD_FROM_SOURCE.md`](BUILD_FROM_SOURCE.md).
