// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/crc32c.hpp"

#include <array>

namespace pff {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc32c_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256u; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = ((crc & 1u) != 0u) ? (0x82F63B78u ^ (crc >> 1)) : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();
constexpr std::uint32_t kCrc32cInit = 0xFFFFFFFFu;
constexpr std::uint32_t kCrc32cXorOut = 0xFFFFFFFFu;

}  // namespace

std::uint32_t crc32c_update(std::uint32_t seed, std::span<const std::byte> data) noexcept {
  std::uint32_t crc = seed;
  for (const std::byte raw : data) {
    const auto index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint32_t>(raw));
    crc = kCrc32cTable[index] ^ (crc >> 8);
  }
  return crc;
}

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  return crc32c_update(kCrc32cInit, data) ^ kCrc32cXorOut;
}

std::uint64_t fnv1a64(std::span<const std::byte> data) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  for (const std::byte raw : data) {
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(raw));
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace pff
