// SPDX-License-Identifier: MIT
//
// Captive DNS for DFU32's access point (SPEC.md §9.5): every A question gets
// 192.168.4.1, so a phone that joins opens the web UI by itself. Private to
// components/net; net.cpp starts it on AP start and stops it on AP stop.
#pragma once

namespace dfu32::net {

/// Binds a raw lwIP udp_pcb to 192.168.4.1:53 (in the tcpip thread). No socket
/// (LWIP_MAX_SOCKETS stays 10) and no task. True when listening (also when it
/// already was).
bool captive_dns_start();
/// Removes the pcb; a no-op when it isn't running.
void captive_dns_stop();

}  // namespace dfu32::net
