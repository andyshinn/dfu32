// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "improv/core.hpp"
#include "improv/serial_frame.hpp"

using namespace dfu32::improv;
using Event = SerialFramer::Event;

namespace {

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

std::string text(const std::vector<uint8_t>& v) { return {v.begin(), v.end()}; }

// A frame as the browser client sends it: header, data, checksum, newline.
std::vector<uint8_t> client_frame(uint8_t type, const std::vector<uint8_t>& data, bool newline = true) {
    std::vector<uint8_t> f = encode_serial_frame(type, data);
    if (newline) f.push_back('\n');
    return f;
}

std::vector<uint8_t> rpc(uint8_t cmd, const std::vector<uint8_t>& data = {}) {
    std::vector<uint8_t> p{cmd, static_cast<uint8_t>(data.size())};
    p.insert(p.end(), data.begin(), data.end());
    return p;
}

std::vector<uint8_t> wifi_rpc(const std::string& ssid, const std::string& pass) {
    std::vector<uint8_t> d{static_cast<uint8_t>(ssid.size())};
    d.insert(d.end(), ssid.begin(), ssid.end());
    d.push_back(static_cast<uint8_t>(pass.size()));
    d.insert(d.end(), pass.begin(), pass.end());
    return rpc(kCmdWifiSettings, d);
}

struct Fed {
    std::vector<uint8_t> console;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> frames;
    std::vector<Event> drops;
};

// Feeds bytes one by one at `now`, collecting what the framer hands out.
void feed(SerialFramer& f, const std::vector<uint8_t>& in, uint32_t now, Fed& out) {
    for (const uint8_t b : in) {
        const Event e = f.push(b, now, out.console);
        if (e == Event::Frame) {
            out.frames.emplace_back(f.type(), std::vector<uint8_t>(f.data().begin(), f.data().end()));
        } else if (e != Event::None) {
            out.drops.push_back(e);
        }
    }
}

}  // namespace

TEST_CASE("Improv Serial: frame encoding", "[improv][serial]") {
    const auto f = encode_serial_frame(kSerialRpc, rpc(kCmdGetCurrentState));
    // "IMPROV", version 1, type 3, length 2, data 02 00, checksum
    REQUIRE(f.size() == 12);
    CHECK(text({f.begin(), f.begin() + 6}) == "IMPROV");
    CHECK(f[6] == 1);
    CHECK(f[7] == kSerialRpc);
    CHECK(f[8] == 2);
    CHECK(f[9] == 0x02);
    CHECK(f[10] == 0x00);
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < f.size(); ++i) sum += f[i];
    CHECK(f.back() == static_cast<uint8_t>(sum));
    CHECK(encode_serial_frame(kSerialRpc, std::vector<uint8_t>(256)).empty());
}

TEST_CASE("Improv Serial: plain console input passes straight through", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    feed(f, bytes("help\r"), 0, out);
    CHECK(text(out.console) == "help\r");
    CHECK(out.frames.empty());
    CHECK(f.idle());
    CHECK(f.wait_ms(0) == SerialFramer::kNoDeadline);
}

TEST_CASE("Improv Serial: a frame is taken out of the stream, with its newline", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    feed(f, bytes("run 3\r"), 0, out);
    feed(f, client_frame(kSerialRpc, rpc(kCmdGetDeviceInfo)), 10, out);
    feed(f, bytes("runs\r"), 20, out);
    CHECK(text(out.console) == "run 3\rruns\r");
    REQUIRE(out.frames.size() == 1);
    CHECK(out.frames[0].first == kSerialRpc);
    CHECK(out.frames[0].second == rpc(kCmdGetDeviceInfo));
    CHECK(out.drops.empty());
}

TEST_CASE("Improv Serial: only the newline right after a frame is swallowed", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    feed(f, client_frame(kSerialRpc, rpc(kCmdGetCurrentState), false), 0, out);
    feed(f, bytes("\n\n"), 1, out);
    CHECK(text(out.console) == "\n");
    REQUIRE(out.frames.size() == 1);

    // Without a newline, the next byte is console input.
    SerialFramer g;
    Fed out2;
    feed(g, client_frame(kSerialRpc, rpc(kCmdGetCurrentState), false), 0, out2);
    feed(g, bytes("x"), 1, out2);
    CHECK(text(out2.console) == "x");

    // A newline long after the frame is someone pressing Enter.
    SerialFramer h;
    Fed out3;
    feed(h, client_frame(kSerialRpc, rpc(kCmdGetCurrentState), false), 0, out3);
    CHECK(h.wait_ms(0) == SerialFramer::kFrameTimeoutMs);
    CHECK(h.poll(SerialFramer::kFrameTimeoutMs, out3.console) == Event::None);
    CHECK(h.idle());
    feed(h, bytes("\n"), SerialFramer::kFrameTimeoutMs + 1, out3);
    CHECK(text(out3.console) == "\n");
}

TEST_CASE("Improv Serial: back-to-back frames", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    auto a = client_frame(kSerialRpc, rpc(kCmdGetCurrentState));
    const auto b = client_frame(kSerialRpc, wifi_rpc("attic", "hunter2hunter2"));
    a.insert(a.end(), b.begin(), b.end());
    feed(f, a, 0, out);
    REQUIRE(out.frames.size() == 2);
    CHECK(out.frames[1].second == wifi_rpc("attic", "hunter2hunter2"));
    CHECK(out.console.empty());
}

TEST_CASE("Improv Serial: a partial header goes back to the console", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    // Mismatch: "IMPR" was typed, then "ESS".
    feed(f, bytes("IMPRESS\r"), 0, out);
    CHECK(text(out.console) == "IMPRESS\r");

    // A mismatch on an 'I' starts a new header with it.
    SerialFramer g;
    Fed out2;
    auto in = bytes("IM");  // then a full frame, starting with 'I'
    const auto fr = client_frame(kSerialRpc, rpc(kCmdGetCurrentState));
    in.insert(in.end(), fr.begin(), fr.end());
    feed(g, in, 0, out2);
    CHECK(text(out2.console) == "IM");
    CHECK(out2.frames.size() == 1);
}

TEST_CASE("Improv Serial: held header bytes are released after a pause", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    feed(f, bytes("IMP"), 1000, out);
    CHECK(out.console.empty());
    CHECK(f.wait_ms(1000) == SerialFramer::kHeaderHoldMs);
    CHECK(f.wait_ms(1060) == SerialFramer::kHeaderHoldMs - 60);
    CHECK(f.poll(1060, out.console) == Event::None);
    CHECK(out.console.empty());
    CHECK(f.poll(1000 + SerialFramer::kHeaderHoldMs, out.console) == Event::None);
    CHECK(text(out.console) == "IMP");
    CHECK(f.idle());

    // A late byte releases them too, before it is handled itself.
    SerialFramer g;
    Fed out2;
    feed(g, bytes("I"), 0, out2);
    feed(g, bytes("M"), SerialFramer::kHeaderHoldMs + 5, out2);
    CHECK(text(out2.console) == "IM");  // the 'I' first, then 'M' (not a header start)
}

TEST_CASE("Improv Serial: a bad checksum drops the frame without leaking it", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    auto bad = client_frame(kSerialRpc, wifi_rpc("attic", "hunter2hunter2"));
    bad[bad.size() - 2] ^= 0x55;  // the checksum (before the newline)
    feed(f, bad, 0, out);
    feed(f, bytes("wifi\r"), 1, out);
    CHECK(out.frames.empty());
    REQUIRE(out.drops.size() == 1);
    CHECK(out.drops[0] == Event::BadChecksum);
    CHECK(text(out.console) == "wifi\r");
}

TEST_CASE("Improv Serial: an unknown version is dropped", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    auto fr = encode_serial_frame(kSerialRpc, rpc(kCmdGetCurrentState));
    fr[6] = 2;
    fr.back() = static_cast<uint8_t>(fr.back() + 1);  // keep the checksum right
    feed(f, fr, 0, out);
    REQUIRE(out.drops.size() == 1);
    CHECK(out.drops[0] == Event::BadVersion);
    CHECK(out.console.empty());
}

TEST_CASE("Improv Serial: an oversized frame is read to its end and dropped", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    std::vector<uint8_t> big(SerialFramer::kMaxData + 1, 'A');
    auto fr = encode_serial_frame(kSerialRpc, big);
    fr.push_back('\n');
    feed(f, fr, 0, out);
    feed(f, bytes("ok\r"), 1, out);
    REQUIRE(out.drops.size() == 1);
    CHECK(out.drops[0] == Event::TooLong);
    CHECK(text(out.console) == "ok\r");  // none of the 'A's
}

TEST_CASE("Improv Serial: a cut-off frame times out and the console works again", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    auto fr = client_frame(kSerialRpc, wifi_rpc("attic", "hunter2hunter2"));
    fr.resize(14);  // header and part of the data
    feed(f, fr, 0, out);
    CHECK(f.wait_ms(100) == SerialFramer::kFrameTimeoutMs - 100);
    CHECK(f.poll(SerialFramer::kFrameTimeoutMs - 1, out.console) == Event::None);
    CHECK(f.poll(SerialFramer::kFrameTimeoutMs, out.console) == Event::TimedOut);
    CHECK(f.idle());
    CHECK(f.wait_ms(SerialFramer::kFrameTimeoutMs) == SerialFramer::kNoDeadline);
    feed(f, bytes("\r"), 300, out);  // not swallowed after a cut-off frame
    feed(f, bytes("help\r"), 301, out);
    CHECK(text(out.console) == "\rhelp\r");
    CHECK(out.frames.empty());
}

TEST_CASE("Improv Serial: a byte after a long gap ends the stale frame first", "[improv][serial]") {
    SerialFramer f;
    std::vector<uint8_t> console;
    auto fr = client_frame(kSerialRpc, rpc(kCmdGetCurrentState));
    for (size_t i = 0; i < 10; ++i) f.push(fr[i], 0, console);  // header and one data byte
    CHECK(f.push('h', 1000, console) == Event::TimedOut);
    CHECK(text(console) == "h");
}

TEST_CASE("Improv Serial: the clock may wrap", "[improv][serial]") {
    SerialFramer f;
    Fed out;
    const uint32_t start = UINT32_MAX - 10;
    feed(f, bytes("IMP"), start, out);
    CHECK(f.wait_ms(start + 20) == SerialFramer::kHeaderHoldMs - 20);  // wrapped to 9
    CHECK(f.poll(start + SerialFramer::kHeaderHoldMs, out.console) == Event::None);
    CHECK(text(out.console) == "IMP");
}

TEST_CASE("Improv Serial: frame data is wiped once read", "[improv][serial]") {
    SerialFramer f;
    std::vector<uint8_t> console;
    const auto fr = client_frame(kSerialRpc, wifi_rpc("attic", "hunter2hunter2"), false);
    Event e = Event::None;
    for (const uint8_t b : fr) e = f.push(b, 0, console);
    REQUIRE(e == Event::Frame);
    const uint8_t* p = f.data().data();
    const size_t n = f.data().size();
    CHECK(f.poll(1, console) == Event::None);
    bool zero = true;
    for (size_t i = 0; i < n; ++i) zero = zero && p[i] == 0;
    CHECK(zero);
}

// ---- the core over Serial ---------------------------------------------------------

namespace {

struct SerialHost : IHost {
    std::vector<State> states;
    std::vector<Error> errors;
    std::vector<std::vector<uint8_t>> results;
    std::string url;  // empty: offline
    std::vector<Network> networks;
    int connects = 0, identifies = 0, scans = 0;

    void publish_state(State s) override { states.push_back(s); }
    void publish_error(Error e) override { errors.push_back(e); }
    void publish_result(std::span<const uint8_t> p) override { results.emplace_back(p.begin(), p.end()); }
    bool begin_connect(const std::string&, const std::string&) override {
        ++connects;
        return true;
    }
    void identify() override { ++identifies; }
    DeviceInfo device_info() override { return {"DFU32", "v0.5.0", "ESP32-S3", "dfu32-ab12", "esp32s3_n16r8"}; }
    bool online_url(std::string& out) override {
        out = url;
        return !url.empty();
    }
    std::vector<Network> scan_networks() override {
        ++scans;
        return networks;
    }
};

// A serial result: command, length, strings (no checksum).
std::pair<uint8_t, std::vector<std::string>> decode(const std::vector<uint8_t>& r) {
    REQUIRE(r.size() >= 2);
    REQUIRE(r[1] == r.size() - 2);
    std::vector<std::string> out;
    size_t i = 2;
    while (i < r.size()) {
        const size_t n = r[i];
        REQUIRE(i + 1 + n <= r.size());
        out.emplace_back(r.begin() + static_cast<std::ptrdiff_t>(i + 1),
                         r.begin() + static_cast<std::ptrdiff_t>(i + 1 + n));
        i += 1 + n;
    }
    return {r[0], out};
}

}  // namespace

TEST_CASE("Improv Serial core: current state, offline and online", "[improv][serial]") {
    SerialHost h;
    Core c(h, Transport::Serial);
    c.start();
    h.states.clear();

    c.handle_rpc(rpc(kCmdGetCurrentState));
    REQUIRE(h.states == std::vector<State>{State::Authorized});
    CHECK(h.results.empty());
    CHECK(h.identifies == 0);  // 0x02 is not identify on Serial

    h.url = "http://192.168.1.50/";
    c.handle_rpc(rpc(kCmdGetCurrentState));
    CHECK(h.states.back() == State::Provisioned);
    REQUIRE(h.results.size() == 1);
    const auto [cmd, strs] = decode(h.results[0]);
    CHECK(cmd == kCmdGetCurrentState);
    CHECK(strs == std::vector<std::string>{"http://192.168.1.50/"});

    // Back offline (router gone): ready for new settings again.
    h.url.clear();
    c.handle_rpc(rpc(kCmdGetCurrentState));
    CHECK(h.states.back() == State::Authorized);
}

TEST_CASE("Improv Serial core: current state while connecting", "[improv][serial]") {
    SerialHost h;
    Core c(h, Transport::Serial);
    c.start();
    c.handle_rpc(wifi_rpc("attic", "hunter2hunter2"));
    REQUIRE(c.state() == State::Provisioning);
    h.url = "http://192.168.1.50/";  // still the old network's address
    h.states.clear();
    c.handle_rpc(rpc(kCmdGetCurrentState));
    CHECK(h.states == std::vector<State>{State::Provisioning});
    CHECK(h.results.empty());
    CHECK(c.state() == State::Provisioning);
}

TEST_CASE("Improv Serial core: WiFi settings without an inner checksum", "[improv][serial]") {
    SerialHost h;
    Core c(h, Transport::Serial);
    c.start();
    c.handle_rpc(wifi_rpc("attic", "hunter2hunter2"));
    CHECK(h.connects == 1);
    c.connect_succeeded("http://192.168.1.77/");
    CHECK(c.state() == State::Provisioned);
    REQUIRE(h.results.size() == 1);
    const auto [cmd, strs] = decode(h.results[0]);
    CHECK(cmd == kCmdWifiSettings);
    CHECK(strs == std::vector<std::string>{"http://192.168.1.77/"});

    // Changing WiFi later is allowed (the installer's "Change Wi-Fi").
    c.handle_rpc(wifi_rpc("garage", "correcthorse"));
    CHECK(h.connects == 2);
    c.connect_failed();
    CHECK(h.errors.back() == Error::UnableToConnect);
    CHECK(c.state() == State::Authorized);
}

TEST_CASE("Improv Serial core: device info", "[improv][serial]") {
    SerialHost h;
    Core c(h, Transport::Serial);
    c.start();
    c.handle_rpc(rpc(kCmdGetDeviceInfo));
    REQUIRE(h.results.size() == 1);
    const auto [cmd, strs] = decode(h.results[0]);
    CHECK(cmd == kCmdGetDeviceInfo);
    CHECK(strs == std::vector<std::string>{"DFU32", "v0.5.0", "ESP32-S3 / esp32s3_n16r8", "dfu32-ab12"});
}

TEST_CASE("Improv Serial core: WiFi networks, then an empty result", "[improv][serial]") {
    SerialHost h;
    h.networks = {{"attic", -48, true}, {"guest", -71, false}};
    Core c(h, Transport::Serial);
    c.start();
    c.handle_rpc(rpc(kCmdGetWifiNetworks));
    CHECK(h.scans == 1);
    REQUIRE(h.results.size() == 3);
    CHECK(decode(h.results[0]).second == std::vector<std::string>{"attic", "-48", "YES"});
    CHECK(decode(h.results[1]).second == std::vector<std::string>{"guest", "-71", "NO"});
    const auto [cmd, strs] = decode(h.results[2]);
    CHECK(cmd == kCmdGetWifiNetworks);
    CHECK(strs.empty());

    // A failed scan is just an empty list.
    h.networks.clear();
    h.results.clear();
    c.handle_rpc(rpc(kCmdGetWifiNetworks));
    REQUIRE(h.results.size() == 1);
    CHECK(decode(h.results[0]).second.empty());
}

TEST_CASE("Improv Serial core: malformed and BLE-only packets", "[improv][serial]") {
    SerialHost h;
    Core c(h, Transport::Serial);
    c.start();
    c.handle_rpc(std::vector<uint8_t>{kCmdGetDeviceInfo});  // no length byte
    CHECK(h.errors.back() == Error::InvalidRpc);
    c.handle_rpc(std::vector<uint8_t>{kCmdGetDeviceInfo, 5});  // length disagrees
    CHECK(h.errors.back() == Error::InvalidRpc);
    c.handle_rpc(rpc(0x09));
    CHECK(h.errors.back() == Error::UnknownRpc);
    // A BLE-style packet (trailing checksum) has one byte too many here.
    auto ble = rpc(kCmdGetDeviceInfo);
    ble.push_back(static_cast<uint8_t>(ble[0] + ble[1]));
    c.handle_rpc(ble);
    CHECK(h.errors.back() == Error::InvalidRpc);
    CHECK(h.results.empty());
}

TEST_CASE("Improv BLE core: no WiFi network list", "[improv]") {
    SerialHost h;
    Core c(h, Transport::Ble);
    c.start();
    auto p = rpc(kCmdGetWifiNetworks);
    p.push_back(static_cast<uint8_t>(p[0] + p[1]));
    c.handle_rpc(p);
    CHECK(h.errors.back() == Error::UnknownRpc);
    CHECK(h.scans == 0);
}

TEST_CASE("Improv Serial core: a quiet start publishes nothing", "[improv][serial]") {
    SerialHost h;
    Core c(h, Transport::Serial);
    c.start(false);
    CHECK(c.state() == State::Authorized);
    CHECK(h.states.empty());
    c.handle_rpc(rpc(kCmdGetCurrentState));
    CHECK(h.states == std::vector<State>{State::Authorized});
}
