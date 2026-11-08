// SPDX-License-Identifier: MIT
#include "improv/core.hpp"

#include <cctype>

#include "improv.h"  // vendored improv-wifi/sdk-cpp

namespace dfu32::improv {

static_assert(static_cast<uint8_t>(State::Provisioned) == ::improv::STATE_PROVISIONED);
static_assert(static_cast<uint8_t>(Error::UnableToConnect) == ::improv::ERROR_UNABLE_TO_CONNECT);
static_assert(kCmdWifiSettings == ::improv::WIFI_SETTINGS && kCmdIdentify == ::improv::IDENTIFY &&
              kCmdGetDeviceInfo == ::improv::GET_DEVICE_INFO);

void Core::set_state(State s) {
    state_ = s;
    host_.publish_state(s);
}

void Core::set_error(Error e) {
    error_ = e;
    host_.publish_error(e);
}

void Core::start() {
    error_ = Error::None;
    set_state(State::Authorized);
}

void Core::stop() {
    if (state_ != State::Stopped) set_state(State::Stopped);
}

void Core::handle_rpc(std::span<const uint8_t> packet) {
    if (state_ == State::Stopped) return;
    if (error_ != Error::None) set_error(Error::None);  // a new command clears the last error
    if (packet.size() < 3 || packet.size() > kMaxPacket) {
        set_error(Error::InvalidRpc);
        return;
    }
    // One spare zero byte: the SDK reads data[ssid_end] before checking it is
    // in range (README.md next to the vendored copy).
    std::vector<uint8_t> copy(packet.begin(), packet.end());
    copy.push_back(0);
    ::improv::ImprovCommand cmd = ::improv::parse_improv_data(copy.data(), packet.size(), true);
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
        case ::improv::IDENTIFY:
            host_.identify();
            return;
        case ::improv::GET_DEVICE_INFO: {
            const DeviceInfo d = host_.device_info();
            const auto out = encode_result(kCmdGetDeviceInfo, {d.firmware, d.version, d.chip, d.name});
            host_.publish_result(out);
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

void Core::connect_succeeded(const std::string& redirect_url) {
    if (state_ != State::Provisioning) return;
    // The result (with the URL) goes out before the state change, so a client
    // that reacts to "provisioned" can already read where to go.
    host_.publish_result(encode_result(kCmdWifiSettings, {redirect_url}));
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

std::vector<uint8_t> encode_result(uint8_t command, const std::vector<std::string>& strings) {
    return ::improv::build_rpc_response(static_cast<::improv::Command>(command), strings, true);
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
