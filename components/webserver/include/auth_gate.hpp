// HTTP Basic auth gate for the control API.
//
// Spec (data/static/swagger.yml): when `httpAuthEnabled` is true, every
// /api/* endpoint — including SSE — requires a matching `Authorization:
// Basic ...` header. Factory default is `httpAuthEnabled=false`, in
// which case the gate is a no-op and every request passes through.
//
// This header is split into two surfaces:
//
//   1. `RequireHttpAuth(httpd_req_t*)` — full IDF path. Reads settings
//      from NVS itself, inspects the request header, sends 401 on
//      failure, returns true on pass-through.
//
//   2. Pure-logic helpers in `auth_gate_logic.hpp` — host-testable
//      without ESP-IDF. Used internally by the gate and driven directly
//      by the host test suite.
//
// Lockout-avoidance: if `httpAuthEnabled` is true but `httpAuthPass` is
// empty, the gate logs a `W` and passes through. The WebUI should
// prevent that state, but if it happens the user needs a recovery path
// that isn't a factory reset.

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

namespace btclock {

// Returns true when the request is authorised (or auth is disabled) and
// the caller should proceed with its handler body. Returns false when
// the gate itself has already sent a 401 response — the caller must
// return `ESP_OK` immediately without writing anything further to the
// request.
//
// Reads `httpAuthEnabled`/`httpAuthUser`/`httpAuthPass` from the
// settings NVS namespace on every call. Cheap enough — NVS reads hit a
// warm cache — and means runtime toggles from PATCH /api/settings take
// effect on the next request without having to refresh a snapshot.
bool RequireHttpAuth(httpd_req_t* req);

// Gate for the three OTA upload routes (/api/firmware/auto_update,
// /upload/firmware, /upload/webui). Mirrors the old Arduino firmware's
// behaviour: when `otaEnabled=false` the routes refuse new uploads
// entirely rather than silently accepting bytes the device will reject
// later. Returns true to proceed, false after sending a 403.
//
// Default is true — the field is documented so a user can opt out of
// remote updates without having to add a firewall rule. Intended to be
// called after RequireHttpAuth so unauthenticated callers see 401 first.
bool RequireOtaEnabled(httpd_req_t* req);

}  // namespace btclock
