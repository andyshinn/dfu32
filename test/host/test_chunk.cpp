// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include "dfu_proto/chunk.hpp"

using dfu::packet_size_for_mtu;
using dfu::plan_chunks;

TEST_CASE("Packet size from ATT MTU", "[chunk]") {
    CHECK(packet_size_for_mtu(22) == 0);
    CHECK(packet_size_for_mtu(23) == 20);
    CHECK(packet_size_for_mtu(24) == 20);
    CHECK(packet_size_for_mtu(185) == 180);  // 182 rounded down to a multiple of 4
    CHECK(packet_size_for_mtu(247) == 244);
    CHECK(packet_size_for_mtu(251) == 244);  // capped by characteristic length
    CHECK(packet_size_for_mtu(517) == 244);
}

TEST_CASE("Legacy bootloader caps packets at 20 bytes", "[chunk]") {
    CHECK(packet_size_for_mtu(247, dfu::kPacketMaxLegacy) == 20);
    CHECK(packet_size_for_mtu(23, dfu::kPacketMaxLegacy) == 20);
}

TEST_CASE("Packet size is always a multiple of 4", "[chunk]") {
    for (uint16_t mtu = 23; mtu <= 600; ++mtu) {
        const uint16_t n = packet_size_for_mtu(mtu);
        INFO("mtu=" << mtu);
        REQUIRE(n % 4 == 0);
        REQUIRE(n >= 20);
        REQUIRE(n <= mtu - 3);
    }
}

TEST_CASE("Chunk plan for real MeshCore image sizes", "[chunk]") {
    dfu::ChunkPlan plan;
    REQUIRE(plan_chunks(440924, 244, plan));  // RAK4631 repeater v1.18.0
    CHECK(plan.packet_count == 1808);
    CHECK(plan.last_packet_size == 16);
    CHECK(static_cast<uint64_t>(plan.packet_count - 1) * 244 + plan.last_packet_size == 440924);

    REQUIRE(plan_chunks(360768, 20, plan));  // XIAO, legacy 20-byte packets
    CHECK(plan.packet_count == 18039);
    CHECK(plan.last_packet_size == 8);

    REQUIRE(plan_chunks(488, 244, plan));
    CHECK(plan.packet_count == 2);
    CHECK(plan.last_packet_size == 244);
}

TEST_CASE("Chunk plan rejects invalid inputs", "[chunk]") {
    dfu::ChunkPlan plan;
    CHECK_FALSE(plan_chunks(0, 244, plan));
    CHECK_FALSE(plan_chunks(440926, 244, plan));  // not word aligned
    CHECK_FALSE(plan_chunks(440924, 0, plan));
    CHECK_FALSE(plan_chunks(440924, 182, plan));  // packet not word aligned
}
