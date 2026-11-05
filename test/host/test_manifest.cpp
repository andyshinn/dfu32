// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>

#include "dfu32_core/manifest.hpp"

using dfu32::Err;
using dfu32::Manifest;
using dfu32::parse_manifest;

namespace {

constexpr const char* kUrl = "http://fw.lan/meshcore/rak4631/manifest.json";
const std::string kSha(64, 'a');

std::string manifest(const std::string& extra = "", const std::string& boards = R"(["rak4631","RAK_4631"])",
                     const std::string& schema = "1", const std::string& dat_size = "14") {
    return R"({"schema":)" + schema + R"(,"name":"MeshCore repeater","version":"v1.18.0-81dbae5","boards":)" + boards +
           R"(,"created":"2026-10-15T12:00:00Z","bin":{"url":"firmware.bin","size":786432,"sha256":")" + kSha +
           R"("},"dat":{"url":"../dat/firmware.dat","size":)" + dat_size + R"(,"sha256":")" + kSha + R"("})" + extra +
           "}";
}

}  // namespace

TEST_CASE("Manifest: SPEC example parses", "[manifest]") {
    Manifest m;
    const auto s = parse_manifest(manifest(R"(,"init":{"device_type":82,"sd_req":[182,256],"crc16":4660},"notes":"n","x":{"y":1})"),
                                  kUrl, m);
    INFO(s.str());
    REQUIRE(s.ok());
    CHECK(m.schema == 1);
    CHECK(m.version == "v1.18.0-81dbae5");
    CHECK(m.name == "MeshCore repeater");
    CHECK(m.boards.size() == 2);
    CHECK(m.bin.url == "http://fw.lan/meshcore/rak4631/firmware.bin");
    CHECK(m.dat.url == "http://fw.lan/meshcore/dat/firmware.dat");
    CHECK(m.bin.size == 786432);
    CHECK(m.dat.size == 14);
    CHECK(m.bin.sha256[0] == 0xAA);
    CHECK(m.init.present);
    CHECK(m.init.device_type == 82);
    CHECK(m.init.sd_req.size() == 2);
    CHECK(m.init.crc16 == 4660);
    CHECK(m.notes == "n");
}

TEST_CASE("Manifest: board matching is exact and case-insensitive", "[manifest]") {
    Manifest m;
    REQUIRE(parse_manifest(manifest(), kUrl, m).ok());
    CHECK(dfu32::board_matches(m, "RAK4631"));
    CHECK(dfu32::board_matches(m, "rak_4631"));
    CHECK_FALSE(dfu32::board_matches(m, "rak463"));
    CHECK_FALSE(dfu32::board_matches(m, ""));
}

TEST_CASE("Manifest: schema handling", "[manifest]") {
    Manifest m;
    CHECK(parse_manifest(manifest("", R"(["x"])", "2"), kUrl, m).code == Err::ManifestUnsupported);
    CHECK(parse_manifest(manifest("", R"(["x"])", "0"), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest("", R"(["x"])", "\"1\""), kUrl, m).code == Err::ManifestInvalid);
}

TEST_CASE("Manifest: invalid documents", "[manifest]") {
    Manifest m;
    CHECK(parse_manifest("not json", kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest("[]", kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest("", "[]"), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest("", R"([""])"), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest("", R"([1])"), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest("", "\"rak4631\""), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest("", R"(["x"])", "1", "0"), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest("", R"(["x"])", "1", "65"), kUrl, m).code == Err::DatTooLong);
    CHECK(parse_manifest(manifest(R"(,"init":{"device_type":70000})"), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest(R"(,"init":{"sd_req":[]})"), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest(R"(,"init":[])"), kUrl, m).code == Err::ManifestInvalid);
    CHECK(parse_manifest(manifest(R"(,"notes":5)"), kUrl, m).code == Err::ManifestInvalid);
    // A relative URL against an https manifest URL is refused (v1 is HTTP only).
    CHECK(parse_manifest(manifest(), "https://fw.lan/m.json", m).code == Err::ManifestInvalid);
    // Too large.
    CHECK(parse_manifest(manifest(",\"notes\":\"" + std::string(9000, 'x') + "\""), kUrl, m).code ==
          Err::ManifestInvalid);

    std::string bad_sha = manifest();
    bad_sha.replace(bad_sha.find(kSha), 64, std::string(64, 'z'));
    CHECK(parse_manifest(bad_sha, kUrl, m).code == Err::ManifestInvalid);

    std::string long_ver = manifest();
    long_ver.replace(long_ver.find("v1.18.0-81dbae5"), 15, std::string(64, 'v'));
    const auto s = parse_manifest(long_ver, kUrl, m);
    CHECK(s.code == Err::ManifestInvalid);
    CHECK(s.detail.find("version") != std::string::npos);
}

TEST_CASE("Manifest: init block cross-check against the .dat", "[manifest]") {
    Manifest m;
    REQUIRE(parse_manifest(manifest(R"(,"init":{"device_type":82,"sd_req":[182],"crc16":4660})"), kUrl, m).ok());
    dfu::InitPacket init;
    init.device_type = 82;
    init.sd_req_count = 1;
    init.sd_req[0] = 182;
    init.firmware_crc16 = 4660;
    CHECK(dfu32::check_init_against_manifest(m, init).ok());
    init.firmware_crc16 = 1;
    CHECK(dfu32::check_init_against_manifest(m, init).code == Err::DatInvalid);
    init.firmware_crc16 = 4660;
    init.sd_req[0] = 0xB6 + 1;
    CHECK(dfu32::check_init_against_manifest(m, init).code == Err::DatInvalid);
    init.sd_req[0] = 182;
    init.device_type = 84;
    CHECK(dfu32::check_init_against_manifest(m, init).code == Err::DatInvalid);

    Manifest none;
    REQUIRE(parse_manifest(manifest(), kUrl, none).ok());
    CHECK(dfu32::check_init_against_manifest(none, init).ok());  // no init block: nothing to compare
}

TEST_CASE("Manifest: parses what tools/dfu32_pack.py writes", "[manifest]") {
    std::ifstream f(DFU32_FIXTURES_DIR "/pack_manifest_golden.json");
    REQUIRE(f.good());
    std::stringstream ss;
    ss << f.rdbuf();
    Manifest m;
    const auto s = parse_manifest(ss.str(), "http://fw.lan:8000/rak4631/manifest.json", m);
    INFO(s.str());
    REQUIRE(s.ok());
    CHECK(m.version == "v1.18.0-81dbae5");
    CHECK(m.bin.url == "http://fw.lan:8000/rak4631/firmware.bin");
    CHECK(m.bin.size == 64);
    CHECK(m.dat.size == 14);
    CHECK(m.init.present);
    CHECK(m.init.device_type == 0x52);
    CHECK(m.init.crc16 == 18911);
    CHECK(dfu32::board_matches(m, "rak4631"));
}
