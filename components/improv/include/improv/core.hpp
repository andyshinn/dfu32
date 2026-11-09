// SPDX-License-Identifier: MIT
//
// Transport-agnostic Improv Wi-Fi core (SPEC.md §9.2). The transport (Improv
// BLE or Improv Serial) hands it complete RPC packets and publishes what it
// reports; the device side connects to the network and reports back.
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
inline constexpr uint8_t kCmdIdentify = 0x02;         // BLE only
inline constexpr uint8_t kCmdGetCurrentState = 0x02;  // Serial only (same value as identify)
inline constexpr uint8_t kCmdGetDeviceInfo = 0x03;
inline constexpr uint8_t kCmdGetWifiNetworks = 0x04;  // Serial only

/// The two transports differ in a few details: a BLE RPC packet ends with its
/// own checksum (a serial frame has one around it instead), command 0x02 is
/// "identify" on BLE but "get current state" on Serial, and only Serial lists
/// WiFi networks.
enum class Transport : uint8_t { Ble, Serial };

/// Largest RPC packet accepted (a WiFi-settings packet is at most 101 bytes).
inline constexpr size_t kMaxPacket = 200;

struct Network {
    std::string ssid;
    int rssi = 0;       // dBm
    bool secured = false;
};

struct DeviceInfo {
    std::string firmware;   // "DFU32"
    std::string version;    // git describe string
    std::string chip;       // "ESP32-S3"
    std::string name;       // hostname
    std::string board;      // DFU32_BOARD profile, "feather_esp32s3"; empty if unknown
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
    /// Serial "get current state": true with the device's URL while it is on
    /// a network (it then reports provisioned).
    virtual bool online_url(std::string& url) {
        (void)url;
        return false;
    }
    /// Serial "get WiFi networks": scans (may block for a few seconds).
    virtual std::vector<Network> scan_networks() { return {}; }
};

class Core {
public:
    explicit Core(IHost& host, Transport transport = Transport::Ble) : host_(host), transport_(transport) {}

    /// Begins a session: Authorized (DFU32 has no authorisation step; the
    /// window itself is the physical control, SPEC.md §9.2). `announce`
    /// publishes the state; Improv Serial starts quietly at boot and answers
    /// only when asked, so a terminal doesn't show a frame on every boot.
    void start(bool announce = true);
    void stop();

    /// One complete RPC packet: command, length, data, and (BLE only) checksum.
    void handle_rpc(std::span<const uint8_t> packet);

    /// From the device, after begin_connect().
    void connect_succeeded(const std::string& redirect_url);
    void connect_failed();

    State state() const noexcept { return state_; }
    Error error() const noexcept { return error_; }

private:
    void set_state(State s);
    void set_error(Error e);
    void publish(uint8_t command, const std::vector<std::string>& strings);
    void report_current_state();

    IHost& host_;
    Transport transport_;
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
    /// Clears (and wipes) the buffer.
    void reset() noexcept;

private:
    std::vector<uint8_t> buf_;
    bool complete_ = false;
};

/// Encodes an RPC result (command + strings), as the SDK does: with a
/// trailing checksum for BLE, without one for Serial.
std::vector<uint8_t> encode_result(uint8_t command, const std::vector<std::string>& strings,
                                   Transport transport = Transport::Ble);

/// Overwrites buffers that held a WiFi passphrase before they are freed
/// (through a volatile pointer, so the stores aren't optimised away).
void wipe(std::span<uint8_t> bytes) noexcept;
void wipe(std::string& s) noexcept;

/// "ESP32-S3" from CONFIG_IDF_TARGET's "esp32s3".
std::string chip_name(std::string_view idf_target);

/// Device info's "hardware chip/variant" string: "ESP32-S3 / feather_esp32s3",
/// or just the chip when the board is unknown. Improv has no board field, and
/// the installer page reads the board from here to pick its manifest, because
/// two board profiles share each chip (SPEC.md §9.2, §9.4).
std::string hardware_variant(std::string_view chip, std::string_view board);

}  // namespace dfu32::improv
