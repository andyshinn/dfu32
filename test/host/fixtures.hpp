// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fixtures {

inline std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

inline std::vector<uint8_t> read_fixture(const std::string& name) {
    return read_file(std::string(DFU32_FIXTURES_DIR) + "/" + name);
}

}  // namespace fixtures
