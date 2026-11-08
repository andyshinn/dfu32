// SPDX-License-Identifier: MIT
//
// Transport-agnostic Improv Wi-Fi core (SPEC.md §9.2). The transport (Improv
// BLE now, Improv Serial in M5) hands it complete RPC packets and publishes
// what it reports; the device side connects to the network and reports back.
// Packet parsing uses the vendored improv-wifi/sdk-cpp. Pure C++, host-tested.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace dfu32::improv {

// Values from the Improv spec (identical to improv::State / improv::Error in
// the SDK; repeated here so users of this header don't pull the SDK in).
enum class State : uint8_t {
    Stopped = 0x00,
    AwaitingAuthorization = 0x01,
    Authorized = 0x02,
    Provisioning = 0x03,
    Provisioned = 0x04,
};

enum class Error : uint8_t {
    None = 0x00,
    InvalidRpc = 0x01,
    UnknownRpc = 0x02,
    UnableToConnect = 0x03,
    NotAuthorized = 0x04,
    Unknown = 0xFF,
};

// RPC commands DFU32 answers.
inline constexpr uint8_t kCmdWifiSettings = 0x01;
inline constexpr uint8_t kCmdIdentify = 0x02;     // BLE; on Serial 0x02 is "get current state"
inline constexpr uint8_t kCmdGetDeviceInfo = 0x03;

/// Largest RPC packet accepted (a WiFi-settings packet is at most 101 bytes).
inline constexpr size_t kMaxPacket = 200;

struct DeviceInfo {
    std::string firmware;   // "DFU32"
    std::string version;    // git describe string
    std::string chip;       // "ESP32-S3"
    std::string name;       // hostname
};

/// What the core needs from the device and the transport. Called with the
/// core's caller's context (no locking inside the core).
class IHost {
public:
    virtual ~IHost() = default;
    virtual void publish_state(State s) = 0;
    virtual void publish_error(Error e) = 0;
    /// An encoded RPC result packet (with checksum) for the transport to send.
    virtual void publish_result(std::span<const uint8_t> packet) = 0;
    /// Start connecting; must not block. Report with Core::connect_succeeded/
    /// failed. Return false if the attempt can't even start: the core then
    /// reports UNABLE_TO_CONNECT and accepts new settings right away.
    virtual bool begin_connect(const std::string& ssid, const std::string& password) = 0;
    virtual void identify() = 0;
    virtual DeviceInfo device_info() = 0;
};

class Core {
public:
    explicit Core(IHost& host) : host_(host) {}

    /// Begins a session: Authorized (DFU32 has no authorisation step; the
    /// window itself is the physical control, SPEC.md §9.2).
    void start();
    void stop();

    /// One complete RPC packet: command, length, data, checksum.
    void handle_rpc(std::span<const uint8_t> packet);

    /// From the device, after begin_connect().
    void connect_succeeded(const std::string& redirect_url);
    void connect_failed();

    State state() const noexcept { return state_; }
    Error error() const noexcept { return error_; }

private:
    void set_state(State s);
    void set_error(Error e);

    IHost& host_;
    State state_ = State::Stopped;
    Error error_ = Error::None;
};

/// Gathers an RPC packet that a BLE client may split across several writes.
class RpcAssembler {
public:
    enum class Status : uint8_t { Incomplete, Complete, TooLong };
    /// Appends a write. Complete: packet() holds exactly one packet, and the
    /// next push() starts a new one. TooLong: the buffer was discarded.
    Status push(std::span<const uint8_t> chunk);
    std::span<const uint8_t> packet() const { return buf_; }
    void reset() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
    bool complete_ = false;
};

/// Encodes an RPC result (command + strings, with checksum), as the SDK does.
std::vector<uint8_t> encode_result(uint8_t command, const std::vector<std::string>& strings);

/// "ESP32-S3" from CONFIG_IDF_TARGET's "esp32s3".
std::string chip_name(std::string_view idf_target);

}  // namespace dfu32::improv
