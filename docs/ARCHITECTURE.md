# BTClock v4 architecture (UML)

Diagrams render natively on Forgejo / GitHub via Mermaid. Each section
maps to a real subsystem: file paths in the captions are the source of
truth — if a diagram drifts from the code, trust the code.

Conventions:
- **Class** boxes name the C++ class as it appears in source.
- **Owns** = `unique_ptr` / `optional` / by-value member.
- **Refs** = raw pointer / reference, lifetime managed elsewhere.
- **Calls** = method invocation, no ownership.

---

## 1. Component overview

Top-level subsystems and the wires between them. `AppCtx`
([main/app/app_ctx.hpp](../main/app/app_ctx.hpp)) is the runtime root —
everything else hangs off it.

```mermaid
flowchart TB
    subgraph Boot["Boot (main.cpp → init_* TUs)"]
        AppMain([app_main])
        AppCtx[(AppCtx)]
    end

    subgraph HW["Hardware (components/)"]
        I2C[I2cBus]
        MCP[Mcp23017 x1-2]
        PCA[Pca9685]
        BH[BH1750]
        EPD[EpdPanel x N]
    end

    subgraph IO["IO controllers (main/io/)"]
        FL[FrontlightController]
        LED[led_controller]
        LS[LightSensor]
        BTN[ButtonReader]
        WG[WifiGuard]
    end

    subgraph Net["Network (components/)"]
        WIFI[Wifi]
        PROV[ProvisioningServer]
        DNS[DnsHijack]
        OTA[OTA upload]
    end

    subgraph Data["Data pipeline"]
        HUB[DataHub]
        SRCS[DataSource impls]
    end

    subgraph Render["Render"]
        SM[ScreenManager]
        SCR[Screen renderers]
    end

    subgraph Web["HTTP / SSE"]
        CS[ControlServer]
        SSE[SseServer]
    end

    subgraph Storage["Persistence"]
        NVS[(NVS)]
        LFS[(LittleFS /lfs/www)]
        PR[Prefs]
        SET[settings::schema]
    end

    EVT[event_loop]

    AppMain --> AppCtx
    AppCtx -.owns.-> HW
    AppCtx -.owns.-> IO
    AppCtx -.owns.-> Net
    AppCtx -.owns.-> Data
    AppCtx -.owns.-> Render
    AppCtx -.owns.-> Web

    SRCS -- Report --> HUB
    HUB -- OnUpdate --> EVT
    BTN -- xQueueSend --> EVT
    EVT -- Render --> SM --> SCR --> EPD
    EVT -- Drain --> CS

    CS -- reads/writes --> HUB
    CS -- adapters --> IO
    CS -- adapters --> SM
    CS -- queue --> EVT
    CS -. broadcast .-> SSE

    PR --> NVS
    SET --> PR
    Web --> LFS
    WIFI --> Net
```

---

## 2. AppCtx ownership (class diagram)

`AppCtx` is behaviour-free — a struct of subsystem handles populated by
the `init_*` TUs in [main/app/boot/](../main/app/boot/). Composition
arrows point from owner to owned.

```mermaid
classDiagram
    class AppCtx {
      +I2cBus i2c
      +Mcp23017 mcp, mcp2
      +Pca9685 pca
      +Bh1750 bh
      +EpdBus epd_bus
      +array~EpdPanel~ panels
      +AppFonts fonts
      +Wifi wifi
      +ProvisioningServer portal
      +DnsHijack dns
      +OutageWatchdog outage_watchdog
      +DataHub hub
      +ScreenManager sm
      +BtclockDataSource* btclock_ws
      +MempoolKrakenSource* mempool_kraken
      +ButtonReader buttons
      +QueueHandle_t button_q
      +ZapListener zap_listener
      +ControlServer ctrl
      +SseServer sse
      +FrontlightAdapter fl_adapter
      +LedsAdapter leds_adapter
      +DndAdapter dnd_adapter
      +LightSensorAdapter light_sensor_adapter
      +TimerAdapter timer_adapter
    }

    class FrontlightController
    class LightSensor
    class DataHub
    class ScreenManager
    class ControlServer
    class SseServer
    class Wifi
    class ButtonReader

    AppCtx *-- FrontlightController
    AppCtx *-- LightSensor
    AppCtx *-- DataHub
    AppCtx *-- ScreenManager
    AppCtx *-- ControlServer
    AppCtx *-- SseServer
    AppCtx *-- Wifi
    AppCtx *-- ButtonReader
```

---

## 3. Data pipeline (class diagram)

Pure-virtual `DataSource`
([components/data_core/include/data_core/source.hpp:23](../components/data_core/include/data_core/source.hpp#L23))
is the contract; `DataHub` ([hub.hpp](../components/data_core/include/data_core/hub.hpp))
fan-ins reports under a mutex and fires an `UpdateCallback`.

```mermaid
classDiagram
    class DataSource {
      <<abstract>>
      +name() const char*
      +Start(DataHub&) esp_err_t
      +Stop() esp_err_t
    }

    class DataHub {
      -mutex
      -snapshot DataSnapshot
      -sources vector~DataSource*~
      +AddSource(DataSource*)
      +StartAll() esp_err_t
      +Report(partial)
      +Snapshot() DataSnapshot
      +SetOnUpdate(UpdateCallback)
    }

    class DataSnapshot {
      +block_height uint32
      +price map~ccy,double~
      +fees FeeRates
      +pool PoolStats
      +bitaxe BitaxeStats
      +latest_zap LatestZap
      +halving HalvingProgress
      +supply double
    }

    class BtclockDataSource {
      WSS ws.btclock.dev/api/v2
      +SetCurrencies(vec)
      +SetBlockFeeDec(int)
    }

    class MempoolKrakenSource {
      WSS mempool.space + Kraken v2
      +IsMempoolConnected() bool
      +IsKrakenConnected() bool
    }

    class NostrDataSource {
      Nostr relay subscriber
    }

    class BitaxeSource {
      LAN HTTP poller
    }

    class PoolDataSource {
      <<abstract>>
      #api_url() string
      #parse_response(body, out) bool
      #pool_name() const char*
      #poll_interval_ms() uint32
      #auth_token() string
      #user_is_secret() bool
      +SupportsDailyEarnings() bool
    }

    DataSource <|-- BtclockDataSource
    DataSource <|-- MempoolKrakenSource
    DataSource <|-- NostrDataSource
    DataSource <|-- BitaxeSource
    DataSource <|-- PoolDataSource

    DataHub o-- DataSource : sources_
    DataHub --> DataSnapshot : holds
    BtclockDataSource ..> DataHub : Report()
    MempoolKrakenSource ..> DataHub : Report()
    PoolDataSource ..> DataHub : Report()
```

---

## 4. Mining-pool plugins (class diagram)

Every `mining_pool_*` component subclasses `PoolDataSource`
([components/mining_pool_common/include/mining_pool_common/pool_base.hpp:41](../components/mining_pool_common/include/mining_pool_common/pool_base.hpp#L41)).
Two pools (ViaBTC, Foundry) inherit through an intermediate
`KeyedGetPoolBase` that swaps the auth header to `X-API-KEY`.

```mermaid
classDiagram
    class PoolDataSource {
      <<abstract>>
      #api_url() string
      #parse_response(body, out) bool
      #pool_name() const char*
      #auth_header_name() const char*
      #user_is_secret() bool
    }

    class KeyedGetPoolBase {
      <<abstract>>
      auth_header_name = X-API-KEY
      user_is_secret = true
    }

    class OceanPool
    class BraiinsPool {
      auth_header = Pool-Auth-Token
    }
    class ViaBTCPool
    class FoundryPool
    class NoderunnersPool {
      SupportsDailyEarnings = false
    }
    class SatoshiRadioPool {
      SupportsDailyEarnings = false
    }
    class PublicPoolPool {
      SupportsDailyEarnings = false
    }
    class CkPoolPool {
      SupportsDailyEarnings = false
    }
    class GobrrPool
    class NerdminerPool

    PoolDataSource <|-- OceanPool
    PoolDataSource <|-- BraiinsPool
    PoolDataSource <|-- KeyedGetPoolBase
    PoolDataSource <|-- NoderunnersPool
    PoolDataSource <|-- SatoshiRadioPool
    PoolDataSource <|-- PublicPoolPool
    PoolDataSource <|-- CkPoolPool
    PoolDataSource <|-- GobrrPool
    PoolDataSource <|-- NerdminerPool

    KeyedGetPoolBase <|-- ViaBTCPool
    KeyedGetPoolBase <|-- FoundryPool
```

---

## 5. ScreenManager + renderers (class diagram)

`ScreenManager` ([main/app/screen_manager.hpp:51](../main/app/screen_manager.hpp#L51))
owns the slot index, refresh policy, rotation timer, and last-rendered
diff state. Renderers are free template functions in
[main/screens/](../main/screens/) — there is no Screen base class.

```mermaid
classDiagram
    class ScreenManager {
      -slot_ size_t
      -dirty_ bool
      -rot_ RotationTimer
      -refresh_state_ RefreshPolicyState
      -rotation_sequence_ vector~size_t~
      -ota_active_ bool
      -debug_mode_ bool
      -custom_active_ bool
      -zap_active_ bool
      +current_kind() ScreenType
      +current_currency() string
      +SetSlot(idx, now)
      +SetKind(kind, now)
      +SetCurrency(ccy, now)
      +NextScreen(now)
      +PrevScreen(now)
      +MaybeAutoRotate(now, period)
      +SetCustomCells(cells, now)
      +SetZapNotify(now, restore, ms)
      +EnterDebug(now)
      +SetOtaOverlay(active)
      +ShouldRender(snap) bool
      +Render(panels, fb, fonts, snap)
      +RenderDebug(panels, fb, fonts, info, now, force_full)
    }

    class RotationTimer {
      +paused bool
      +Restart(now)
    }

    class RefreshPolicyState {
      +last_full_ms int64
    }

    class ScreenType {
      <<enumeration>>
      kBlockHeight
      kClock
      kBtcPrice
      kMoscowTime
      kMarketCap
      kFeeRate
      kHalving
      kBitcoinSupply
      kMiningPoolHashrate
      kMiningPoolEarnings
      kBitaxeHashrate
      kBitaxeBestDiff
      kNostrZap
      kCustom
      kDebug
      kOtaUpdate
    }

    class EpdPanel {
      +Render(fb)
      +FullRefresh()
      +PartialRefresh()
    }

    class AppFonts

    class Renderers {
      <<free functions>>
      RenderBlockHeightScreen()
      RenderClockScreen()
      RenderBtcPriceScreen()
      RenderMoscowTimeScreen()
      RenderFeeRateScreen()
      RenderHalvingScreen()
      RenderBitcoinSupplyScreen()
      RenderMarketCapScreen()
      RenderMiningPoolHashrateScreen()
      RenderMiningPoolEarningsScreen()
      RenderBitaxeHashrateScreen()
      RenderBitaxeBestDiffScreen()
      RenderNostrZapScreen()
      RenderCustomScreen()
      RenderDebugScreen()
      RenderOtaUpdateScreen()
    }

    ScreenManager *-- RotationTimer
    ScreenManager *-- RefreshPolicyState
    ScreenManager ..> ScreenType : current_kind()
    ScreenManager ..> Renderers : Render() dispatch
    Renderers --> EpdPanel : Render(fb)
    Renderers --> AppFonts : reads
```

---

## 6. ControlServer + adapter interfaces (class diagram)

`ControlServer` ([components/webserver/include/control_server.hpp](../components/webserver/include/control_server.hpp))
talks to `main/` only through pure-virtual `*Iface` interfaces so the
webserver component never has to include `main/`. `main.cpp` instantiates
adapter structs in [main/app/boot/adapters.hpp](../main/app/boot/adapters.hpp)
that forward to the real subsystems.

```mermaid
classDiagram
    class ControlServer {
      -httpd_handle_t
      -DataHub& hub
      -Wifi& wifi
      -fl FrontlightIface*
      -leds LedsIface*
      -dnd DndIface*
      -timer TimerIface*
      -light LightSensorIface*
      -button_q QueueHandle_t
      -sse SseServer*
      +Start()
      +Stop()
    }

    class FrontlightIface {
      <<interface>>
      +On() / Off() / Flash()
      +SetBrightness(duty)
      +GetStatus() Status
    }
    class LedsIface { <<interface>> }
    class DndIface {
      <<interface>>
      +GetStatus() Status
      +SetEnabled(bool)
    }
    class TimerIface {
      <<interface>>
      +IsPaused() bool
      +SetPaused(bool)
      +Restart()
    }
    class LightSensorIface { <<interface>> }

    class FrontlightAdapter
    class LedsAdapter
    class DndAdapter
    class TimerAdapter
    class LightSensorAdapter

    class FrontlightController
    class led_controller
    class Dnd
    class ScreenManager
    class LightSensor

    FrontlightIface <|.. FrontlightAdapter
    LedsIface <|.. LedsAdapter
    DndIface <|.. DndAdapter
    TimerIface <|.. TimerAdapter
    LightSensorIface <|.. LightSensorAdapter

    FrontlightAdapter --> FrontlightController : forwards
    LedsAdapter --> led_controller : forwards
    DndAdapter --> Dnd : forwards
    TimerAdapter --> ScreenManager : forwards
    LightSensorAdapter --> LightSensor : forwards

    ControlServer --> FrontlightIface
    ControlServer --> LedsIface
    ControlServer --> DndIface
    ControlServer --> TimerIface
    ControlServer --> LightSensorIface
```

---

## 7. IO controllers (class diagram)

[main/io/](../main/io/) wraps the chip drivers in higher-level controllers
the rest of the app talks to.

```mermaid
classDiagram
    class FrontlightController {
      -Pca9685* pca
      -FrontlightFader fader
      -AmbientPolicy policy
      +On() / Off() / Flash()
      +SetBrightness(duty)
      +Tick(now, lux)
    }

    class FrontlightFader {
      +StepTowards(target)
    }

    class AmbientPolicy {
      +Decide(lux, threshold) AutoOff
    }

    class led_controller {
      <<namespace>>
      +SetColor(idx, rgb)
      +Off()
      +Flash(rgb, ms)
      +Identify()
      +OnBlockUpdate()
    }

    class LightSensor {
      -Bh1750& sensor
      -task TaskHandle_t
      +IsAvailable() bool
      +Lux() uint32
    }

    class ButtonReader {
      -Mcp23017* mcp
      -task TaskHandle_t
      -queue QueueHandle_t
      +Start(queue)
      +Stop()
      +SetInverted(bool)
      +SetOnAllButtonsLongPress(cb)
    }

    class Mcp23017
    class Pca9685
    class Bh1750
    class WifiGuard {
      Auto-fallback to AP
      after sustained STA loss
    }

    FrontlightController *-- FrontlightFader
    FrontlightController *-- AmbientPolicy
    FrontlightController --> Pca9685
    LightSensor --> Bh1750
    ButtonReader --> Mcp23017
    WifiGuard --> Wifi
```

---

## 8. Boot sequence

`app_main` ([main/main.cpp:37](../main/main.cpp#L37)) is straight-line
wire-up. Each step populates `AppCtx`; the last call hands off to the
event loop.

```mermaid
sequenceDiagram
    autonumber
    participant ESP as ESP-IDF
    participant M as app_main
    participant C as AppCtx
    participant HW as InitHardware
    participant P as InitPanels
    participant S as InitStorage
    participant N as InitNetwork
    participant SM as InitScreenManager
    participant DP as DispatchBootPath
    participant API as InitControlApi
    participant DNS as InitMdns
    participant EL as RunEventLoop

    ESP->>M: app_main()
    M->>M: vTaskPrioritySet(20)
    M->>M: InitBootLeds()
    M->>C: construct AppCtx
    M->>HW: I2C, MCP23017, PCA9685, BH1750
    HW-->>C: ctx.i2c / mcp / pca / bh
    M->>P: EpdBus, EpdPanel[N], splash render
    P-->>C: ctx.epd_bus, ctx.panels
    M->>S: NVS mount, LittleFS, TZ
    S-->>C: prefs ready
    M->>N: WiFi: STA creds in NVS?
    alt creds present
        N->>N: Wifi::Connect(ssid,pw)
        N-->>C: ctx.wifi (STA)
    else missing or failed
        N->>N: StartSoftAp + DnsHijack
        N-->>C: ctx.wifi (AP), ctx.portal
    end
    M->>SM: ScreenManager + ButtonReader
    SM-->>C: ctx.sm, ctx.buttons, ctx.button_q
    M->>DP: branch on AP vs STA
    alt AP mode
        DP->>DP: RenderProvisioningScreen
        DP->>DP: ProvisioningServer.Start()
    else STA mode
        DP->>DP: WireDataSources(hub, sources)
        DP->>DP: InitZapListener (optional)
    end
    M->>API: ControlServer + SseServer + adapters
    API-->>C: ctx.ctrl, ctx.sse
    M->>DNS: mDNS advertise (STA only)
    M->>EL: RunEventLoop(ctx) [noreturn]
```

---

## 9. Data update → render

Hot path from a data source receiving a frame to pixels on the EPD.

```mermaid
sequenceDiagram
    autonumber
    participant Src as DataSource (e.g. BtclockDataSource)
    participant Hub as DataHub
    participant SSE as SseServer
    participant EL as event_loop (main task)
    participant SM as ScreenManager
    participant R as Renderer (free fn)
    participant EPD as EpdPanel[N]
    participant LED as led_controller

    Src->>Src: WSS frame arrives (own task)
    Src->>Hub: Report(partial DataSnapshot)
    Hub->>Hub: lock, merge, unlock
    Hub-->>SSE: OnUpdate fan-out (JSON broadcast)
    Hub-->>EL: OnUpdate notify
    EL->>SM: ConsumeNewBlock(snap)?
    alt new block
        SM-->>LED: OnBlockUpdate (flash)
        opt stealFocus
            EL->>SM: SetKind(kBlockHeight, now)
        end
    end
    EL->>SM: ShouldRender(snap)?
    alt yes
        EL->>SM: Render(panels, fb, fonts, snap)
        SM->>R: dispatch by current_kind()
        R->>EPD: write framebuffer + Refresh
        EPD-->>R: BUSY done
        SM->>SM: clear dirty_, stamp last_rendered_*
    end
```

---

## 10. Button press → screen change

Buttons live on an MCP23017; the reader polls in its own task and drops
events into a queue the main task drains.

```mermaid
sequenceDiagram
    autonumber
    participant U as User
    participant MCP as Mcp23017
    participant BR as ButtonReader (task)
    participant Q as button_q (FreeRTOS queue)
    participant EL as event_loop
    participant SM as ScreenManager
    participant R as Renderer
    participant EPD as EpdPanel[N]

    U->>MCP: press button N
    BR->>MCP: read GPA pins (poll)
    BR->>BR: debounce, classify click vs long-press
    BR->>Q: xQueueSend(ButtonInput{id, event})
    EL->>Q: xQueueReceive (one per pass)
    alt button 1 (pause)
        EL->>SM: TogglePaused()
    else button 2 (back)
        EL->>SM: PrevScreen(now)
    else button 3 (forward)
        EL->>SM: NextScreen(now)
    else button 4 (debug)
        EL->>SM: ToggleDebug(now)
    else all buttons long-press (≥5 s)
        EL->>EL: factory_reset trigger
    end
    EL->>SM: ShouldRender(snap) → true
    EL->>SM: Render(...)
    SM->>R: dispatch
    R->>EPD: paint
```

---

## 11. HTTP API → screen change

A WebUI / curl client jumps to a specific slot via
`POST /api/show/screen?s=<idx>`. Handlers run on the httpd worker task;
state mutation is queued for the main task to keep `ScreenManager`
single-threaded.

```mermaid
sequenceDiagram
    autonumber
    participant Client as WebUI / curl
    participant HTTPD as esp_http_server worker
    participant CS as ControlServer
    participant AG as AuthGate
    participant Q as button_q (CMD)
    participant EL as event_loop (main task)
    participant SM as ScreenManager
    participant R as Renderer

    Client->>HTTPD: POST /api/show/screen?s=42
    HTTPD->>CS: registered handler
    CS->>AG: RequireHttpAuth(req)
    alt auth fails
        AG-->>Client: 401 Unauthorized
    end
    CS->>CS: parse query, validate range
    CS->>Q: xQueueSend(ControlCommand::ShowSlot{42})
    CS-->>Client: 200 OK (immediate)
    EL->>Q: xQueueReceive (drain phase)
    EL->>SM: SetSlot(42, now)
    EL->>SM: Render(...)
    SM->>R: dispatch
```

---

## 12. WiFi provisioning (AP → STA)

Cold-boot path when no STA credentials are stored: device puts up a
SoftAP, the user joins it, the captive portal collects the new SSID/PSK,
the device retries against STA, and on success persists creds and
restarts.

```mermaid
sequenceDiagram
    autonumber
    participant U as User phone
    participant W as Wifi
    participant DNS as DnsHijack
    participant PROV as ProvisioningServer
    participant P as Prefs (NVS)
    participant ESP as esp_restart

    Note over W: InitNetwork: no STA creds in NVS
    W->>W: StartSoftAp("BTClock-XXXX")
    W->>DNS: hijack *.local → 192.168.4.1
    PROV->>PROV: HTTP server up on AP
    U->>W: join AP SSID
    U->>PROV: GET / (captive portal)
    PROV-->>U: HTML form
    U->>PROV: GET /scan
    PROV->>W: Wifi::Scan()
    W-->>PROV: visible networks
    PROV-->>U: JSON list
    U->>PROV: POST /save {ssid, pw}
    PROV->>W: TryConnect(ssid, pw) [synchronous]
    alt creds work
        W-->>PROV: kConnected
        PROV->>P: SetString(ssid, pw) + Commit()
        PROV-->>U: 200 (rebooting)
        PROV->>ESP: esp_restart()
    else creds rejected
        W-->>PROV: kDisconnected (reason)
        PROV-->>U: 401/422 with reason for retry
    end
```

---

## 13. OTA firmware upload

`POST /upload/firmware` streams the new app partition. The handler
latches the OTA overlay on `ScreenManager` so the EPD shows progress
and the main loop stays out of the renderer for the duration.

```mermaid
sequenceDiagram
    autonumber
    participant Cli as curl / WebUI
    participant HTTPD as httpd worker
    participant CS as ControlServer
    participant AG as AuthGate
    participant SM as ScreenManager
    participant OTA as esp_ota
    participant EPD as EpdPanel[N]
    participant ESP as esp_restart

    Cli->>HTTPD: POST /upload/firmware (~1.5 MiB)
    HTTPD->>CS: handler
    CS->>AG: RequireHttpAuth (if enabled)
    CS->>SM: SetOtaOverlay(true)
    CS->>EPD: RenderOtaUpdateScreen (once)
    CS->>OTA: esp_ota_begin(next_partition)
    loop chunked body
        Cli->>CS: chunk
        CS->>OTA: esp_ota_write(buf)
    end
    CS->>OTA: esp_ota_end + esp_ota_set_boot_partition
    alt write OK
        CS-->>Cli: 200 OK
        CS->>ESP: esp_restart()
    else write failed
        CS->>SM: SetOtaOverlay(false)
        CS-->>Cli: 5xx
    end
```

---

## 14. WiFi state machine

```mermaid
stateDiagram-v2
    [*] --> kIdle : Wifi()
    kIdle --> kConnecting : Connect(ssid, pw)
    kConnecting --> kConnected : got IP
    kConnecting --> kDisconnected : auth failed / timeout
    kConnected --> kDisconnected : link drop
    kDisconnected --> kConnecting : auto-retry (5 s)
    kDisconnected --> kAp : WifiGuard escalation
    kIdle --> kAp : StartSoftAp() (no creds)
    kAp --> kConnecting : TryConnect() from portal
    kAp --> [*] : esp_restart after save
```

---

## 15. ScreenManager mode/overlay priority

ScreenManager has several latching overlays. Priority (highest first):
`OTA > Debug > Zap notify > Custom > rotation slot`.

```mermaid
stateDiagram-v2
    [*] --> Rotation
    Rotation --> Custom : SetCustomCells()
    Custom --> Rotation : Next/Prev/SetSlot/auto-rotate
    Rotation --> Zap : SetZapNotify()
    Zap --> Rotation : timeout (auto_restore) / nav
    Rotation --> Debug : ToggleDebug() / button 4
    Debug --> Rotation : ToggleDebug()
    Rotation --> OTA : SetOtaOverlay(true)
    Custom --> OTA : SetOtaOverlay(true)
    Zap --> OTA : SetOtaOverlay(true)
    Debug --> OTA : SetOtaOverlay(true)
    OTA --> [*] : esp_restart
```

---

## File index

- Boot: [main/main.cpp](../main/main.cpp), [main/app/boot/](../main/app/boot/)
- Runtime: [main/app/event_loop.cpp](../main/app/event_loop.cpp), [main/app/screen_manager.hpp](../main/app/screen_manager.hpp)
- Data: [components/data_core/](../components/data_core/), [main/sources/](../main/sources/)
- Pools: [components/mining_pool_common/](../components/mining_pool_common/), [components/mining_pool_*/](../components/)
- HTTP: [components/webserver/](../components/webserver/)
- IO: [main/io/](../main/io/)
- Network: [components/wifi/](../components/wifi/)
- Settings: [components/settings/](../components/settings/), [components/prefs/](../components/prefs/)
