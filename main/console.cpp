// SPDX-License-Identifier: MIT
//
// Bench console (USB serial): target, wifi, check, stage, scan, staged, probe,
// flash, runs, run, logs, blreset, job, token, improv. The HTTP API and the web UI (M4)
// cover the same ground; the console stays as a debug tool. Commands that use
// BLE or the staging partition take the job lease, so they can't run while an
// API job does (and vice versa). The target commands act on the default
// target (`target use <id>` picks it). Ctrl+C or Ctrl+D aborts the BLE commands
// (scan, probe, flash, blreset); `job abort` stops an API job.
#include "console.hpp"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "api/api.hpp"
#include "console_io.hpp"
#include "config/config.hpp"
#include "dfu_ble/dfu_ble.hpp"
#include "fetch/fetch.hpp"
#include "improv/improv.hpp"
#include "job/job.hpp"
#include "net/net.hpp"
#include "dfu_proto/protocol.hpp"
#include "dfu_proto/retry.hpp"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "runlog/run_record.hpp"
#include "runlog/runlog_idf.hpp"
#include "staging/staging.hpp"

namespace dfu32 {
namespace {

constexpr const char* TAG = "console";

// ---- helpers ------------------------------------------------------------------

// While a command runs, the REPL is not reading the console, so Ctrl+C would
// just sit in the input buffer. This watcher reads the console input (from the
// reader in console_io, which has already taken out any Improv frames) and
// turns Ctrl+C (0x03) or Ctrl+D (0x04) into ble::request_abort(). An abort can
// leave the target erased, so a read counts only if it holds nothing but those
// bytes: a paste or escape sequence that happens to contain one doesn't abort.
// Other keys typed meanwhile are discarded so they don't run as a command
// afterwards. Construct it on the command's stack; the destructor stops the
// watcher before the REPL reads input again.
class AbortOnCtrlC {
public:
    AbortOnCtrlC() {
        ble::clear_abort();
        done_ = xSemaphoreCreateBinary();
        if (done_ == nullptr || xTaskCreate(&AbortOnCtrlC::run, "ctrl_c", 3072, this, uxTaskPriorityGet(nullptr),
                                            nullptr) != pdPASS) {
            ESP_LOGW(TAG, "Ctrl+C watcher not started; Ctrl+C won't abort this command");
            if (done_ != nullptr) vSemaphoreDelete(done_);
            done_ = nullptr;
            return;
        }
        std::printf("(Ctrl+C to abort)\n");
        std::fflush(stdout);
    }
    ~AbortOnCtrlC() {
        if (done_ != nullptr) {
            stop_.store(true);
            xSemaphoreTake(done_, portMAX_DELAY);
            vSemaphoreDelete(done_);
        }
    }
    AbortOnCtrlC(const AbortOnCtrlC&) = delete;
    AbortOnCtrlC& operator=(const AbortOnCtrlC&) = delete;

private:
    static bool only_abort_keys(const char* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != 0x03 && p[i] != 0x04) return false;
        }
        return n > 0;
    }

    static void run(void* arg) {
        auto* self = static_cast<AbortOnCtrlC*>(arg);
        bool announced = false;
        while (!self->stop_.load()) {
            char buf[16];
            const size_t n = console_io::read(buf, sizeof(buf), pdMS_TO_TICKS(50));
            if (n == 0) continue;
            if (!announced && only_abort_keys(buf, n)) {
                ESP_LOGW(TAG, "abort requested from the console; stopping at the next safe point");
                ble::request_abort();
                announced = true;
            }
        }
        xSemaphoreGive(self->done_);
        vTaskDelete(nullptr);
    }

    SemaphoreHandle_t done_ = nullptr;
    std::atomic<bool> stop_{false};
};

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

/// The default target and its MAC; false (with a hint) if there is none or
/// it has no MAC. `cfg`, if given, receives the target's settings.
bool require_target(ble::Mac& mac, TargetConfig* cfg = nullptr) {
    TargetConfig t;
    bool found = false;
    if (!config::load_default_target(t, found)) {
        std::printf("Failed to read settings from NVS\n");
        return false;
    }
    if (found && !t.mac.empty() && ble::Mac::parse(t.mac, t.addr_type, mac)) {
        if (cfg != nullptr) *cfg = std::move(t);
        return true;
    }
    if (found) {
        std::printf("No target MAC set for the default target '%s'. Use: target <MAC> [random|public]\n"
                    "(MeshCore prints the MAC in its `start ota` reply; `target use <id>` picks another target.)\n",
                    t.effective_id().c_str());
    } else {
        std::printf("No target MAC set. Use: target <MAC> [random|public]\n"
                    "(MeshCore prints the MAC in its `start ota` reply.)\n");
    }
    return false;
}

/// The default target's settings (all defaults when there is none). Commands
/// that only need a manifest URL or board (check, stage) work without one.
TargetConfig default_target() {
    TargetConfig t;
    bool found = false;
    if (!config::load_default_target(t, found)) std::printf("Failed to read settings from NVS\n");
    return t;
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

/// Takes the BLE/staging lease or explains who has it.
bool acquire(job::Lease& lease) {
    if (lease.ok()) return true;
    const char* h = job::holder();
    if (h != nullptr && std::strcmp(h, "improv") == 0) {
        std::printf("Busy: Improv BLE provisioning is open. Set WiFi first, or `improv stop`.\n");
    } else {
        std::printf("Busy: %s is using BLE/staging. Wait for it (see `job`), or `job abort`.\n", h ? h : "something");
    }
    return false;
}

// ---- commands -----------------------------------------------------------------

void print_target(const TargetConfig& t, bool is_default) {
    // tools/bench/parse.py reads these lines (the first word after the label).
    std::printf("  id         %s%s\n", t.effective_id().c_str(), is_default ? " (default target)" : "");
    std::printf("  mac        %s", t.mac.empty() ? "(not set)" : t.mac.c_str());
    ble::Mac mac;
    if (!t.mac.empty() && ble::Mac::parse(t.mac, t.addr_type, mac)) {
        std::printf(" (%s), bootloader after reset at %s or %s", t.addr_type ? "random" : "public", mac.str().c_str(),
                    mac.plus_one().str().c_str());
    }
    std::printf("\n  board      %s\n", t.board.empty() ? "(not set: images aren't compared with it)" : t.board.c_str());
    std::printf("  manifest   %s\n", t.manifest_url.empty() ? "(not set)" : t.manifest_url.c_str());
    if (t.max_app_size) {
        std::printf("  max app    %" PRIu32 " B\n", t.max_app_size);
    } else {
        std::printf("  max app    default (798720 B for nRF52, none for nRF54)\n");
    }
}

constexpr const char* kTargetUsage =
    "Use: target [list | use <id> | add <id> [<MAC> [random|public]] | <MAC> [random|public] | board <ID>\n"
    "            | url <manifest-url> | id <slug> | maxapp <bytes> | clear | delete <id>]\n";

/// Prints the default target, or how to set one.
void show_default(const TargetTable& table) {
    const StoredTarget* def = table.default_target();
    if (def == nullptr) {
        std::printf("No target yet. Use: target <MAC> [random|public]\n");
        return;
    }
    print_target(def->cfg, true);
    size_t pos = 1;
    for (const auto& st : table.targets()) {
        if (&st == def) break;
        ++pos;
    }
    std::printf("  default    yes (%u of %u target%s; max %u)%s\n", static_cast<unsigned>(pos),
                static_cast<unsigned>(table.size()), table.size() == 1 ? "" : "s",
                static_cast<unsigned>(table.max_targets()), table.size() > 1 ? ": `target list` shows all" : "");
}

void list_targets(const TargetTable& table) {
    if (table.empty()) {
        std::printf("No targets yet. Use: target <MAC> [random|public], or target add <id> [<MAC> [random|public]]\n");
        return;
    }
    std::printf("  %-20s  %-17s  %-16s  %s\n", "ID", "MAC", "BOARD", "LAST OK");
    for (const auto& st : table.targets()) {
        const TargetConfig& c = st.cfg;
        std::printf("%c %-20s  %-17s  %-16s  %s\n", table.is_default(st.id()) ? '*' : ' ', st.id().c_str(),
                    c.mac.empty() ? "-" : c.mac.c_str(), c.board.empty() ? "-" : c.board.c_str(),
                    c.last_ok_version.empty() ? "-" : c.last_ok_version.c_str());
    }
    std::printf("%u of %u targets. * = default: check, stage, probe, flash and blreset use it (`target use <id>`).\n",
                static_cast<unsigned>(table.size()), static_cast<unsigned>(table.max_targets()));
}

bool commit(const TargetTable& table, const TargetWrites& w) {
    if (w.empty() || config::commit_targets(table, w)) return true;
    std::printf("Failed to save target to NVS\n");
    return false;
}

/// Stores `t` (create or update by id) and reports a refusal.
bool put_target(TargetTable& table, TargetConfig t) {
    const auto r = table.put(std::move(t));
    if (r.status != TargetTable::PutStatus::Created && r.status != TargetTable::PutStatus::Updated) {
        std::printf("Not saved: %s\n", r.error.c_str());
        return false;
    }
    return commit(table, r.writes);
}

/// Parses `<MAC> [random|public]` into `t`; false if the MAC is bad.
bool set_mac(TargetConfig& t, const char* mac_arg, const char* type_arg) {
    const std::string mac = normalize_mac(mac_arg);
    if (mac.empty()) return false;
    t.mac = mac;
    t.addr_type = (type_arg != nullptr && std::strcmp(type_arg, "public") == 0) ? 0 : 1;
    return true;
}

int cmd_target(int argc, char** argv) {
    const std::string field = argc >= 2 ? argv[1] : "";
    const std::string value = argc >= 3 ? argv[2] : "";
    const bool read_only = field.empty() || field == "list";
    // Changes hold the device-wide lease like the API's target PUT/DELETE, and
    // load the table under it: otherwise a console `target add` and an API
    // PUT could both pick the same free slot and one target would be lost.
    std::optional<job::Lease> lease;
    if (!read_only) {
        lease.emplace("config");
        if (!acquire(*lease)) return 1;
    }
    TargetTable table;
    if (!config::load_targets(table)) {
        std::printf("Failed to read settings from NVS\n");
        return 1;
    }
    if (field.empty()) {
        show_default(table);
        return 0;
    }
    if (field == "list") {
        list_targets(table);
        return 0;
    }

    if (field == "use") {
        if (value.empty()) {
            std::printf("Use: target use <id> (`target list` shows the ids)\n");
            return 1;
        }
        TargetWrites w;
        if (!table.set_default(value, w)) {
            std::printf("No target '%s' (`target list` shows the ids).\n", value.c_str());
            return 1;
        }
        if (!commit(table, w)) return 1;
        std::printf("Default target: '%s'. check, stage, probe, flash and blreset use the default target.\n",
                    value.c_str());
        show_default(table);
        return 0;
    }

    if (field == "add") {
        if (value.empty()) {
            std::printf("Use: target add <id> [<MAC> [random|public]]\n");
            return 1;
        }
        if (auto e = validate_target_id(value); !e.empty()) {
            std::printf("Not saved: %s\n", e.c_str());
            return 1;
        }
        if (table.find(value) != nullptr) {
            std::printf("Not saved: a target with id '%s' already exists (`target use %s` makes it the default).\n",
                        value.c_str(), value.c_str());
            return 1;
        }
        TargetConfig t;
        t.id = value;
        if (argc >= 4 && !set_mac(t, argv[3], argc >= 5 ? argv[4] : nullptr)) {
            std::printf("Not saved: bad MAC '%s' (e.g. F4:1C:36:A7:33:95)\n", argv[3]);
            return 1;
        }
        auto r = table.put(std::move(t));
        if (r.status != TargetTable::PutStatus::Created) {
            std::printf("Not saved: %s\n", r.error.empty() ? "the target was not created" : r.error.c_str());
            return 1;
        }
        // The new target becomes the default, so `target board ...` etc. edit it next.
        table.set_default(value, r.writes);
        if (!commit(table, r.writes)) return 1;
        std::printf("Target '%s' added; it is now the default target.\n", value.c_str());
        show_default(table);
        return 0;
    }

    if (field == "clear" || field == "delete") {
        std::string id = value;
        if (field == "clear") {
            id = table.default_id();
            if (id.empty()) {
                std::printf("No target to clear.\n");
                return 0;
            }
        } else if (id.empty()) {
            std::printf("Use: target delete <id> (`target list` shows the ids)\n");
            return 1;
        }
        TargetWrites w;
        if (!table.remove(id, w)) {
            std::printf("No target '%s' (`target list` shows the ids).\n", id.c_str());
            return 1;
        }
        if (!commit(table, w)) {
            std::printf("Failed to delete the target\n");
            return 1;
        }
        if (table.empty()) {
            std::printf("Target '%s' deleted. No targets left.\n", id.c_str());
        } else {
            std::printf("Target '%s' deleted. The default target is now '%s'.\n", id.c_str(),
                        table.default_id().c_str());
        }
        return 0;
    }

    // Everything else edits the default target; with none, it creates one
    // (id `target`, or the id given to `target id`).
    const StoredTarget* def = table.default_target();
    TargetConfig t = def != nullptr ? def->cfg : TargetConfig{};
    if (field == "id") {
        if (value.empty()) {
            std::printf("Use: target id <slug>\n");
            return 1;
        }
        if (def == nullptr) {
            if (auto e = validate_target_id(value); !e.empty()) {
                std::printf("Not saved: %s\n", e.c_str());
                return 1;
            }
            t.id = value;
            if (!put_target(table, std::move(t))) return 1;
        } else {
            TargetWrites w;
            if (auto e = table.rename(def->id(), value, w); !e.empty()) {
                std::printf("Not saved: %s\n", e.c_str());
                return 1;
            }
            if (!commit(table, w)) return 1;
        }
        show_default(table);
        return 0;
    }

    std::string err;
    if (field == "board") {
        err = validate_board(value);
        t.board = value;
    } else if (field == "url") {
        err = validate_manifest_url(value);
        t.manifest_url = value;
    } else if (field == "maxapp") {
        uint32_t bytes = 0;
        if (!parse_u32(value, bytes)) {
            std::printf("Not saved: '%s' is not a number of bytes (e.g. 802816 or 0xC4000; 0 = default)\n",
                        value.c_str());
            return 1;
        }
        err = validate_max_app_size(bytes);
        t.max_app_size = bytes;
    } else if (!set_mac(t, argv[1], argc >= 3 ? argv[2] : nullptr)) {
        // target <MAC> [random|public]
        std::printf("Bad MAC or field '%s'. %s", argv[1], kTargetUsage);
        return 1;
    }
    if (!err.empty()) {
        std::printf("Not saved: %s\n", err.c_str());
        return 1;
    }
    if (!put_target(table, std::move(t))) return 1;
    show_default(table);
    return 0;
}

// ---- networking (M3) -------------------------------------------------------------

int cmd_wifi(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "forget") == 0) {
        const bool ok = net::forget();
        std::printf("%s\n", ok ? "WiFi credentials cleared. `improv start` opens Improv BLE to set new ones."
                                : "Failed to clear WiFi credentials.");
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
    const TargetConfig t = default_target();
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
                t.board.empty() ? "not set, not compared" : r.board_ok ? "match" : "MISMATCH");
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
    const TargetConfig t = default_target();
    std::string url;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') url = argv[i];
    }
    if (url.empty() && t.manifest_url.empty()) {
        std::printf("No manifest URL. Use: stage <url>, or set one with `target url <url>`.\n");
        return 1;
    }
    if (!require_network()) return 1;
    job::Lease lease("console");
    if (!acquire(lease)) return 1;
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
    job::Lease lease("console");
    if (!acquire(lease)) return 1;
    std::printf("Scanning %ld s...\n", secs);
    std::vector<ble::Advert> list;
    {
        AbortOnCtrlC watch;
        list = ble::scan(static_cast<uint32_t>(secs > 0 ? secs : 8) * 1000);
    }
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
        std::printf("Nothing staged (%s). Use `stage`, upload one from the web UI's Image page, or write one with "
                    "tools/dfu32_stage_image.py.\n",
                    dfu::to_string(img.header_status()));
        return 1;
    }
    const auto& h = img.header();
    char sha[65] = {};
    hex(h.bin_sha256.data(), 32, sha);
    std::printf("  bin        %" PRIu32 " B  sha256 %s\n", h.bin_size, sha);
    std::printf("  board      \"%s\"  version \"%s\"  target \"%s\"\n", h.board.c_str(), h.version.c_str(),
                h.target_id.c_str());
    // A manifest URL, or where the image came from: "upload:<file>" (web UI / API), "usb:<file>" (installer page).
    if (!h.manifest_url.empty()) std::printf("  from       %s\n", h.manifest_url.c_str());
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
    job::Lease lease("console");
    if (!acquire(lease)) return 1;
    std::printf("Probing %s (waiting up to %ld s; run `start ota` on the repeater now if you haven't).\n"
                "Non-destructive: no START is sent%s.\n",
                mac.str().c_str(), wait_s, reset ? "; SYS_RESET at the end reboots the target into its app" : "");
    ble::ConnParams conn;
    conn.interval_ms = static_cast<uint16_t>(opt_long(argc, argv, "--interval", 0));
    ble::ProbeReport r;
    {
        AbortOnCtrlC watch;
        r = ble::probe(mac, static_cast<uint32_t>(wait_s) * 1000, reset, conn);
    }
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
    TargetConfig tc;
    if (!require_target(mac, &tc)) return 1;
    if (!has_flag(argc, argv, "--yes")) {
        std::printf("This ERASES the target's application and flashes the staged image.\n"
                    "Re-run as: flash --yes [--attempts N] [--prn N] [--force-prn] [--packet N] [--interval MS]\n"
                    "                       [--settle MS] [--wait S] [--wifi on|off]\n"
                    "Ctrl+C or Ctrl+D aborts; before START the target is left untouched.\n");
        return 1;
    }
    job::Lease lease("console");
    if (!acquire(lease)) return 1;
    StagedImage img;
    if (!img.open()) return 1;
    const auto v = img.verify();
    if (!v.ok) {
        std::printf("Staged image failed checks: %s. Nothing was touched.\n", v.error.c_str());
        return 1;
    }
    ble::FlashOptions o;
    o.prn = static_cast<uint16_t>(opt_long(argc, argv, "--prn", dfu::kDefaultPrn));
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
    job::RunContext ctx;
    ctx.seq = run_log ? run_log->next_seq() : 0;
    ctx.target_id = tc.effective_id();
    ctx.started_uptime_s = uptime_s();
    ctx.started_unix = net::unix_time();
    ctx.trigger = "console";
    ctx.source = "staged";
    const uint64_t log_mark = runlog::idf::log_position();
    if (run_log != nullptr) {  // without a run log an abort couldn't be recorded anyway
        runlog::idf::JobMarker marker;
        marker.seq = ctx.seq;
        marker.started_uptime_s = ctx.started_uptime_s;
        std::snprintf(marker.target, sizeof(marker.target), "%s", mac.str().c_str());
        std::snprintf(marker.version, sizeof(marker.version), "%s", img.header().version.c_str());
        runlog::idf::job_begin(marker);
        o.before_transfer = [](uint8_t) { runlog::idf::job_set_stage(runlog::idf::JobStage::Transferring); };
    }

    // SPEC.md §7.1: WiFi stays up during the transfer by default (bench: about
    // 47 s vs 39 s for the T096 image, no failures either way); `--wifi off`
    // stops it. The console is attended, so it skips the M3b run deadline and
    // simply restores WiFi afterwards.
    bool wifi_off = false;
    const char* wifi_opt = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--wifi") == 0) wifi_opt = argv[i + 1];
    }
    if (wifi_opt != nullptr && std::strcmp(wifi_opt, "off") == 0 && net::status().initialised) {
        std::printf("WiFi is off until the flash finishes.\n");
        std::fflush(stdout);
        wifi_off = net::stop();
    }

    ble::FlashReport r;
    {
        AbortOnCtrlC watch;
        r = ble::flash(mac, img, img.header().bin_size, img.header().dat_bytes(), v.init, o);
    }

    if (wifi_off) {
        const bool back = net::restore(30000);
        std::printf("WiFi %s.\n", back ? "restored" : "restarted; still reconnecting in the background");
    }

    uint32_t recorded = 0;
    if (run_log != nullptr) {
        ctx.wifi_off = wifi_off;
        const std::string json = runlog::to_json(job::make_run_record(ctx, mac, img, r));
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
    const bool aborted = r.aborted;
    std::printf("FLASH %s after %u attempt(s): %s\n", aborted ? "ABORTED" : "FAILED",
                static_cast<unsigned>(r.attempts.size()), r.error.c_str());
    if (recorded) std::printf("Recorded as run #%" PRIu32 " (`run %" PRIu32 "` for details and log).\n", recorded, recorded);
    if (run_log != nullptr && !recorded) std::printf("Warning: could not write the run record.\n");
    if (r.target_erased) {
        const bool recoverable = r.failure == dfu::FailureKind::LinkOrOverrun ||
                                 r.failure == dfu::FailureKind::Transient ||
                                 r.failure == dfu::FailureKind::StaleSession ||
                                 (aborted && r.sys_reset_after_failure);
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
    job::Lease lease("console");
    if (!acquire(lease)) return 1;
    std::string err;
    bool ok = false;
    {
        AbortOnCtrlC watch;
        ok = ble::reset_bootloader(mac, static_cast<uint32_t>(wait_s) * 1000, err);
    }
    if (!ok) {
        std::printf("blreset failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("SYS_RESET sent.\n");
    return 0;
}

int cmd_job(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "abort") == 0) {
        const job::AbortResult r = job::abort();
        if (!r.was_active) {
            std::printf("No job is running.\n");
            return 1;
        }
        std::printf("Abort requested in %s.%s\n", job::to_string(r.state),
                    r.target_erased ? " The transfer had started: the target's app is erased; flash again to recover."
                                    : "");
        return 0;
    }
    const job::Snapshot s = job::snapshot();
    if (s.id == 0) {
        std::printf("No job since boot. Start one with POST /api/v1/targets/{id}/stage or /flash.\n");
        return 0;
    }
    std::printf("  job        #%" PRIu32 " %s (%s) for '%s'\n", s.id, job::to_string(s.kind), job::to_string(s.source),
                s.target.c_str());
    std::printf("  state      %s%s%s\n", job::to_string(s.state), s.phase.empty() ? "" : " / ", s.phase.c_str());
    if (!s.version.empty()) std::printf("  image      \"%s\"\n", s.version.c_str());
    if (s.bytes_total) {
        std::printf("  progress   %" PRIu32 " / %" PRIu32 " B", s.bytes_done, s.bytes_total);
        if (s.rate_bps) std::printf(", %" PRIu32 " B/s", s.rate_bps);
        std::printf("\n");
    }
    if (s.attempt) std::printf("  attempt    %" PRIu32 " of %" PRIu32 "\n", s.attempt, s.max_attempts);
    if (s.target_erased) std::printf("  target     ERASED (START reached)\n");
    if (!s.error.empty()) std::printf("  error      %s\n", s.error.c_str());
    if (s.run_seq) std::printf("  run        #%" PRIu32 " (`run %" PRIu32 "`)\n", s.run_seq, s.run_seq);
    const char* h = job::holder();
    std::printf("  lease      %s\n", h ? h : "free");
    return 0;
}

int cmd_token(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "rotate") == 0) {
        const std::string t = api::rotate_token();
        if (t.empty()) {
            std::printf("Failed to store a new token.\n");
            return 1;
        }
        std::printf("New API token (the old one stops working):\n%s\n", t.c_str());
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "clear") == 0) {
        const bool ok = api::clear_token();
        std::printf("%s\n", ok ? "Token cleared: the API is open (no token needed). `token rotate` sets one again."
                              : "Failed to clear the token.");
        return ok ? 0 : 1;
    }
    std::array<uint8_t, 32> h{};
    std::printf("API %s. `token rotate` sets a new token; `token clear` turns the token off.\n",
                config::load_token_hash(h) ? "token required" : "open: no token set");
    return 0;
}

int cmd_improv(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "start") == 0) {
        if (!improv::open(improv::Window::Console)) {
            std::printf("Could not open Improv BLE (disabled in this build, or BLE is busy: see `job`).\n");
            return 1;
        }
        std::printf("Improv BLE open: set WiFi from Home Assistant or https://www.improv-wifi.com/ "
                    "(`improv stop` closes it).\n");
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "stop") == 0) {
        std::printf("%s\n", improv::close() ? "Improv BLE closing." : "Improv BLE is not open.");
        return 0;
    }
    static const char* const kStates[] = {"stopped", "awaiting authorization", "waiting for WiFi settings",
                                          "connecting", "provisioned"};
    const improv::Snapshot snap = improv::snapshot();
    const auto st = static_cast<unsigned>(snap.state);
    if (snap.active) {
        std::printf("Improv BLE open: %s\n", st < 5 ? kStates[st] : "?");
    } else {
        std::printf("Improv BLE closed (`improv start` opens it)\n");
    }
    std::printf("Improv Serial always on, on this port (the installer page uses it)\n");
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

// ---- REPL ----------------------------------------------------------------------
//
// esp_console_new_repl_stdio would let linenoise read stdin itself; here it
// reads from console_io instead, whose reader has already taken out any
// Improv Serial frames (SPEC.md §9.2). The loop is the stock REPL's.

constexpr size_t kMaxLine = 256;
constexpr const char* kPromptPlain = "dfu32> ";
#if CONFIG_LOG_COLORS
constexpr const char* kPromptColor = LOG_COLOR_I "dfu32> " LOG_RESET_COLOR;
#else
constexpr const char* kPromptColor = "dfu32> ";
#endif

/// linenoiseProbe() through console_io: asks the terminal for its status
/// (ESC [5n) and waits up to 500 ms for ESC [0n. No answer means no escape
/// sequences, so line editing is off, as with the stock REPL.
bool terminal_answers() {
    std::fputs("\x1b[5n", stdout);
    std::fflush(stdout);
    fsync(fileno(stdout));
    const int64_t until_us = esp_timer_get_time() + 500000;
    size_t got = 0;
    while (got < 4) {
        const int64_t left_us = until_us - esp_timer_get_time();
        if (left_us <= 0) return false;
        char c = 0;
        if (console_io::read(&c, 1, pdMS_TO_TICKS(left_us / 1000) + 1) == 0) continue;
        if (got == 0 && c != 0x1b) continue;  // something else first: keep waiting
        ++got;
    }
    return true;
}

void repl_task(void*) {
    const bool smart = terminal_answers();
    if (!smart) linenoiseSetDumbMode(1);
    const char* prompt = smart ? kPromptColor : kPromptPlain;
    std::printf("\nType 'help' to get the list of commands.\n"
                "Use UP/DOWN arrows to navigate through command history.\n"
                "Press TAB when typing command name to auto-complete.\n");
    if (!smart) {
        std::printf("\nYour terminal application does not support escape sequences.\n"
                    "Line editing and history features are disabled.\n");
    }
    std::fflush(stdout);
    for (;;) {
        char* line = linenoise(prompt);
        if (line == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));  // input backend gone (USB host lost): don't spin
            continue;
        }
        linenoiseHistoryAdd(line);
        int ret = 0;
        const esp_err_t err = esp_console_run(line, &ret);
        if (err == ESP_ERR_NOT_FOUND) {
            std::printf("Unrecognized command\n");
        } else if (err == ESP_OK && ret != ESP_OK) {
            std::printf("Command returned non-zero error code: 0x%x (%s)\n", ret, esp_err_to_name(ret));
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {  // INVALID_ARG: empty line
            std::printf("Internal error: %s\n", esp_err_to_name(err));
        }
        linenoiseFree(line);
    }
}

}  // namespace

void start_console() {
    esp_console_config_t cc = ESP_CONSOLE_CONFIG_DEFAULT();
    cc.max_cmdline_length = kMaxLine;
#if CONFIG_LOG_COLORS
    cc.hint_color = std::atoi(LOG_COLOR_CYAN);
#else
    cc.hint_color = -1;
#endif
    ESP_ERROR_CHECK(esp_console_init(&cc));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback(reinterpret_cast<linenoiseHintsCallback*>(&esp_console_get_hint));
    linenoiseHistorySetMaxLen(32);
    linenoiseSetMaxLineLen(kMaxLine);
    linenoiseSetReadFunction(&console_io::linenoise_read);

    reg("target",
        "Show or set the default target (BLE MAC, board, manifest URL, id, max app size); list, add, pick or delete "
        "targets. check, stage, probe, flash and blreset use the default target.",
        "[list | use <id> | add <id> [<MAC> [random|public]] | <MAC> [random|public] | board <ID> | url <manifest-url>"
        " | id <slug> | maxapp <bytes> | clear | delete <id>]",
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
    reg("job", "Show the current (or last) API job, or abort it.", "[abort]", &cmd_job);
    reg("token", "API token: show whether one is required, set one, or turn it off.", "[rotate | clear]", &cmd_token);
    reg("improv", "Improv WiFi provisioning: show state, open or close Improv BLE.", "[start | stop]", &cmd_improv);

    // HTTP client and manifest parsing run on this task.
    if (xTaskCreate(&repl_task, "console_repl", 12288, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "console task failed to start");
        return;
    }
    ESP_LOGI(TAG, "console ready; type 'help'");
}

}  // namespace dfu32
