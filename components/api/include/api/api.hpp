// SPDX-License-Identifier: MIT
//
// HTTP API (SPEC.md §10.1) on esp_http_server, port 80. JSON everywhere;
// bearer-token auth with claim-on-first-use (§9.3). Route matching, request
// parsing and auth helpers are pure (dfu32_core/api_core.hpp).
#pragma once

#include <string>

namespace dfu32::api {

/// Starts the server and announces it over mDNS. Call after net::init() and
/// job::init(). Returns false if the server can't start.
bool start();

/// Issues a new API token (replacing any old one) and returns it; "" on
/// failure. For the console's `token` command while there is no button flow.
std::string rotate_token();

/// Back to unclaimed: the next POST /api/v1/claim issues a token.
bool unclaim();

}  // namespace dfu32::api
