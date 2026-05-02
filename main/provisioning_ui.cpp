#include "provisioning_ui.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include "board/board.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "net_util.hpp"
#include "qrcodegen.h"

namespace btclock {
namespace {
constexpr const char* kTag = "prov-ui";

constexpr const char* kRef = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

template <size_t N>
LandscapeFb PrepFb(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                   uint8_t (&fb_storage)[N][16 * 296], int i) {
  LandscapeFb lfb = {};
  lfb.native_fb = fb_storage[i];
  lfb.native_stride = panels[i]->kStride;
  lfb.native_width = panels[i]->Width();
  lfb.native_height = panels[i]->Height();
  lfb.rotation = Rotation::k180;
  return lfb;
}

void DrawQr(LandscapeFb& lfb, const char* text) {
  static uint8_t qr_buf[qrcodegen_BUFFER_LEN_MAX];
  static uint8_t qr_temp[qrcodegen_BUFFER_LEN_MAX];
  if (!qrcodegen_encodeText(text, qr_temp, qr_buf, qrcodegen_Ecc_MEDIUM,
                            qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                            qrcodegen_Mask_AUTO, true)) {
    ESP_LOGW(kTag, "qr encode failed: %s", text);
    return;
  }
  const int size = qrcodegen_getSize(qr_buf);
  struct Ctx {
    const uint8_t* buf;
  };
  Ctx ctx{qr_buf};
  auto get = [](int x, int y, const void* c) {
    return qrcodegen_getModule(static_cast<const Ctx*>(c)->buf, x, y);
  };
  DrawQrCode(lfb, lfb.native_width, lfb.native_height, size, get, &ctx,
             /*white_bg=*/true);
}

// Resolved from the active board header.
constexpr const char* kHwName = board::kHardwareName;

// Wrap a `git describe --tags --always --dirty --match "[0-9]*"` string
// across newlines so DrawMarkdown's panel-wide auto-fit isn't dragged
// down by one wide row (snapshot builds emit e.g.
// "4.0.0-beta.6-7-gd3ace07-dirty" — at the panel's 18 px request that
// shrinks the entire panel by ~50% to make the line fit). Splits are
// chosen at the well-defined `-g<hash>` boundary and the `-dirty`
// suffix, both of which `git describe --long` always emits at the
// same position.
std::string WrapGitDescribe(const char* v) {
  std::string s(v ? v : "");
  bool dirty = false;
  if (auto pos = s.rfind("-dirty"); pos != std::string::npos) {
    s.erase(pos);
    dirty = true;
  }
  std::string out;
  if (auto pos = s.find("-g"); pos != std::string::npos) {
    out = s.substr(0, pos);
    out += '\n';
    out += s.substr(pos + 1);  // skip the leading '-' for visual cleanliness
  } else {
    out = s;
  }
  if (dirty) {
    out += "\n-dirty";
  }
  return out;
}

}  // namespace

template <size_t N>
void RenderProvisioningScreen(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                              uint8_t (&fb_storage)[N][16 * 296],
                              const AppFonts& fonts, const std::string& ap_ssid,
                              const std::string& ap_pw) {
  static_assert(N >= 7, "provisioning layout needs at least 7 panels");

  const Font& reg = fonts.atkinson();
  const Font& bold = fonts.atkinson_bold();

  constexpr float kInstrPx = 20.0f;
  constexpr float kInfoPx = 18.0f;

  // P0 — Welcome! (auto-fit to avoid '!' clip)
  {
    auto lfb = PrepFb(panels, fb_storage, 0);
    ClearFb(lfb, /*white=*/true);
    const float px =
        FitTextPx("Welcome!", bold, 32.0f, 14.0f, lfb.native_width - 6);
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, "Welcome!", kRef,
                     bold, px, /*white_text=*/false);
  }
  // P1 — Bienvenidos!
  {
    auto lfb = PrepFb(panels, fb_storage, 1);
    ClearFb(lfb, true);
    const float px =
        FitTextPx("Bienvenidos!", bold, 28.0f, 12.0f, lfb.native_width - 6);
    DrawTextCentered(lfb, lfb.native_width, lfb.native_height, "Bienvenidos!",
                     kRef, bold, px, false);
  }
  // P2 — EN instructions
  {
    auto lfb = PrepFb(panels, fb_storage, 2);
    ClearFb(lfb, true);
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height,
                 "To setup\nscan QR or\nconnect\nmanually", reg, bold, kInstrPx,
                 false);
  }
  // P3 — ES instructions (same px as EN)
  {
    auto lfb = PrepFb(panels, fb_storage, 3);
    ClearFb(lfb, true);
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height,
                 "Para\nconfigurar\nescanear QR\no conectar\nmanualmente", reg,
                 bold, kInstrPx, false);
  }
  // P4 — SSID / Password / Hostname
  {
    auto lfb = PrepFb(panels, fb_storage, 4);
    ClearFb(lfb, true);
    char body[256];
    std::snprintf(body, sizeof(body),
                  "*SSID:*\n%s\n\n*Password:*\n%s\n\n*Hostname:*\n%s",
                  ap_ssid.c_str(), ap_pw.c_str(), ap_ssid.c_str());
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kInfoPx, false);
  }
  // P5 — HW / SW / Built
  {
    auto lfb = PrepFb(panels, fb_storage, 5);
    ClearFb(lfb, true);
    // esp_app_desc_t::version is populated from PROJECT_VER, which our
    // top-level CMakeLists derives from `git describe --tags --always
    // --dirty --match "[0-9]*"`: "X.Y.Z" on a tagged release, the short
    // commit hash on a snapshot build, "-dirty" suffix when there are
    // uncommitted edits at build time.
    const esp_app_desc_t* app = esp_app_get_description();
    const char* fw_ver = (app && app->version[0]) ? app->version : "unknown";
    const std::string fw_ver_wrapped = WrapGitDescribe(fw_ver);
    char body[256];
    std::snprintf(body, sizeof(body),
                  "*HW:*\n%s\n2.13\"\n\n*SW:*\nBTClock v4\n%s\n\n*Built:*\n%s",
                  kHwName, fw_ver_wrapped.c_str(), __DATE__);
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height, body, reg, bold,
                 kInfoPx, false);
  }
  // P6 — WiFi join QR
  {
    auto lfb = PrepFb(panels, fb_storage, 6);
    ClearFb(lfb, true);
    const std::string qr_text = FormatWifiQr(ap_ssid, ap_pw);
    DrawQr(lfb, qr_text.c_str());
  }
  // P7 (V8 only) — companion URL hint so the 8th panel isn't blank.
  if constexpr (N >= 8) {
    auto lfb = PrepFb(panels, fb_storage, 7);
    ClearFb(lfb, true);
    DrawMarkdown(lfb, lfb.native_width, lfb.native_height,
                 "*Web:*\nhttp://\n192.168.\n4.1", reg, bold, kInfoPx, false);
  }

  // Parallel full-refresh of every panel.
  for (size_t i = 0; i < N; ++i) {
    panels[i]->DrawFramebufferStart(fb_storage[i], RefreshKind::kFull);
  }
  for (size_t i = 0; i < N; ++i) {
    panels[i]->WaitForRefresh();
  }
}

// Explicit instantiations — one per panel count we support.
template void RenderProvisioningScreen<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const std::string&, const std::string&);
template void RenderProvisioningScreen<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const std::string&, const std::string&);

}  // namespace btclock
