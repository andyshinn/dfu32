// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "improv/core.hpp"

using namespace dfu32::improv;

namespace {

struct FakeHost : IHost {
    std::vector<State> states;
    std::vector<Error> errors;
    std::vector<std::vector<uint8_t>> results;
    std::string ssid, password;
    int connects = 0, identifies = 0;
    bool connect_starts = true;

    void publish_state(State s) override { states.push_back(s); }
    void publish_error(Error e) override { errors.push_back(e); }
    void publish_result(std::span<const uint8_t> p) override { results.emplace_back(p.begin(), p.end()); }
    bool begin_connect(const std::string& s, const std::string& p) override {
        ssid = s;
        password = p;
        ++connects;
        return connect_starts;
    }
    void identify() override { ++identifies; }
    DeviceInfo device_info() override { return {"DFU32", "v0.4.0", "ESP32-S3", "dfu32-ab12", "feather_esp32s3"}; }
};

uint8_t checksum(const std::vector<uint8_t>& p) {
    uint32_t sum = 0;
    for (const uint8_t b : p) sum += b;
    return static_cast<uint8_t>(sum);
}

// command, length, data..., checksum
std::vector<uint8_t> packet(uint8_t cmd, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> p{cmd, static_cast<uint8_t>(data.size())};
    p.insert(p.end(), data.begin(), data.end());
    p.push_back(checksum(p));
    return p;
}

std::vector<uint8_t> wifi(const std::string& ssid, const std::string& pass) {
    std::vector<uint8_t> d{static_cast<uint8_t>(ssid.size())};
    d.insert(d.end(), ssid.begin(), ssid.end());
    d.push_back(static_cast<uint8_t>(pass.size()));
    d.insert(d.end(), pass.begin(), pass.end());
    return packet(kCmdWifiSettings, d);
}

// Decodes a result packet into its strings (and checks its checksum).
std::vector<std::string> strings(const std::vector<uint8_t>& r) {
    REQUIRE(r.size() >= 3);
    REQUIRE(r[1] == r.size() - 3);
    REQUIRE(r.back() == checksum(std::vector<uint8_t>(r.begin(), r.end() - 1)));
    std::vector<std::string> out;
    for (size_t i = 2; i < r.size() - 1;) {
        const size_t n = r[i];
        out.emplace_back(r.begin() + static_cast<long>(i + 1), r.begin() + static_cast<long>(i + 1 + n));
        i += 1 + n;
    }
    return out;
}

}  // namespace

TEST_CASE("Improv: provisioning succeeds", "[improv]") {
    FakeHost h;
    Core c(h);
    c.handle_rpc(wifi("Net", "password1"));  // ignored while stopped
    CHECK(h.connects == 0);
    c.start();
    CHECK(c.state() == State::Authorized);
    c.handle_rpc(wifi("MyNetwork", "hunter2hunter2"));
    CHECK(c.state() == State::Provisioning);
    CHECK(h.connects == 1);
    CHECK(h.ssid == "MyNetwork");
    CHECK(h.password == "hunter2hunter2");
    c.connect_succeeded("http://192.168.1.50/");
    CHECK(c.state() == State::Provisioned);
    REQUIRE(h.results.size() == 1);
    CHECK(h.results[0][0] == kCmdWifiSettings);
    CHECK(strings(h.results[0]) == std::vector<std::string>{"http://192.168.1.50/"});
    // The result is published before the state change.
    CHECK(h.states.back() == State::Provisioned);
    c.connect_succeeded("http://x/");  // stray report: ignored
    CHECK(h.results.size() == 1);
}

TEST_CASE("Improv: connection failure reports an error and allows a retry", "[improv]") {
    FakeHost h;
    Core c(h);
    c.start();
    c.handle_rpc(wifi("MyNetwork", "wrongpassword"));
    c.handle_rpc(wifi("MyNetwork", "again-while-busy"));  // still connecting
    CHECK(h.connects == 1);
    CHECK(c.error() == Error::Unknown);
    c.connect_failed();
    CHECK(c.state() == State::Authorized);
    CHECK(c.error() == Error::UnableToConnect);
    c.handle_rpc(wifi("MyNetwork", "rightpassword"));
    CHECK(c.error() == Error::None);  // a new command clears the error
    CHECK(h.connects == 2);
    CHECK(c.state() == State::Provisioning);
}

TEST_CASE("Improv: a connect that can't start doesn't leave the core provisioning", "[improv]") {
    FakeHost h;
    h.connect_starts = false;
    Core c(h);
    c.start();
    c.handle_rpc(wifi("MyNetwork", "password1"));
    CHECK(c.state() == State::Authorized);
    CHECK(c.error() == Error::UnableToConnect);
    h.connect_starts = true;
    c.handle_rpc(wifi("MyNetwork", "password1"));  // accepted again at once
    CHECK(c.state() == State::Provisioning);
    CHECK(h.connects == 2);
}

TEST_CASE("Improv: open networks and credential limits", "[improv]") {
    FakeHost h;
    Core c(h);
    c.start();
    c.handle_rpc(wifi("Open", ""));
    CHECK(h.connects == 1);
    c.connect_failed();
    c.handle_rpc(wifi("", "password1"));
    CHECK(c.error() == Error::InvalidRpc);
    c.handle_rpc(wifi(std::string(33, 's'), "password1"));
    CHECK(c.error() == Error::InvalidRpc);
    c.handle_rpc(wifi("Net", "short"));
    CHECK(c.error() == Error::InvalidRpc);
    c.handle_rpc(wifi("Net", std::string(65, 'p')));
    CHECK(c.error() == Error::InvalidRpc);
    CHECK(h.connects == 1);
}

TEST_CASE("Improv: malformed packets", "[improv]") {
    FakeHost h;
    Core c(h);
    c.start();
    auto p = wifi("Net", "password1");
    p.back() ^= 0xFF;
    c.handle_rpc(p);
    CHECK(c.error() == Error::InvalidRpc);  // bad checksum
    c.handle_rpc(std::vector<uint8_t>{0x01, 0x05, 0x00});
    CHECK(c.error() == Error::InvalidRpc);  // length field disagrees
    c.handle_rpc(std::vector<uint8_t>{0x01});
    CHECK(c.error() == Error::InvalidRpc);
    c.handle_rpc(std::vector<uint8_t>(kMaxPacket + 1, 0));
    CHECK(c.error() == Error::InvalidRpc);
    // SSID length pointing exactly at the end of the packet (the SDK would read
    // past the buffer without DFU32's padding).
    c.handle_rpc(packet(kCmdWifiSettings, {0x04, 'a', 'b', 'c'}));
    CHECK(c.error() == Error::InvalidRpc);
    // SSID length past the end.
    c.handle_rpc(packet(kCmdWifiSettings, {0x09, 'a'}));
    CHECK(c.error() == Error::InvalidRpc);
    c.handle_rpc(packet(0x42, {}));
    CHECK(c.error() == Error::UnknownRpc);
    CHECK(h.connects == 0);
    CHECK(c.state() == State::Authorized);
}

TEST_CASE("Improv: identify and device info", "[improv]") {
    FakeHost h;
    Core c(h);
    c.start();
    c.handle_rpc(packet(kCmdIdentify, {}));
    CHECK(h.identifies == 1);
    CHECK(h.results.empty());
    c.handle_rpc(packet(kCmdGetDeviceInfo, {}));
    REQUIRE(h.results.size() == 1);
    CHECK(h.results[0][0] == kCmdGetDeviceInfo);
    CHECK(strings(h.results[0]) ==
          std::vector<std::string>{"DFU32", "v0.4.0", "ESP32-S3 / feather_esp32s3", "dfu32-ab12"});
    c.stop();
    CHECK(c.state() == State::Stopped);
}

TEST_CASE("Improv: RPC packets split across BLE writes", "[improv]") {
    RpcAssembler a;
    const auto p = wifi("MyNetwork", "hunter2hunter2");
    const std::span<const uint8_t> all(p);
    CHECK(a.push(all.subspan(0, 1)) == RpcAssembler::Status::Incomplete);
    CHECK(a.push(all.subspan(1, 10)) == RpcAssembler::Status::Incomplete);
    CHECK(a.push(all.subspan(11)) == RpcAssembler::Status::Complete);
    CHECK(std::vector<uint8_t>(a.packet().begin(), a.packet().end()) == p);
    // The next write starts a new packet.
    CHECK(a.push(all) == RpcAssembler::Status::Complete);
    // Trailing garbage and oversize input are discarded.
    auto longer = p;
    longer.push_back(0);
    CHECK(a.push(longer) == RpcAssembler::Status::TooLong);
    CHECK(a.push(std::vector<uint8_t>(kMaxPacket + 1, 0xFF)) == RpcAssembler::Status::TooLong);
    CHECK(a.packet().empty());
}

TEST_CASE("Improv: chip names", "[improv]") {
    CHECK(chip_name("esp32s3") == "ESP32-S3");
    CHECK(chip_name("esp32") == "ESP32");
    CHECK(chip_name("esp32c6") == "ESP32-C6");
    CHECK(hardware_variant("ESP32-S3", "feather_esp32s3") == "ESP32-S3 / feather_esp32s3");
    CHECK(hardware_variant("ESP32", "rak11200_rak19007") == "ESP32 / rak11200_rak19007");
    CHECK(hardware_variant("ESP32", "") == "ESP32");
}

TEST_CASE("Improv: passphrase buffers are wiped", "[improv]") {
    std::vector<uint8_t> v{'s', 'e', 'c', 'r', 'e', 't'};
    wipe(v);
    CHECK(v == std::vector<uint8_t>(6, 0));
    std::string s = "hunter2hunter2";
    wipe(s);
    CHECK(s == std::string(14, '\0'));

    RpcAssembler a;
    const auto p = wifi("Net", "password1");
    REQUIRE(a.push(std::span<const uint8_t>(p).subspan(0, 5)) == RpcAssembler::Status::Incomplete);
    const uint8_t* data = a.packet().data();
    const size_t n = a.packet().size();
    std::vector<uint8_t> before(data, data + n);
    a.reset();
    CHECK(a.packet().empty());
    // The storage is kept by clear(); its bytes are zero now.
    for (size_t i = 0; i < n; ++i) CHECK(data[i] == 0);
    CHECK(before != std::vector<uint8_t>(n, 0));
}
