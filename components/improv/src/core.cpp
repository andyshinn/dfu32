// SPDX-License-Identifier: MIT
#include "improv/core.hpp"

#include <cctype>
#include <string>

#include "improv.h"  // vendored improv-wifi/sdk-cpp

namespace dfu32::improv {

static_assert(static_cast<uint8_t>(State::Provisioned) == ::improv::STATE_PROVISIONED);
static_assert(static_cast<uint8_t>(Error::UnableToConnect) == ::improv::ERROR_UNABLE_TO_CONNECT);
static_assert(kCmdWifiSettings == ::improv::WIFI_SETTINGS && kCmdIdentify == ::improv::IDENTIFY &&
              kCmdGetCurrentState == ::improv::GET_CURRENT_STATE && kCmdGetDeviceInfo == ::improv::GET_DEVICE_INFO &&
              kCmdGetWifiNetworks == ::improv::GET_WIFI_NETWORKS);

void Core::set_state(State s) {
    state_ = s;
    host_.publish_state(s);
}

void Core::set_error(Error e) {
    error_ = e;
    host_.publish_error(e);
}

void Core::start(bool announce) {
    error_ = Error::None;
    if (announce) {
        set_state(State::Authorized);
    } else {
        state_ = State::Authorized;
    }
}

void Core::stop() {
    if (state_ != State::Stopped) set_state(State::Stopped);
}

void Core::publish(uint8_t command, const std::vector<std::string>& strings) {
    host_.publish_result(encode_result(command, strings, transport_));
}

void Core::handle_rpc(std::span<const uint8_t> packet) {
    if (state_ == State::Stopped) return;
    if (error_ != Error::None) set_error(Error::None);  // a new command clears the last error
    const bool checksummed = transport_ == Transport::Ble;
    if (packet.size() < (checksummed ? 3U : 2U) || packet.size() > kMaxPacket) {
        set_error(Error::InvalidRpc);
        return;
    }
    // One spare zero byte: the SDK reads data[ssid_end] before checking it is
    // in range (README.md next to the vendored copy).
    std::vector<uint8_t> copy(packet.begin(), packet.end());
    copy.push_back(0);
    ::improv::ImprovCommand cmd = ::improv::parse_improv_data(copy.data(), packet.size(), checksummed);
    wipe(copy);  // may hold a passphrase
    struct WipeOnExit {
        std::string& s;
        ~WipeOnExit() { wipe(s); }
    } guard{cmd.password};

    switch (cmd.command) {
        case ::improv::WIFI_SETTINGS: {
            if (state_ == State::Provisioning) {
                set_error(Error::Unknown);  // already connecting; the client must wait
                return;
            }
            const bool ssid_ok = !cmd.ssid.empty() && cmd.ssid.size() <= 32;
            const bool pass_ok = cmd.password.empty() || (cmd.password.size() >= 8 && cmd.password.size() <= 64);
            if (!ssid_ok || !pass_ok) {
                set_error(Error::InvalidRpc);
                return;
            }
            set_state(State::Provisioning);
            if (!host_.begin_connect(cmd.ssid, cmd.password)) connect_failed();
            return;
        }
        case ::improv::IDENTIFY:  // == GET_CURRENT_STATE
            if (transport_ == Transport::Serial) {
                report_current_state();
            } else {
                host_.identify();
            }
            return;
        case ::improv::GET_DEVICE_INFO: {
            const DeviceInfo d = host_.device_info();
            publish(kCmdGetDeviceInfo, {d.firmware, d.version, d.chip, d.name});
            return;
        }
        case ::improv::GET_WIFI_NETWORKS: {
            if (transport_ != Transport::Serial) {
                set_error(Error::UnknownRpc);
                return;
            }
            // One result per network, then an empty one to end the list.
            for (const Network& n : host_.scan_networks()) {
                publish(kCmdGetWifiNetworks, {n.ssid, std::to_string(n.rssi), n.secured ? "YES" : "NO"});
            }
            publish(kCmdGetWifiNetworks, {});
            return;
        }
        case ::improv::UNKNOWN:       // length field disagrees with the packet
        case ::improv::BAD_CHECKSUM:
            set_error(Error::InvalidRpc);
            return;
        default:
            set_error(Error::UnknownRpc);
            return;
    }
}

void Core::report_current_state() {
    // Always answer with a state packet, even an unchanged one: that's what
    // the client waits for. While connecting the attempt decides what's next.
    if (state_ == State::Provisioning) {
        host_.publish_state(state_);
        return;
    }
    std::string url;
    if (host_.online_url(url)) {
        set_state(State::Provisioned);
        publish(kCmdGetCurrentState, {url});
    } else {
        set_state(State::Authorized);
    }
}

void Core::connect_succeeded(const std::string& redirect_url) {
    if (state_ != State::Provisioning) return;
    // The result (with the URL) goes out before the state change, so a client
    // that reacts to "provisioned" can already read where to go.
    publish(kCmdWifiSettings, {redirect_url});
    set_state(State::Provisioned);
}

void Core::connect_failed() {
    if (state_ != State::Provisioning) return;
    set_error(Error::UnableToConnect);
    set_state(State::Authorized);
}

void RpcAssembler::reset() noexcept {
    wipe(buf_);
    buf_.clear();
    complete_ = false;
}

RpcAssembler::Status RpcAssembler::push(std::span<const uint8_t> chunk) {
    if (complete_) reset();
    buf_.insert(buf_.end(), chunk.begin(), chunk.end());
    if (buf_.size() > kMaxPacket) {
        reset();
        return Status::TooLong;
    }
    if (buf_.size() < 2) return Status::Incomplete;
    const size_t need = size_t{buf_[1]} + 3;  // command, length, data, checksum
    if (buf_.size() < need) return Status::Incomplete;
    if (buf_.size() > need) {  // trailing bytes: not a packet we understand
        reset();
        return Status::TooLong;
    }
    complete_ = true;
    return Status::Complete;
}

std::vector<uint8_t> encode_result(uint8_t command, const std::vector<std::string>& strings, Transport transport) {
    const bool checksum = transport == Transport::Ble;
    std::vector<uint8_t> out =
        ::improv::build_rpc_response(static_cast<::improv::Command>(command), strings, checksum);
    // Without a checksum the SDK still leaves room for one (a zero byte).
    if (!checksum && !out.empty()) out.pop_back();
    return out;
}

void wipe(std::span<uint8_t> bytes) noexcept {
    volatile uint8_t* p = bytes.data();
    for (size_t i = 0; i < bytes.size(); ++i) p[i] = 0;
}

void wipe(std::string& s) noexcept {
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
}

std::string chip_name(std::string_view idf_target) {
    std::string out;
    for (const char c : idf_target) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (out.size() > 5 && out.compare(0, 5, "ESP32") == 0) out.insert(5, "-");
    return out;
}

}  // namespace dfu32::improv
