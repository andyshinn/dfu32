// SPDX-License-Identifier: MIT
#include "captive_dns.hpp"

#include <cstdint>
#include <cstring>

#include "dfu32_core/ap.hpp"
#include "dfu32_core/captive.hpp"
#include "esp_netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

namespace dfu32::net {
namespace {

constexpr u16_t kDnsPort = 53;

// Touched only in the tcpip thread: the start/stop callbacks run there through
// esp_netif_tcpip_exec() (TCP/IP core locking is off), and so does on_query.
udp_pcb* s_pcb = nullptr;
// Two static buffers instead of a task stack (SPEC.md §9.5).
uint8_t s_in[captive::kDnsMaxPacket];
uint8_t s_out[captive::kDnsMaxPacket];

void on_query(void*, udp_pcb* pcb, pbuf* p, const ip_addr_t* addr, u16_t port) {
    if (p->tot_len > sizeof(s_in)) {  // oversized: dropped
        pbuf_free(p);
        return;
    }
    const u16_t len = pbuf_copy_partial(p, s_in, p->tot_len, 0);
    pbuf_free(p);
    // The parsing and the answer are pure and fuzzed (dfu32_core/captive.hpp);
    // 0 = drop (responses, other opcodes, malformed names, ...).
    const size_t n = captive::dns_answer(s_in, len, ap::kIp, s_out, sizeof(s_out));
    if (n == 0) return;
    pbuf* q = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(n), PBUF_RAM);
    if (q == nullptr) return;
    std::memcpy(q->payload, s_out, n);
    udp_sendto(pcb, q, addr, port);
    pbuf_free(q);
}

esp_err_t start_in_tcpip(void*) {
    if (s_pcb != nullptr) return ESP_OK;
    udp_pcb* pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == nullptr) return ESP_ERR_NO_MEM;
    // Bound to the AP address, so LAN clients on the station side are never answered.
    const ip_addr_t local = IPADDR4_INIT_BYTES(ap::kIp[0], ap::kIp[1], ap::kIp[2], ap::kIp[3]);
    if (udp_bind(pcb, &local, kDnsPort) != ERR_OK) {
        udp_remove(pcb);
        return ESP_FAIL;
    }
    udp_recv(pcb, &on_query, nullptr);
    s_pcb = pcb;
    return ESP_OK;
}

esp_err_t stop_in_tcpip(void*) {
    if (s_pcb != nullptr) udp_remove(s_pcb);
    s_pcb = nullptr;
    return ESP_OK;
}

}  // namespace

bool captive_dns_start() { return esp_netif_tcpip_exec(&start_in_tcpip, nullptr) == ESP_OK; }

void captive_dns_stop() { esp_netif_tcpip_exec(&stop_in_tcpip, nullptr); }

}  // namespace dfu32::net
