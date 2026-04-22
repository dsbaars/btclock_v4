#include "provisioning_server.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "esp_app_desc.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "portal";

// Gzipped at build time — served with Content-Encoding: gzip.
extern "C" const uint8_t kPortalHtmlGzStart[] asm(
    "_binary_portal_html_gz_start");
extern "C" const uint8_t kPortalHtmlGzEnd[] asm("_binary_portal_html_gz_end");

std::string UrlDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < s.size()) {
      auto hex = [](char x) -> int {
        if (x >= '0' && x <= '9') return x - '0';
        if (x >= 'a' && x <= 'f') return x - 'a' + 10;
        if (x >= 'A' && x <= 'F') return x - 'A' + 10;
        return -1;
      };
      const int hi = hex(s[i + 1]);
      const int lo = hex(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(c);
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string FormField(const std::string& body, const std::string& key) {
  const std::string needle = key + "=";
  size_t pos = 0;
  while (pos < body.size()) {
    const size_t eq = body.find(needle, pos);
    if (eq == std::string::npos) return "";
    if (eq != 0 && body[eq - 1] != '&') {
      pos = eq + 1;
      continue;
    }
    const size_t vstart = eq + needle.size();
    const size_t vend = body.find('&', vstart);
    return UrlDecode(body.substr(vstart,
                                 vend == std::string::npos ? std::string::npos
                                                           : vend - vstart));
  }
  return "";
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x",
                        static_cast<unsigned>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

}  // namespace

ProvisioningServer::ProvisioningServer(Wifi& wifi, const char* hw_name,
                                       SaveCallback on_save)
    : wifi_(&wifi),
      hw_name_(hw_name ? hw_name : ""),
      on_save_(std::move(on_save)) {}

ProvisioningServer::~ProvisioningServer() {
  if (server_) httpd_stop(server_);
}

esp_err_t ProvisioningServer::Start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.max_uri_handlers = 8;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.stack_size = 6144;
  cfg.lru_purge_enable = true;

  esp_err_t err = httpd_start(&server_, &cfg);
  if (err != ESP_OK) return err;

  const httpd_uri_t portal = {
      .uri = "/", .method = HTTP_GET, .handler = HandlePortal, .user_ctx = this};
  const httpd_uri_t scan = {.uri = "/api/scan",
                            .method = HTTP_GET,
                            .handler = HandleScan,
                            .user_ctx = this};
  const httpd_uri_t version = {.uri = "/api/version",
                               .method = HTTP_GET,
                               .handler = HandleVersion,
                               .user_ctx = this};
  const httpd_uri_t wifi = {.uri = "/api/wifi",
                            .method = HTTP_POST,
                            .handler = HandleWifi,
                            .user_ctx = this};
  const httpd_uri_t any = {.uri = "/*",
                           .method = HTTP_GET,
                           .handler = HandleAny,
                           .user_ctx = this};
  httpd_register_uri_handler(server_, &portal);
  httpd_register_uri_handler(server_, &scan);
  httpd_register_uri_handler(server_, &version);
  httpd_register_uri_handler(server_, &wifi);
  httpd_register_uri_handler(server_, &any);

  ESP_LOGI(kTag, "provisioning portal listening on http://%s/",
           wifi_->ap_ip().c_str());
  return ESP_OK;
}

esp_err_t ProvisioningServer::HandlePortal(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const size_t len =
      static_cast<size_t>(kPortalHtmlGzEnd - kPortalHtmlGzStart);
  return httpd_resp_send(req,
                         reinterpret_cast<const char*>(kPortalHtmlGzStart),
                         len);
}

esp_err_t ProvisioningServer::HandleScan(httpd_req_t* req) {
  auto* self = static_cast<ProvisioningServer*>(req->user_ctx);
  const auto nets = self->wifi_->Scan();

  std::string body = "[";
  bool first = true;
  for (const auto& n : nets) {
    if (!first) body += ",";
    first = false;
    body += "{\"ssid\":\"";
    body += JsonEscape(n.ssid);
    body += "\",\"rssi\":";
    body += std::to_string(static_cast<int>(n.rssi));
    body += ",\"secured\":";
    body += n.secured ? "true" : "false";
    body += "}";
  }
  body += "]";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t ProvisioningServer::HandleVersion(httpd_req_t* req) {
  auto* self = static_cast<ProvisioningServer*>(req->user_ctx);
  const esp_app_desc_t* app = esp_app_get_description();

  std::string body = "{\"hw\":\"";
  body += JsonEscape(self->hw_name_);
  body += "\",\"version\":\"";
  body += JsonEscape(app ? app->version : "");
  body += "\",\"built\":\"";
  if (app) {
    body += JsonEscape(std::string(app->date) + " " + app->time);
  }
  body += "\",\"idf\":\"";
  body += JsonEscape(app ? app->idf_ver : "");
  body += "\"}";

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t ProvisioningServer::HandleWifi(httpd_req_t* req) {
  auto* self = static_cast<ProvisioningServer*>(req->user_ctx);
  const int remaining = req->content_len;
  if (remaining <= 0 || remaining > 512) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    return ESP_FAIL;
  }
  std::string body;
  body.resize(remaining);
  int received = 0;
  while (received < remaining) {
    const int r = httpd_req_recv(req, body.data() + received,
                                 remaining - received);
    if (r <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv");
      return ESP_FAIL;
    }
    received += r;
  }

  const std::string ssid = FormField(body, "ssid");
  const std::string pw = FormField(body, "pw");
  if (ssid.empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    return ESP_FAIL;
  }

  ESP_LOGI(kTag, "credentials received: ssid='%s' pw=%d chars",
           ssid.c_str(), static_cast<int>(pw.size()));
  if (self->on_save_) self->on_save_(ssid, pw);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true}", 11);
}

esp_err_t ProvisioningServer::HandleAny(httpd_req_t* req) {
  // Captive-portal bounce. 302 any unknown path to "/" so Android/iOS
  // "connectivity check" URLs land on the portal.
  auto* self = static_cast<ProvisioningServer*>(req->user_ctx);
  const std::string location = "http://" + self->wifi_->ap_ip() + "/";
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", location.c_str());
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "portal", 6);
}

}  // namespace btclock
