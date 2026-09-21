// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <array>
#include <cstdint>
#include <string>

#include "pff/ids.hpp"

namespace pff {

std::string id_hex(std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = kDigits[value & 0xFull];
    value >>= 4;
  }
  return out;
}

}  // namespace pff
