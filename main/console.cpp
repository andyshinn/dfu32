// SPDX-License-Identifier: MIT
//
// M1 bench console (USB serial). Commands: target, scan, staged, probe, flash,
// blreset. Replaced by the HTTP API / web UI in M3-M4, kept as a debug tool.
#include "console.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "dfu_ble/dfu_ble.hpp"
#include "dfu_proto/protocol.hpp"
#include "esp_console.h"
#include "esp_log.h"
#include "nvs.h"
#include "staging/staging.hpp"

namespace dfu32 {
namespace {

constexpr const char* TAG = "console";
constexpr const char* kNvsNamespace = "dfu32";
constexpr const char* kKeyMac = "tgt0_mac";
constexpr const char* kKeyType = "tgt0_type";

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
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return false;
    char buf[18] = {};
    size_t len = sizeof(buf);
    uint8_t type = 1;
    const bool ok = nvs_get_str(h, kKeyMac, buf, &len) == ESP_OK;
    nvs_get_u8(h, kKeyType, &type);
    nvs_close(h);
    return ok && ble::Mac::parse(buf, type, out);
}

bool save_target(const ble::Mac& mac) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, kKeyMac, mac.str().c_str()) == ESP_OK &&
                    nvs_set_u8(h, kKeyType, mac.type) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool require_target(ble::Mac& mac) {
    if (load_target(mac)) return true;
    std::printf("No target set. Use: target <MAC> [random|public]\n"
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
    std::printf("  timings           find %" PRIu32 " ms, jump %" PRIu32 " ms, connect %" PRIu32 " ms\n", l.find_ms,
                l.jump_ms, l.connect_ms);
}

// ---- commands -----------------------------------------------------------------

int cmd_target(int argc, char** argv) {
    if (argc >= 2) {
        uint8_t type = 1;
        if (argc >= 3 && std::strcmp(argv[2], "public") == 0) type = 0;
        ble::Mac mac;
        if (!ble::Mac::parse(argv[1], type, mac)) {
            std::printf("Bad MAC '%s' (want AA:BB:CC:DD:EE:FF)\n", argv[1]);
            return 1;
        }
        if (!save_target(mac)) {
            std::printf("Failed to save target to NVS\n");
            return 1;
        }
    }
    ble::Mac mac;
    if (!load_target(mac)) {
        std::printf("No target set.\n");
        return 0;
    }
    std::printf("target %s (%s), bootloader after reset at %s\n", mac.str().c_str(),
                mac.type ? "random" : "public", mac.plus_one().str().c_str());
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
        std::printf("Nothing staged (%s). Write one with tools/dfu32_stage_image.py.\n",
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
    const auto r = ble::probe(mac, static_cast<uint32_t>(wait_s) * 1000, reset);
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
                    "Re-run as: flash --yes [--prn N] [--packet N] [--settle MS] [--wait S]\n");
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
    o.max_packet = static_cast<uint16_t>(opt_long(argc, argv, "--packet", 0));
    o.start_settle_ms = static_cast<uint32_t>(opt_long(argc, argv, "--settle", 500));
    o.scan_ms = static_cast<uint32_t>(opt_long(argc, argv, "--wait", 600)) * 1000;
    std::printf("Staged image OK (%" PRIu32 " B, \"%s\"). Waiting up to %" PRIu32
                " s for %s: run `start ota` on the repeater now.\n",
                img.header().bin_size, img.header().version.c_str(), o.scan_ms / 1000, mac.str().c_str());

    const auto r = ble::flash(mac, img, img.header().bin_size, img.header().dat_bytes(), v.init, o);
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
    if (r.ok) {
        std::printf("FLASH OK in %" PRIu32 ".%" PRIu32 " s%s. Check the repeater with `ver`.\n", r.total_ms / 1000,
                    (r.total_ms % 1000) / 100,
                    t.activate_disconnect_seen ? "" : " (no disconnect seen after ACTIVATE)");
        return 0;
    }
    std::printf("FLASH FAILED: %s\n", r.error.c_str());
    if (t.target_erased) {
        std::printf("The target's app was erased. The staged image is intact: run `flash --yes` again%s.\n",
                    r.sys_reset_after_failure ? " (SYS_RESET sent; OTAFIX returns to BLE OTA, possibly at MAC+1)"
                                              : "; if START reports INVALID_STATE, run `blreset` first");
    } else {
        std::printf("The target was not erased.\n");
    }
    return 1;
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

void reg(const char* name, const char* help, const char* hint, esp_console_cmd_func_t fn) {
    esp_console_cmd_t cmd{};
    cmd.command = name;
    cmd.help = help;
    cmd.hint = hint;
    cmd.func = fn;
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

}  // namespace

void start_console() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    cfg.prompt = "dfu32> ";
    cfg.task_stack_size = 8192;
    cfg.max_cmdline_length = 256;
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&cfg, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());

    reg("target", "Show or set the target repeater's BLE MAC (app address).", "[MAC [random|public]]", &cmd_target);
    reg("scan", "List BLE advertisers (address, type, RSSI, DFU service, name).", "[seconds]", &cmd_scan);
    reg("staged", "Show and verify the image in the staging partition.", nullptr, &cmd_staged);
    reg("probe",
        "Non-destructive link test: enter the bootloader, report MTU / packet size / PHY / bootloader version, "
        "then SYS_RESET back to the app.",
        "[--no-reset] [--wait S]", &cmd_probe);
    reg("flash", "Flash the staged image to the target (erases its app).",
        "--yes [--prn N] [--packet N] [--settle MS] [--wait S]", &cmd_flash);
    reg("blreset", "Send SYS_RESET to the target's bootloader (recover a stuck session).", "[--wait S]",
        &cmd_blreset);

    ESP_LOGI(TAG, "console ready; type 'help'");
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

}  // namespace dfu32
