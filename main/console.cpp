// SPDX-License-Identifier: MIT
//
// Bench console (USB serial): target, wifi, check, stage, scan, staged, probe,
// flash, runs, run, logs, blreset. The HTTP API (M3b) and web UI (M4) cover the
// same ground; the console stays as a debug tool.
#include "console.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "config/config.hpp"
#include "dfu_ble/dfu_ble.hpp"
#include "fetch/fetch.hpp"
#include "net/net.hpp"
#include "dfu_proto/protocol.hpp"
#include "dfu_proto/retry.hpp"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "runlog/run_record.hpp"
#include "runlog/runlog_idf.hpp"
#include "staging/staging.hpp"

namespace dfu32 {
namespace {

constexpr const char* TAG = "console";

// ---- helpers ------------------------------------------------------------------

bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

long opt_long(int argc, char** argv, const char* flag, long def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return std::strtol(argv[i + 1], nullptr, 0);
    }
    return def;
}

bool load_target(ble::Mac& out) {
    TargetConfig t;
    return config::load_target(t) && !t.mac.empty() && ble::Mac::parse(t.mac, t.addr_type, out);
}

bool require_target(ble::Mac& mac) {
    if (load_target(mac)) return true;
    std::printf("No target MAC set. Use: target <MAC> [random|public]\n"
                "(MeshCore prints the MAC in its `start ota` reply.)\n");
    return false;
}

void hex(const uint8_t* p, size_t n, char* out) {
    for (size_t i = 0; i < n; ++i) std::sprintf(out + 2 * i, "%02x", p[i]);
}

void print_link(const ble::LinkInfo& l) {
    std::printf("  reached at        %s%s%s\n", l.address.str().c_str(), l.found_at_plus_one ? " (MAC+1)" : "",
                l.via_buttonless ? " after buttonless jump" : " (already in bootloader)");
    std::printf("  DFU Version char  0x%04x\n", l.version_char);
    std::printf("  DIS firmware rev  \"%s\"\n", l.dis_fw_rev.c_str());
    std::printf("  DIS model         \"%s\"\n", l.dis_model.c_str());
    std::printf("  bootloader class  %s", dfu::to_string(l.bootloader.cls));
    if (l.bootloader.sd_fwid) {
        std::printf("  (%s %s, FWID 0x%04X)", l.bootloader.sd_variant.c_str(), l.bootloader.sd_version.c_str(),
                    l.bootloader.sd_fwid);
    }
    std::printf("\n");
    if (l.bootloader.device_type) {
        std::printf("  .dat device type  0x%04X required\n", l.bootloader.device_type);
    }
    std::printf("  ATT MTU           %u\n", l.mtu);
    std::printf("  packet size       %u bytes%s\n", l.packet_size,
                l.packet_size >= 244 ? "" : l.packet_size == 20 ? "  (legacy 20-byte mode)" : "");
    std::printf("  PHY tx/rx         %u/%u  (1 = 1M, 2 = 2M)\n", l.tx_phy, l.rx_phy);
    if (l.conn_interval) {
        const unsigned hundredths = l.conn_interval * 125u;  // 1.25 ms units -> 0.01 ms
        std::printf("  conn interval     %u.%02u ms, latency %u, supervision %u ms\n", hundredths / 100,
                    hundredths % 100, l.conn_latency, l.conn_timeout * 10u);
    }
    std::printf("  timings           find %" PRIu32 " ms, jump %" PRIu32 " ms, connect %" PRIu32 " ms\n", l.find_ms,
                l.jump_ms, l.connect_ms);
}

uint32_t uptime_s() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

uint32_t stream_rate_x10(const dfu::TransferResult& t) {
    const uint32_t ms = t.phase_ms[static_cast<size_t>(dfu::TransferPhase::Stream)];
    return ms ? static_cast<uint32_t>(uint64_t{t.bytes_sent} * 10 / ms) : 0;
}

runlog::RunRecord make_record(uint32_t seq, const ble::Mac& mac, const StagedImage& img, const ble::FlashReport& r,
                              uint32_t started_s, bool wifi_off) {
    runlog::RunRecord rec;
    rec.seq = seq;
    rec.result = r.ok ? "SUCCEEDED" : "FAILED";
    rec.error = r.error;
    rec.failure_kind = dfu::to_string(r.failure);
    rec.target = mac.str();
    rec.image_version = img.header().version;
    char sha[65] = {};
    hex(img.header().bin_sha256.data(), 32, sha);
    rec.image_sha256 = sha;
    rec.image_size = img.header().bin_size;
    rec.started_uptime_s = started_s;
    rec.duration_ms = r.total_ms;
    bool any_transfer = false;
    for (const auto& a : r.attempts) any_transfer = any_transfer || a.ran_transfer;
    if (any_transfer) rec.last_phase = dfu::to_string(r.transfer.phase);
    rec.bytes_acked = r.transfer.bytes_acked;
    rec.target_erased = r.target_erased ? "yes" : "no";
    rec.sys_reset_after_failure = r.sys_reset_after_failure;
    rec.wifi_off = wifi_off;
    rec.bootloader = r.link.dis_fw_rev;
    rec.bootloader_class = dfu::to_string(r.link.bootloader.cls);
    rec.mtu = r.link.mtu;
    rec.packet_size = r.link.packet_size;
    rec.conn_interval_x125 = r.link.conn_interval;
    rec.rate_x10_kBps = stream_rate_x10(r.transfer);
    for (const auto& a : r.attempts) {
        runlog::RunAttempt ra;
        ra.number = a.number;
        ra.interval_ms = a.params.interval_ms;
        ra.prn = a.params.prn;
        ra.outcome = a.failure == dfu::FailureKind::None ? "OK" : a.error;
        ra.kind = dfu::to_string(a.failure);
        ra.bytes_acked = a.transfer.bytes_acked;
        rec.attempts.push_back(std::move(ra));
    }
    return rec;
}

// ---- commands -----------------------------------------------------------------

void print_target(const TargetConfig& t) {
    std::printf("  id         %s%s\n", t.effective_id().c_str(), t.id.empty() ? " (default)" : "");
    std::printf("  mac        %s", t.mac.empty() ? "(not set)" : t.mac.c_str());
    ble::Mac mac;
    if (!t.mac.empty() && ble::Mac::parse(t.mac, t.addr_type, mac)) {
        std::printf(" (%s), bootloader after reset at %s or %s", t.addr_type ? "random" : "public", mac.str().c_str(),
                    mac.plus_one().str().c_str());
    }
    std::printf("\n  board      %s\n", t.board.empty() ? "(not set: needed for `check`/`stage`)" : t.board.c_str());
    std::printf("  manifest   %s\n", t.manifest_url.empty() ? "(not set)" : t.manifest_url.c_str());
    if (t.max_app_size) {
        std::printf("  max app    %" PRIu32 " B\n", t.max_app_size);
    } else {
        std::printf("  max app    default (798720 B for nRF52, none for nRF54)\n");
    }
}

int cmd_target(int argc, char** argv) {
    TargetConfig t;
    if (!config::load_target(t)) {
        std::printf("Failed to read settings from NVS\n");
        return 1;
    }
    if (argc >= 2) {
        const std::string field = argv[1];
        const std::string value = argc >= 3 ? argv[2] : "";
        std::string err;
        if (field == "board") {
            err = validate_board(value);
            t.board = value;
        } else if (field == "url") {
            err = validate_manifest_url(value);
            t.manifest_url = value;
        } else if (field == "id") {
            err = validate_target_id(value);
            t.id = value;
        } else if (field == "maxapp") {
            uint32_t bytes = 0;
            if (!parse_u32(value, bytes)) {
                std::printf("Not saved: '%s' is not a number of bytes (e.g. 802816 or 0xC4000; 0 = default)\n",
                            value.c_str());
                return 1;
            }
            err = validate_max_app_size(bytes);
            t.max_app_size = bytes;
        } else if (field == "clear") {
            if (!config::erase_target()) {
                std::printf("Failed to clear the target\n");
                return 1;
            }
            std::printf("Target cleared.\n");
            return 0;
        } else {
            // target <MAC> [random|public]
            const std::string mac = normalize_mac(field);
            if (mac.empty()) {
                std::printf("Bad MAC or field '%s'. Use: target <MAC> [random|public] | board <name> | url <manifest-url>"
                            " | id <slug> | maxapp <bytes> | clear\n",
                            argv[1]);
                return 1;
            }
            t.mac = mac;
            t.addr_type = (argc >= 3 && std::strcmp(argv[2], "public") == 0) ? 0 : 1;
        }
        if (!err.empty()) {
            std::printf("Not saved: %s\n", err.c_str());
            return 1;
        }
        if (!config::save_target(t)) {
            std::printf("Failed to save target to NVS\n");
            return 1;
        }
    }
    print_target(t);
    return 0;
}

// ---- networking (M3) -------------------------------------------------------------

int cmd_wifi(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "forget") == 0) {
        const bool ok = net::forget();
        std::printf("%s\n", ok ? "WiFi credentials cleared." : "Failed to clear WiFi credentials.");
        return ok ? 0 : 1;
    }
    if (argc >= 2) {
        const std::string ssid = argv[1];
        const std::string pass = argc >= 3 ? argv[2] : "";
        if (ssid.size() > 32 || pass.size() > 64 || (!pass.empty() && pass.size() < 8)) {
            std::printf("SSID must be 1-32 characters and the password 8-64 (or none for an open network).\n");
            return 1;
        }
        if (!net::set_credentials(ssid, pass)) {
            std::printf("Failed to store credentials.\n");
            return 1;
        }
        std::printf("Connecting to \"%s\"...\n", ssid.c_str());
        std::fflush(stdout);
        if (!net::wait_connected(20000)) std::printf("Not connected yet; it keeps retrying in the background.\n");
    }
    const net::Status st = net::status();
    std::printf("  hostname   %s (http://%s.local/)\n", st.hostname.c_str(), st.hostname.c_str());
    std::printf("  network    %s\n", st.provisioned ? st.ssid.c_str() : "(none: wifi <ssid> [password])");
    if (st.connected) {
        std::printf("  connected  yes, IP %s, RSSI %d dBm\n", st.ip.c_str(), st.rssi);
    } else {
        std::printf("  connected  no%s", st.stopped ? " (stopped for a transfer)" : "");
        if (st.last_disconnect_reason) std::printf(", last disconnect reason %u", st.last_disconnect_reason);
        std::printf("\n");
    }
    std::printf("  reconnects %" PRIu32 "\n", st.reconnects);
    std::printf("  clock      %s\n", st.time_synced ? "synced (SNTP)" : "not synced");
    return 0;
}

bool require_network() {
    if (net::status().connected) return true;
    std::printf("WiFi is not connected (see `wifi`).\n");
    return false;
}

int cmd_check(int argc, char** argv) {
    TargetConfig t;
    config::load_target(t);
    const std::string url = argc >= 2 ? argv[1] : t.manifest_url;
    if (url.empty()) {
        std::printf("No manifest URL. Use: check <url>, or set one with `target url <url>`.\n");
        return 1;
    }
    if (!require_network()) return 1;
    fetch::CheckReport r;
    const Status s = fetch::check(url, t.board, r);
    if (!s.ok()) {
        std::printf("CHECK FAILED: %s\n", s.str().c_str());
        return 1;
    }
    const Manifest& m = r.manifest;
    std::printf("  version    %s%s%s\n", m.version.c_str(), m.name.empty() ? "" : "  (", m.name.empty() ? "" : (m.name + ")").c_str());
    std::string boards;
    for (const auto& b : m.boards) boards += (boards.empty() ? "" : ", ") + b;
    std::printf("  boards     %s  -> target board \"%s\": %s\n", boards.c_str(), t.board.c_str(),
                t.board.empty() ? "NOT SET" : r.board_ok ? "match" : "MISMATCH");
    std::printf("  bin        %" PRIu32 " B  sha256 %s\n             %s\n", m.bin.size, sha256_hex(m.bin.sha256).c_str(),
                m.bin.url.c_str());
    std::printf("  dat        %" PRIu32 " B  %s\n", m.dat.size, m.dat.url.c_str());
    if (!m.created.empty()) std::printf("  created    %s\n", m.created.c_str());
    if (r.staged_valid) {
        std::printf("  staged     \"%s\": %s\n", r.staged.version.c_str(),
                    r.staged_same ? "same image (stage/flash will reuse it)" : "different image");
    } else {
        std::printf("  staged     nothing\n");
    }
    return r.board_ok ? 0 : 1;
}

int cmd_stage(int argc, char** argv) {
    TargetConfig t;
    config::load_target(t);
    std::string url;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') url = argv[i];
    }
    if (url.empty() && t.manifest_url.empty()) {
        std::printf("No manifest URL. Use: stage <url>, or set one with `target url <url>`.\n");
        return 1;
    }
    if (t.board.empty()) {
        std::printf("Set the target's board first (`target board <name>`, matched against manifest \"boards\").\n");
        return 1;
    }
    if (!require_network()) return 1;
    fetch::StageOptions o;
    o.force = has_flag(argc, argv, "--force");
    o.staged_at = net::unix_time();
    fetch::StageReport r;
    const Status s = fetch::stage(t, url, o, r);
    if (!s.ok()) {
        std::printf("STAGE FAILED: %s\n", s.str().c_str());
        if (r.invalidated) std::printf("The previous staged image was invalidated; stage again once this is fixed.\n");
        return 1;
    }
    if (r.reused) {
        std::printf("STAGE OK: \"%s\" was already staged and re-verified (%" PRIu32 " ms). Use --force to download "
                    "anyway.\n",
                    r.manifest.version.c_str(), r.ms);
    } else {
        std::printf("STAGE OK: \"%s\", %" PRIu32 " B in %" PRIu32 ".%" PRIu32 " s, verified. `flash --yes` when "
                    "ready.\n",
                    r.manifest.version.c_str(), r.bytes, r.ms / 1000, (r.ms % 1000) / 100);
    }
    return 0;
}

int cmd_scan(int argc, char** argv) {
    const long secs = argc >= 2 ? std::strtol(argv[1], nullptr, 0) : 8;
    std::printf("Scanning %ld s...\n", secs);
    const auto list = ble::scan(static_cast<uint32_t>(secs > 0 ? secs : 8) * 1000);
    std::printf("%-17s  %-6s  %4s  %-4s  %s\n", "ADDRESS", "TYPE", "RSSI", "DFU", "NAME");
    for (const auto& a : list) {
        std::printf("%-17s  %-6s  %4d  %-4s  %s\n", a.mac.str().c_str(), a.mac.type ? "random" : "public", a.rssi,
                    a.dfu_service ? "yes" : "", a.name.c_str());
    }
    std::printf("%u device(s). MeshCore repeaters only advertise after `start ota` (name like RAK4631_OTA).\n"
                "Set one with: target <ADDRESS> [random|public]\n",
                static_cast<unsigned>(list.size()));
    return 0;
}

int cmd_staged(int, char**) {
    StagedImage img;
    if (!img.open()) {
        std::printf("No staging partition.\n");
        return 1;
    }
    if (img.header_status() != dfu::StagingHeaderError::Ok) {
        std::printf("Nothing staged (%s). Use `stage`, or write one with tools/dfu32_stage_image.py.\n",
                    dfu::to_string(img.header_status()));
        return 1;
    }
    const auto& h = img.header();
    char sha[65] = {};
    hex(h.bin_sha256.data(), 32, sha);
    std::printf("  bin        %" PRIu32 " B  sha256 %s\n", h.bin_size, sha);
    std::printf("  board      \"%s\"  version \"%s\"  target \"%s\"\n", h.board.c_str(), h.version.c_str(),
                h.target_id.c_str());
    std::printf("Verifying...\n");
    const auto v = img.verify();
    if (v.init.sd_req_count) {
        std::printf("  dat        device_type 0x%04X  sd_req", v.init.device_type);
        for (uint16_t sd : v.init.softdevice_requirements()) std::printf(" 0x%04X", sd);
        std::printf("  crc16 0x%04X (bin 0x%04X)\n", v.init.firmware_crc16, v.bin_crc16);
    }
    std::printf("  result     %s%s\n", v.ok ? "OK" : "FAIL: ", v.error.c_str());
    return v.ok ? 0 : 1;
}

int cmd_probe(int argc, char** argv) {
    ble::Mac mac;
    if (!require_target(mac)) return 1;
    const bool reset = !has_flag(argc, argv, "--no-reset");
    const long wait_s = opt_long(argc, argv, "--wait", 60);
    std::printf("Probing %s (waiting up to %ld s; run `start ota` on the repeater now if you haven't).\n"
                "Non-destructive: no START is sent%s.\n",
                mac.str().c_str(), wait_s, reset ? "; SYS_RESET at the end reboots the target into its app" : "");
    ble::ConnParams conn;
    conn.interval_ms = static_cast<uint16_t>(opt_long(argc, argv, "--interval", 0));
    const auto r = ble::probe(mac, static_cast<uint32_t>(wait_s) * 1000, reset, conn);
    if (!r.ok) {
        std::printf("PROBE FAILED: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("PROBE OK\n");
    print_link(r.link);
    if (!reset) {
        std::printf("Target left in its bootloader. Send `blreset` (or power-cycle) to return it to the app.\n");
    }
    return 0;
}

int cmd_flash(int argc, char** argv) {
    ble::Mac mac;
    if (!require_target(mac)) return 1;
    if (!has_flag(argc, argv, "--yes")) {
        std::printf("This ERASES the target's application and flashes the staged image.\n"
                    "Re-run as: flash --yes [--attempts N] [--prn N] [--force-prn] [--packet N] [--interval MS]\n"
                    "                       [--settle MS] [--wait S] [--wifi on|off]\n");
        return 1;
    }
    StagedImage img;
    if (!img.open()) return 1;
    const auto v = img.verify();
    if (!v.ok) {
        std::printf("Staged image failed checks: %s. Nothing was touched.\n", v.error.c_str());
        return 1;
    }
    ble::FlashOptions o;
    o.prn = static_cast<uint16_t>(opt_long(argc, argv, "--prn", 8));
    o.force_prn = has_flag(argc, argv, "--force-prn");
    o.max_packet = static_cast<uint16_t>(opt_long(argc, argv, "--packet", 0));
    o.start_settle_ms = static_cast<uint32_t>(opt_long(argc, argv, "--settle", 500));
    o.scan_ms = static_cast<uint32_t>(opt_long(argc, argv, "--wait", 600)) * 1000;
    o.conn.interval_ms = static_cast<uint16_t>(opt_long(argc, argv, "--interval", 0));
    const long attempts = opt_long(argc, argv, "--attempts", 3);
    o.attempts = static_cast<uint8_t>(attempts < 1 ? 1 : attempts > 10 ? 10 : attempts);
    std::printf("Staged image OK (%" PRIu32 " B, \"%s\"). Waiting up to %" PRIu32
                " s for %s: run `start ota` on the repeater now. Up to %u attempt(s).\n",
                img.header().bin_size, img.header().version.c_str(), o.scan_ms / 1000, mac.str().c_str(), o.attempts);

    runlog::RunLog* run_log = runlog::idf::open_run_log();
    const uint32_t seq = run_log ? run_log->next_seq() : 0;
    const uint64_t log_mark = runlog::idf::log_position();
    const uint32_t started_s = uptime_s();
    if (run_log != nullptr) {  // without a run log an abort couldn't be recorded anyway
        runlog::idf::JobMarker job;
        job.seq = seq;
        job.started_uptime_s = started_s;
        std::snprintf(job.target, sizeof(job.target), "%s", mac.str().c_str());
        std::snprintf(job.version, sizeof(job.version), "%s", img.header().version.c_str());
        runlog::idf::job_begin(job);
        o.before_transfer = [](uint8_t) { runlog::idf::job_set_stage(runlog::idf::JobStage::Transferring); };
    }

    // SPEC.md §7.1: WiFi off during the transfer is the provisional default;
    // `--wifi on` keeps it up so the two can be compared in the run records.
    // The console is attended, so it skips the M3b run deadline and simply
    // restores WiFi afterwards.
    bool wifi_off = false;
    const char* wifi_opt = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--wifi") == 0) wifi_opt = argv[i + 1];
    }
    if (!(wifi_opt != nullptr && std::strcmp(wifi_opt, "on") == 0) && net::status().initialised) {
        std::printf("WiFi is off until the flash finishes (--wifi on keeps it up).\n");
        std::fflush(stdout);
        wifi_off = net::stop();
    }

    const auto r = ble::flash(mac, img, img.header().bin_size, img.header().dat_bytes(), v.init, o);

    if (wifi_off) {
        const bool back = net::restore(30000);
        std::printf("WiFi %s.\n", back ? "restored" : "restarted; still reconnecting in the background");
    }

    uint32_t recorded = 0;
    if (run_log != nullptr) {
        const std::string json = runlog::to_json(make_record(seq, mac, img, r, started_s, wifi_off));
        recorded = run_log->append(json, runlog::idf::log_since(log_mark, runlog::kSlotPayloadMax));
        runlog::idf::job_end();
    }

    if (r.link.mtu) print_link(r.link);
    const auto& t = r.transfer;
    for (size_t i = 0; i < dfu::kTransferPhaseCount; ++i) {
        if (t.phase_ms[i]) {
            std::printf("  %-17s %" PRIu32 " ms\n", dfu::to_string(static_cast<dfu::TransferPhase>(i)), t.phase_ms[i]);
        }
    }
    const uint32_t stream_ms = t.phase_ms[static_cast<size_t>(dfu::TransferPhase::Stream)];
    if (stream_ms) {
        const uint32_t tenths = static_cast<uint32_t>(uint64_t{t.bytes_sent} * 10 / stream_ms);  // 0.1 kB/s
        std::printf("  throughput        %" PRIu32 ".%" PRIu32 " kB/s\n", tenths / 10, tenths % 10);
    }
    if (r.attempts.size() > 1 || !r.ok) {
        for (const auto& a : r.attempts) {
            std::printf("  attempt %u         interval %u ms, PRN %u: %s", a.number, a.params.interval_ms, a.params.prn,
                        a.failure == dfu::FailureKind::None ? "OK" : a.error.c_str());
            if (a.failure != dfu::FailureKind::None) {
                std::printf(" [%s, %" PRIu32 " B acked]", dfu::to_string(a.failure), a.transfer.bytes_acked);
            }
            std::printf("\n");
        }
    }
    if (r.ok) {
        std::printf("FLASH OK in %" PRIu32 ".%" PRIu32 " s%s%s. Check the repeater with `ver`.\n", r.total_ms / 1000,
                    (r.total_ms % 1000) / 100, r.attempts.size() > 1 ? " (after retries)" : "",
                    t.activate_disconnect_seen ? "" : " (no disconnect seen after ACTIVATE)");
        if (recorded) std::printf("Recorded as run #%" PRIu32 " (`run %" PRIu32 "` for details).\n", recorded, recorded);
        if (run_log != nullptr && !recorded) std::printf("Warning: could not write the run record.\n");
        return 0;
    }
    std::printf("FLASH FAILED after %u attempt(s): %s\n", static_cast<unsigned>(r.attempts.size()), r.error.c_str());
    if (recorded) std::printf("Recorded as run #%" PRIu32 " (`run %" PRIu32 "` for details and log).\n", recorded, recorded);
    if (run_log != nullptr && !recorded) std::printf("Warning: could not write the run record.\n");
    if (r.target_erased) {
        const bool recoverable = r.failure == dfu::FailureKind::LinkOrOverrun ||
                                 r.failure == dfu::FailureKind::Transient ||
                                 r.failure == dfu::FailureKind::StaleSession;
        std::printf("The target's app was erased. The staged image is intact; run `flash --yes` again once the "
                    "cause is fixed.%s\n",
                    recoverable ? " OTAFIX waits in BLE OTA, usually at MAC+1."
                                : " Bootloader state unknown: check with `probe --no-reset` before retrying.");
    } else {
        std::printf("The target was not erased.\n");
    }
    return 1;
}

int cmd_runs(int argc, char** argv) {
    runlog::RunLog* run_log = runlog::idf::open_run_log();
    if (run_log == nullptr) {
        std::printf("No runlog partition.\n");
        return 1;
    }
    long limit = argc >= 2 ? std::strtol(argv[1], nullptr, 0) : 20;
    if (limit <= 0) limit = 20;
    const auto runs = run_log->list();
    if (runs.empty()) {
        std::printf("No runs recorded yet.\n");
        return 0;
    }
    std::printf("%5s  %-17s  %7s  %3s  %-6s  %s\n", "RUN", "RESULT", "TIME", "ATT", "ERASED", "VERSION / ERROR");
    long shown = 0;
    for (const auto& s : runs) {
        if (shown++ >= limit) break;
        const std::string result = runlog::json_get_string(s.json, "result");
        const uint64_t ms = runlog::json_get_uint(s.json, "duration_ms");
        size_t attempts = 0;
        for (size_t at = s.json.find("{\"n\":"); at != std::string::npos; at = s.json.find("{\"n\":", at + 1)) {
            ++attempts;
        }
        const std::string err = runlog::json_get_string(s.json, "error");
        std::printf("%5" PRIu32 "  %-17s  %3u.%01us  %3u  %-6s  %s%s%s\n", s.seq, result.c_str(),
                    static_cast<unsigned>(ms / 1000), static_cast<unsigned>((ms % 1000) / 100),
                    static_cast<unsigned>(attempts), runlog::json_get_string(s.json, "target_erased").c_str(),
                    runlog::json_get_string(s.json, "image_version").c_str(), err.empty() ? "" : " / ", err.c_str());
    }
    return 0;
}

int cmd_run(int argc, char** argv) {
    runlog::RunLog* run_log = runlog::idf::open_run_log();
    if (run_log == nullptr || argc < 2) {
        std::printf("Usage: run <N>  (see `runs`)\n");
        return 1;
    }
    runlog::RunSlot slot;
    if (!run_log->get(static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 0)), slot)) {
        std::printf("Run %s not found (rotated out, or never recorded).\n", argv[1]);
        return 1;
    }
    std::printf("run #%" PRIu32 "\n%s\n--- log (%u bytes) ---\n", slot.seq, slot.json.c_str(),
                static_cast<unsigned>(slot.log.size()));
    std::fwrite(slot.log.data(), 1, slot.log.size(), stdout);
    return 0;
}

int cmd_logs(int argc, char** argv) {
    const long bytes = argc >= 2 ? std::strtol(argv[1], nullptr, 0) : 4096;
    const std::string tail = runlog::idf::log_tail(static_cast<size_t>(bytes > 0 ? bytes : 4096));
    std::fwrite(tail.data(), 1, tail.size(), stdout);
    return 0;
}

int cmd_blreset(int argc, char** argv) {
    ble::Mac mac;
    if (!require_target(mac)) return 1;
    const long wait_s = opt_long(argc, argv, "--wait", 20);
    std::string err;
    if (!ble::reset_bootloader(mac, static_cast<uint32_t>(wait_s) * 1000, err)) {
        std::printf("blreset failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("SYS_RESET sent.\n");
    return 0;
}

// linenoise redraws the prompt with a raw write() that starts with "\r" and
// "erase to end of line". Anything still in the stdio buffer at that point is
// overwritten or appears after the prompt, so flush before returning.
int run_and_flush(void* context, int argc, char** argv) {
    const auto fn = reinterpret_cast<esp_console_cmd_func_t>(context);
    const int ret = fn(argc, argv);
    std::fflush(stdout);
    fsync(fileno(stdout));
    return ret;
}

void reg(const char* name, const char* help, const char* hint, esp_console_cmd_func_t fn) {
    esp_console_cmd_t cmd{};
    cmd.command = name;
    cmd.help = help;
    cmd.hint = hint;
    cmd.func_w_context = &run_and_flush;
    cmd.context = reinterpret_cast<void*>(fn);
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace

void start_console() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "dfu32> ";
    cfg.task_stack_size = 12288;  // HTTP client + manifest parsing run here
    cfg.max_cmdline_length = 256;
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&cfg, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());

    reg("target", "Show or set the target: BLE MAC (app address), board, manifest URL, id, max app size.",
        "[<MAC> [random|public] | board <name> | url <manifest-url> | id <slug> | maxapp <bytes> | clear]",
        &cmd_target);
    reg("wifi", "Show WiFi status, or join a network (stored for later boots).", "[<ssid> [password] | forget]",
        &cmd_wifi);
    reg("check", "Fetch the target's manifest and compare it with the board and the staged image (writes nothing).",
        "[manifest-url]", &cmd_check);
    reg("stage", "Download, verify and stage the target's image from its manifest.", "[manifest-url] [--force]",
        &cmd_stage);
    reg("scan", "List BLE advertisers (address, type, RSSI, DFU service, name).", "[seconds]", &cmd_scan);
    reg("staged", "Show and verify the image in the staging partition.", nullptr, &cmd_staged);
    reg("probe",
        "Non-destructive link test: enter the bootloader, report MTU / packet size / PHY / bootloader version, "
        "then SYS_RESET back to the app.",
        "[--no-reset] [--interval MS] [--wait S]", &cmd_probe);
    reg("flash", "Flash the staged image to the target (erases its app), retrying up to --attempts times.",
        "--yes [--attempts N] [--prn N] [--force-prn] [--packet N] [--interval MS] [--settle MS] [--wait S] "
        "[--wifi on|off]",
        &cmd_flash);
    reg("runs", "List recorded flash runs (newest first).", "[count]", &cmd_runs);
    reg("run", "Show one run record and its log tail.", "<N>", &cmd_run);
    reg("logs", "Show recent captured log output.", "[bytes]", &cmd_logs);
    reg("blreset", "Send SYS_RESET to the target's bootloader (recover a stuck session).", "[--wait S]",
        &cmd_blreset);

    ESP_LOGI(TAG, "console ready; type 'help'");
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

}  // namespace dfu32
