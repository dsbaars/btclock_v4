// HTTP control-API server — the runtime counterpart to the provisioning
// portal. Started in STA mode after WiFi comes up. Mirrors the endpoints
// exposed by the production firmware (src/lib/net/webserver/*) so the
// existing WebUI in data/ can drive the firmware unchanged.
//
// The endpoint surface is defined by data/static/swagger.yml. Endpoints
// whose backing subsystem has not yet been ported to IDF (DND, lights,
// frontlight, OTA, full settings CRUD) are registered as 501 stubs so
// clients see a clear "not implemented" error rather than a 404 that
// makes it look like the route is missing entirely.
//
// Threading: all request handlers run on the esp_http_server worker
// task. Subsystems safe to touch directly from that task — DataHub (has
// its own mutex), esp_wifi_*, esp_system, esp_timer, NVS — are called
// inline. Anything that mutates main-loop state (screen index, screen
// manager flags, EPD refresh, esp_restart) goes through the command
// queue so the main task can process it on its next iteration without
// us having to synchronise ScreenManager.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "data_core/hub.hpp"
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "wifi.hpp"

namespace btclock {

class SseServer;  // components/webserver/include/sse_server.hpp


// Minimal abstract surface the control server calls to drive the
// per-board frontlight controller. Kept as a pure-virtual interface so
// the webserver component doesn't have to depend on `main/`, where the
// concrete `FrontlightController` lives; main wires up a thin adapter
// (see main.cpp). Boards without a frontlight (Rev A, V8) pass
// `nullptr` and the /api/frontlight/* handlers respond 503.
class FrontlightIface {
 public:
  struct Status {
    bool enabled;
    uint16_t current_duty;
    uint16_t target_duty;
    uint16_t configured_brightness;
    uint32_t lux_threshold;
    bool ambient_auto_off;
  };
  virtual ~FrontlightIface() = default;
  virtual void On() = 0;
  virtual void Off() = 0;
  virtual void Flash() = 0;
  virtual void SetBrightness(uint16_t duty) = 0;
  virtual Status GetStatus() const = 0;
};

// Do-Not-Disturb surface. The webserver component stays free of the
// dnd component (and its NVS + mutex deps) by routing all DND reads
// and writes through this interface. main.cpp wires it to the
// process-wide btclock::dnd::Dnd singleton. Nullptr -> /api/dnd/*
// endpoints respond 503 and /api/status emits the off/false stub.
class DndIface {
 public:
  struct Status {
    bool enabled = false;         // manual "force on now" flag
    bool time_enabled = false;    // schedule gate
    uint8_t start_hour = 0;
    uint8_t start_minute = 0;
    uint8_t end_hour = 0;
    uint8_t end_minute = 0;
    bool active = false;          // current wall-clock query
  };
  virtual ~DndIface() = default;
  virtual Status GetStatus() const = 0;
  virtual void SetEnabled(bool enabled) = 0;
};

// Screen-rotation timer control — lets /api/action/pause and
// /api/action/timer_restart reach the main-loop ScreenManager without
// the webserver component having to depend on main/. Concrete
// adapter in main.cpp forwards to the ScreenManager instance.
class TimerIface {
 public:
  virtual ~TimerIface() = default;
  virtual bool IsPaused() const = 0;
  virtual void SetPaused(bool paused) = 0;
  virtual void Restart() = 0;
};

// Ambient-light sensor surface. Exposed through ControlServer::Config
// so the settings/status handlers can emit `hasLightLevel`/`lightLevel`
// without the webserver component depending on the BH1750 driver. The
// concrete adapter in main.cpp forwards to `btclock::LightSensor`.
// Nullptr (or IsAvailable() == false) -> response suppresses the field.
class LightSensorIface {
 public:
  virtual ~LightSensorIface() = default;
  virtual bool IsAvailable() const = 0;
  virtual float GetLux() const = 0;
};

// NeoPixel-side counterpart to FrontlightIface. Keeps the webserver
// component independent of `main/io/led_controller.hpp` (which drags
// FreeRTOS + RMT + NVS into every TU that includes it). `main.cpp`
// wires up a thin adapter that forwards these calls to the controller's
// namespace-level functions.
//
// Semantics mirror the old firmware's /api/lights surface:
//   - GetStatus: read the current per-pixel mirror + master prefs.
//   - SetSolidColor: paint every pixel the same colour; 0 = off.
//   - SetPixels: per-pixel RGB; `count` clamped to the strip width.
//   - SetDisabled: global mute flag (persisted to NVS).
//   - TriggerIdentify: fire the identify effect (rapid multi-colour).
class LedsIface {
 public:
  struct Status {
    uint8_t brightness = 0;
    uint32_t block_flash_color = 0;
    bool disabled = false;
    bool flash_on_update = false;
    // Per-pixel colour mirror. `pixel_count` is the filled prefix.
    uint32_t pixels[8] = {0};
    uint32_t pixel_count = 0;
  };
  virtual ~LedsIface() = default;
  virtual Status GetStatus() const = 0;
  virtual void SetSolidColor(uint32_t rgb) = 0;
  virtual void SetPixels(const uint32_t* rgb_array, uint32_t count) = 0;
  virtual void SetDisabled(bool disabled) = 0;
  virtual void SetBrightness(uint8_t value) = 0;
  virtual void SetBlockFlashColor(uint32_t rgb) = 0;
  virtual void TriggerIdentify() = 0;
};

// Commands the HTTP task posts to the main task. `arg` is command-
// specific (e.g. slot index for kShowScreen). Keep this trivially
// copyable — we xQueueSend it by value.
struct ControlCommand {
  enum class Kind : uint8_t {
    kFullRefresh,
    kIdentify,
    kRestart,
    kShowScreen,      // arg_i = slot index
    kShowCurrency,    // arg_s = currency code
    kNextScreen,
    kPrevScreen,
    kStopDataSources,
    kRestartDataSources,
    // Runtime-pushed custom screen. The per-panel string payload doesn't
    // fit in this trivially-copyable struct (up to 8 × multi-char
    // strings), so the payload is stored on ControlServer::pending_custom_
    // and consumed by main via TakePendingCustomCells() when it handles
    // this kind. If two requests race, the later payload wins — matches
    // the old EPDManager::setContent which overwrites unconditionally.
    kShowCustom,
  };
  Kind kind;
  int32_t arg_i = 0;
  char arg_s[16] = {0};  // short enough for currency codes ("USD"+"\0")
};

class ControlServer {
 public:
  struct Config {
    Wifi* wifi = nullptr;
    DataHub* hub = nullptr;
    // Non-empty list of active currency codes. The control server uses
    // it to resolve /api/show/currency?c=<code> against /api/status's
    // `actCurrencies` and to size price arrays in /api/status.
    std::vector<std::string> currencies;
    // Number of EPD panels — drives the size of /api/status `data`.
    // The array contents come from ScreenManager via LiveStatus's
    // `panel_texts`; this keeps the length pinned even before the
    // first render has populated that mirror.
    size_t num_screens = 0;
    // Human-readable hardware label, e.g. "Rev B", "V8".
    std::string hw_name;
    // Optional frontlight hook. Nullptr on boards without a PCA9685
    // backlight (Rev A, V8); /api/frontlight/* then responds 503.
    FrontlightIface* frontlight = nullptr;
    // NeoPixel control surface. Nullptr if the LED subsystem failed to
    // initialise — /api/lights/* will 503 in that case. Every BTClock
    // board ships with WS2812B strips so this is expected to be set.
    LedsIface* leds = nullptr;
    // DND control surface. Nullptr -> /api/dnd/* returns 503 and the
    // /api/status `dnd` block reports the inactive stub.
    DndIface* dnd = nullptr;
    // Screen-rotation timer. Nullptr -> /api/action/pause and
    // /api/action/timer_restart respond 503.
    TimerIface* timer = nullptr;
    // Ambient-light sensor. Nullptr (or IsAvailable() == false) ->
    // /api/settings response reports `hasLightLevel: false` and
    // suppresses the `lightLevel` number. Rev A / V8 have no BH1750
    // and wire this as nullptr.
    LightSensorIface* light_sensor = nullptr;

    // Full rotatable-screen catalogue used by /api/settings. Order
    // here is the fallback rotation order (the WebUI shows this as
    // the default before the user customises). Id/name map entries
    // mirror ScreenMapping in the old firmware.
    struct ScreenEntry {
      int id;
      std::string name;
    };
    std::vector<ScreenEntry> screens_catalog;
    // Fonts + pools + currencies the renderer and data-source layer
    // support. Drives the "availableFonts" / "availablePools" /
    // "availableCurrencies" arrays in GET /api/settings so the WebUI
    // can populate its dropdowns. `currencies` above is the *active*
    // subset; `available_currencies` is the full set.
    std::vector<std::string> available_fonts;
    std::vector<std::string> available_pools;
    std::vector<std::string> available_currencies;

    // Fires when PATCH /api/settings writes any DND field (schedule
    // enable or the hh:mm window). The callback is expected to mirror
    // the fresh NVS values into the runtime DND subsystem so the LED
    // and frontlight suppressors pick up the new window immediately —
    // without this hook, the cached snapshot inside `dnd::Instance()`
    // keeps serving the old values until reboot. Nullable; a null
    // callback reproduces the pre-hook behaviour.
    std::function<void()> on_dnd_changed;

    // Fires when PATCH /api/settings writes `tzString`. The callback
    // receives the IANA zone name (validated-but-not-yet-applied to the
    // process TZ) and is expected to call timezone::SetTimezoneByName
    // so the clock screen, log timestamps, and any scheduled work pick
    // up the new zone without a reboot. Nullable: when unwired the zone
    // only takes effect on the next boot (InitFromNvs reads the settings
    // namespace directly).
    std::function<void(const std::string&)> on_tz_changed;

    // Fires when PATCH /api/settings writes `invertedColor`. The
    // callback receives the stored bool; main installs it on the EPD
    // driver (`EpdSetGlobalInverted`) and marks the screen dirty so the
    // next frame repaints with the new polarity. Nullable — an unwired
    // callback leaves the change deferred to reboot (main.cpp reads the
    // pref once at InitOnce + first Render).
    std::function<void(bool)> on_inverted_color_changed;

    // Fires when PATCH /api/settings writes `fontName`. The callback
    // receives the new id string (already validated against
    // available_fonts by the settings layer) and is expected to call
    // AppFonts::SetFamily() + ScreenManager::MarkDirty() so the next
    // frame paints with the newly selected family. Nullable — an
    // unwired callback defers the change to reboot, which is what the
    // firmware did before this hook existed.
    std::function<void(const std::string&)> on_font_changed;

    // Fires on POST /api/factory_reset after the confirmation gate has
    // accepted the body. The callback is expected to render a
    // "RESETTING" splash on the EPDs and then call
    // btclock::settings::PerformFactoryReset(). Because the helper is
    // [[noreturn]], control never comes back to the HTTP task — the
    // response has already been sent before we invoke the callback.
    // Nullable: a null callback responds 503 so the WebUI can tell
    // the user why the reset didn't take effect.
    std::function<void()> on_factory_reset;

    // Fires when PATCH /api/settings writes `blockFlashColor`. The
    // callback receives the stored uint32 (0x00RRGGBB) and is expected
    // to mirror it into the LED controller's namespace so the next
    // block flash uses the new colour without a reboot. Nullable: a
    // null callback leaves the change deferred — the settings NVS key
    // is updated but the LED controller's cached value stays stale.
    std::function<void(uint32_t)> on_block_flash_color_changed;

    // Fires when PATCH /api/settings writes `ledBrightness` /
    // `disableLeds` / `ledFlashOnUpd`. Callbacks push the new value
    // into the LED controller's runtime state so the change applies
    // without reboot — without these hooks, the NVS key is updated but
    // the controller keeps serving the boot-time value until restart.
    // Nullable: an unwired callback defers the change to reboot.
    std::function<void(uint8_t)> on_led_brightness_changed;
    std::function<void(bool)> on_disable_leds_changed;
    std::function<void(bool)> on_led_flash_on_upd_changed;

    // Fires on every successful PATCH /api/settings (after NVS commit,
    // before the response is sent). The callback is expected to emit
    // a short visible confirmation (e.g. green LED pulse) so the user
    // knows the save landed. Nullable: a null callback leaves the save
    // silent beyond the 200 OK.
    std::function<void()> on_settings_patched;

    // Fires when PATCH /api/settings touches `screenOrder`, any
    // `screen<id>Visible` key, or `actCurrencies`. The callback is
    // expected to rebuild the ScreenManager's rotation sequence (and
    // refresh per-currency slot expansion) from the freshly-persisted
    // NVS values so the new order / visibility / currency set takes
    // effect on the next auto-rotate or /api/screen/next without a
    // reboot. Without this hook the sequence is only built at boot
    // (init_screen_manager.cpp) and a runtime PATCH writes NVS but
    // leaves the runtime traversal stale. Nullable: a null callback
    // defers the change to reboot.
    std::function<void()> on_screens_changed;

    // Fires when PATCH /api/settings touches `blockFeeDec`. Receives
    // the new bool. The callback is expected to switch the v2 WS
    // client's fee-stream subscription between "blockfee" (integer)
    // and "blockfee2" (2-decimal) without a reboot. Without this hook
    // the WS keeps streaming the previously-selected stream until the
    // next reconnect and HandleBinaryFrame would either drop or
    // double-up fee ticks. Nullable: a null callback defers to reboot.
    std::function<void(bool)> on_block_fee_dec_changed;

    // Live Nostr zap-relay connection state. Non-null only when the
    // zap listener is wired (nostrZapNotify=true + valid relay URL +
    // 64-char zap pubkey). Returns false while the WebSocket is
    // disconnected or reconnecting so /api/status `connectionStatus.nostr`
    // mirrors reality — a stale "true" here would mask relay failures
    // from the WebUI's health indicator.
    std::function<bool()> nostr_connected;

    // Live data-source connection state for the /api/status
    // `connectionStatus.price` / `connectionStatus.blocks` / `V2`
    // fields. Each callback returns the current connection state of the
    // corresponding upstream:
    //   * price_connected  — price channel (Kraken on dataSource=1,
    //                        the v2 WS on dataSource=0)
    //   * blocks_connected — blocks/fees channel (mempool.space on
    //                        dataSource=1, the v2 WS on dataSource=0)
    //   * v2_connected     — true only when the btclock_v2 source is the
    //                        active source AND its socket is up
    // All three are nullable. When null, the GET /api/status handler
    // falls back to a "hub is wired" heuristic, which was the pre-bd
    // behaviour and is approximately correct only on the v2 path.
    // bd btclock_v4-1xc.
    std::function<bool()> price_connected;
    std::function<bool()> blocks_connected;
    std::function<bool()> v2_connected;

    // Fires when PATCH /api/settings touches a runtime-editable nostr
    // key (`nostrZapPubkey`, `nostrZapNotify`, `ledFlashOnZap`,
    // `flFlashOnZap`, `scrnRestoreZap`). The callback is expected to
    // re-read the canonical "settings" namespace and rebuild the zap
    // listener (Stop() + Start() its RelayClient + ZapListener) so the
    // new pubkey / gates take effect without a reboot. `nostrRelay` and
    // `nostrPubKey` are flagged boot_only in the schema and trigger the
    // generic rebootRequired response — they intentionally do NOT fire
    // this hook because tearing down the v2 WS data source mid-flight
    // is more disruptive than asking the user to reboot. Nullable: a
    // null callback defers all nostr changes to reboot.
    std::function<void()> on_nostr_changed;

    // Probes a catalogue `screens[].id` for runtime suppression. Returns
    // true when the currently-configured environment makes the screen
    // useless (e.g. mining-pool earnings on a solo pool that only reports
    // hashrate). The /api/settings GET handler drops hidden ids from the
    // emitted `screens[]` and POST /api/show/screen?s=<id> rejects them
    // with 409 so a stale WebUI button can't force the slot back on.
    // Nullable — a null hook keeps every catalogue entry visible.
    std::function<bool(int)> screen_is_hidden;

    // Maps a settings-catalog `screens[].id` (api_id: 0, 3, 4, 6, 10,
    // 20, 30, 40) to the dense rotation slot that ScreenManager tracks.
    // Wired in main.cpp; non-null when the rotation subsystem is up.
    // Returns a negative value when the api_id isn't in the current
    // rotation — the HTTP handler responds 400 in that case rather
    // than silently wrapping to slot 0.
    //
    // The inverse (slot -> api_id) is needed for /api/status's
    // `currentScreen` field so the WebUI can match it against the
    // picker's button ids.
    std::function<int(int)> api_id_to_slot;
    std::function<int(size_t)> slot_to_api_id;

    // Fires from HandleUploadFirmware after SendJson has flushed the
    // success body and before ScheduleReboot latches the timer. The
    // implementation is expected to play a brief "done" blink on the
    // NeoPixels (3× green) so the user sees a clear completion signal
    // even though the EPDs still carry the "UPDATE!" overlay. Blocks
    // the httpd worker thread for the blink duration (~1 s); the
    // reboot delay is widened to accommodate. Nullable — a null hook
    // skips the blink (matches pre-hook behaviour).
    std::function<void()> on_ota_completion_blink;
  };

  explicit ControlServer(Config cfg);
  ~ControlServer();

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // Start listening on port 80. Must be called after WiFi STA is up
  // (the server binds to all interfaces; calling it in AP mode would
  // collide with ProvisioningServer).
  esp_err_t Start();

  // Pop one pending command into `out`. Non-blocking. The main task
  // calls this once per event-loop iteration.
  bool TryPopCommand(ControlCommand* out);

  // Consume the most-recently-posted custom-cells payload that goes
  // with a `kShowCustom` command. Returns true iff a payload was
  // waiting; `*out` gets one string per panel (caller-sized view of the
  // board's panel count). Safe to call concurrently with the HTTP task.
  bool TakePendingCustomCells(std::vector<std::string>* out);

  // Snapshot of live status emitted by the main task so /api/status
  // responses reflect the actual screen state without poking the
  // ScreenManager from the HTTP task. Updated on every render; read
  // atomically under a small mutex.
  struct LiveStatus {
    int32_t current_slot = 0;
    int32_t slot_count = 1;
    bool timer_running = true;
    std::string currency;  // "" for block screen
    // Per-panel text mirror for /api/status `data[]`. Length should be
    // `num_screens` when populated. Empty means the first render hasn't
    // happened yet; HandleStatus falls back to empty strings then.
    std::vector<std::string> panel_texts;
  };
  void PublishStatus(const LiveStatus& status);

  // Attach an SSE server for live broadcasts. Non-owning — the caller
  // (main.cpp) keeps the SseServer alive for the ControlServer's
  // lifetime. When set, state-changing handlers push an updated
  // `status` event after applying their mutation, and callers can
  // force a broadcast with `BroadcastStatus()` after a DataHub
  // snapshot update lands.
  void AttachSse(SseServer* sse) { sse_ = sse; }

  // Emit the current status JSON as an SSE `status` event to every
  // connected client. Cheap no-op if no SSE server is attached or no
  // clients are connected. Safe to call from any task.
  void BroadcastStatus();

  // Replace the cached active-currency list at runtime. Used by the
  // on_screens_changed hook when PATCH /api/settings updates
  // `actCurrencies` so /api/show/currency stops 404'ing freshly-added
  // codes (the list had been a one-shot snapshot from the Config and
  // never refreshed). The status slot_count baseline also grows in
  // step so a status broadcast right after the PATCH carries the new
  // shape rather than the old.
  void SetCurrencies(std::vector<std::string> currencies);

 private:
  static esp_err_t TrampolineStatus(httpd_req_t* req);
  static esp_err_t TrampolineSystemStatus(httpd_req_t* req);
  static esp_err_t TrampolineFullRefresh(httpd_req_t* req);
  static esp_err_t TrampolineIdentify(httpd_req_t* req);
  static esp_err_t TrampolineRestart(httpd_req_t* req);
  static esp_err_t TrampolineShowScreen(httpd_req_t* req);
  static esp_err_t TrampolineShowCurrency(httpd_req_t* req);
  static esp_err_t TrampolineShowText(httpd_req_t* req);
  static esp_err_t TrampolineShowCustom(httpd_req_t* req);
  static esp_err_t TrampolineScreenNext(httpd_req_t* req);
  static esp_err_t TrampolineScreenPrev(httpd_req_t* req);
  static esp_err_t TrampolineStopDataSources(httpd_req_t* req);
  static esp_err_t TrampolineRestartDataSources(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightOn(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightOff(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightFlash(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightStatus(httpd_req_t* req);
  static esp_err_t TrampolineFrontlightBrightness(httpd_req_t* req);
  static esp_err_t TrampolineWifiTxPower(httpd_req_t* req);
  static esp_err_t TrampolineUploadWebui(httpd_req_t* req);
  static esp_err_t TrampolineLightsStatus(httpd_req_t* req);
  static esp_err_t TrampolineLightsColor(httpd_req_t* req);
  static esp_err_t TrampolineLightsOff(httpd_req_t* req);
  static esp_err_t TrampolineLightsSet(httpd_req_t* req);
  static esp_err_t TrampolineSettingsGet(httpd_req_t* req);
  static esp_err_t TrampolineSettingsPatch(httpd_req_t* req);
  static esp_err_t TrampolineDndStatus(httpd_req_t* req);
  static esp_err_t TrampolineDndEnable(httpd_req_t* req);
  static esp_err_t TrampolineDndDisable(httpd_req_t* req);
  static esp_err_t TrampolineActionPause(httpd_req_t* req);
  static esp_err_t TrampolineActionTimerRestart(httpd_req_t* req);
  static esp_err_t TrampolineFirmwareAutoUpdate(httpd_req_t* req);
  static esp_err_t TrampolineUploadFirmware(httpd_req_t* req);
  static esp_err_t TrampolineFactoryReset(httpd_req_t* req);
  static esp_err_t TrampolineNotImplemented(httpd_req_t* req);
  static esp_err_t TrampolineOptions(httpd_req_t* req);
  static esp_err_t TrampolineStatic(httpd_req_t* req);

  esp_err_t HandleStatus(httpd_req_t* req);
  esp_err_t HandleSystemStatus(httpd_req_t* req);
  esp_err_t HandleFullRefresh(httpd_req_t* req);
  esp_err_t HandleIdentify(httpd_req_t* req);
  esp_err_t HandleRestart(httpd_req_t* req);
  esp_err_t HandleShowScreen(httpd_req_t* req);
  esp_err_t HandleShowCurrency(httpd_req_t* req);
  esp_err_t HandleShowText(httpd_req_t* req);
  esp_err_t HandleShowCustom(httpd_req_t* req);
  esp_err_t HandleScreenNext(httpd_req_t* req);
  esp_err_t HandleScreenPrev(httpd_req_t* req);
  esp_err_t HandleStopDataSources(httpd_req_t* req);
  esp_err_t HandleRestartDataSources(httpd_req_t* req);
  esp_err_t HandleFrontlightOn(httpd_req_t* req);
  esp_err_t HandleFrontlightOff(httpd_req_t* req);
  esp_err_t HandleFrontlightFlash(httpd_req_t* req);
  esp_err_t HandleFrontlightStatus(httpd_req_t* req);
  esp_err_t HandleFrontlightBrightness(httpd_req_t* req);
  esp_err_t HandleWifiTxPower(httpd_req_t* req);
  esp_err_t HandleUploadWebui(httpd_req_t* req);
  esp_err_t HandleLightsStatus(httpd_req_t* req);
  esp_err_t HandleLightsColor(httpd_req_t* req);
  esp_err_t HandleLightsOff(httpd_req_t* req);
  esp_err_t HandleLightsSet(httpd_req_t* req);
  esp_err_t HandleSettingsGet(httpd_req_t* req);
  esp_err_t HandleSettingsPatch(httpd_req_t* req);
  esp_err_t HandleDndStatus(httpd_req_t* req);
  esp_err_t HandleDndEnable(httpd_req_t* req);
  esp_err_t HandleDndDisable(httpd_req_t* req);
  esp_err_t HandleActionPause(httpd_req_t* req);
  esp_err_t HandleActionTimerRestart(httpd_req_t* req);
  esp_err_t HandleFirmwareAutoUpdate(httpd_req_t* req);
  esp_err_t HandleUploadFirmware(httpd_req_t* req);
  esp_err_t HandleFactoryReset(httpd_req_t* req);
  esp_err_t HandleStatic(httpd_req_t* req);

  bool PostCommand(const ControlCommand& cmd);
  static void ApplyCors(httpd_req_t* req);

  // Render the current /api/status JSON as a string. Single source of
  // truth — both the REST handler and SSE broadcasts use this so a
  // new field added to one automatically surfaces on the other.
  // Empty string on OOM, which the caller surfaces as a 500.
  std::string BuildStatusJson() const;

  Config cfg_;
  httpd_handle_t server_ = nullptr;
  QueueHandle_t cmd_queue_ = nullptr;
  SseServer* sse_ = nullptr;  // non-owning; nullable

  // Live-status snapshot. Lightweight — one int + short string — so a
  // single std::mutex is more than enough.
  mutable std::mutex status_mu_;
  LiveStatus status_;

  // Side-channel payload for /api/show/text and /api/show/custom —
  // strings don't fit in the trivially-copyable ControlCommand struct
  // that goes through the FreeRTOS queue. Protected by its own mutex
  // rather than sharing `status_mu_` so a slow Render() on the main
  // task can't block an incoming HTTP handler. "Latest wins": if two
  // requests land before main consumes the pending slot, the later
  // payload overwrites the earlier — matches the old firmware's
  // EPDManager::setContent which also overwrites unconditionally.
  mutable std::mutex pending_custom_mu_;
  bool pending_custom_valid_ = false;
  std::vector<std::string> pending_custom_cells_;
};

}  // namespace btclock
