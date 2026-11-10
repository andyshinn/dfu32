// SPDX-License-Identifier: MIT
//
// HTTP API (SPEC.md §10.1) on esp_http_server, port 80. JSON everywhere;
// optional bearer-token auth (§9.3): open until a token is set. Route
// matching, request parsing and auth helpers are pure (dfu32_core/api_core.hpp).
#pragma once

#include <string>

namespace dfu32::api {

/// Starts the server and announces it over mDNS. Call after net::init() and
/// job::init(). Returns false if the server can't start.
bool start();

/// Issues a new API token (replacing any old one, and turning token auth on)
/// and returns it; "" on failure. For the console's `token` command.
std::string rotate_token();

/// Turns token auth off (back to an open API); POST /api/v1/claim turns it
/// on again with a new token.
bool unclaim();

}  // namespace dfu32::api
