// Path Failover Fabric - checked integer arithmetic.
//
// Signed overflow is undefined behaviour, so every arithmetic operation that
// depends on externally supplied numbers goes through these helpers. A failed
// check is a deterministic refusal, never a wrapped value.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>

namespace pff {

[[nodiscard]] constexpr bool checked_add(std::int64_t a, std::int64_t b,
                                         std::int64_t& out) noexcept {
  if (b > 0 && a > (INT64_MAX - b)) {
    return false;
  }
  if (b < 0 && a < (INT64_MIN - b)) {
    return false;
  }
  out = a + b;
  return true;
}

[[nodiscard]] constexpr bool checked_sub(std::int64_t a, std::int64_t b,
                                         std::int64_t& out) noexcept {
  if (b == INT64_MIN) {
    return false;
  }
  return checked_add(a, -b, out);
}

[[nodiscard]] constexpr bool checked_mul(std::int64_t a, std::int64_t b,
                                         std::int64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  const bool negative = (a < 0) != (b < 0);
  const std::uint64_t ua = (a < 0) ? (0u - static_cast<std::uint64_t>(a))
                                   : static_cast<std::uint64_t>(a);
  const std::uint64_t ub = (b < 0) ? (0u - static_cast<std::uint64_t>(b))
                                   : static_cast<std::uint64_t>(b);
  const std::uint64_t limit = negative ? (std::uint64_t{1} << 63)
                                       : ((std::uint64_t{1} << 63) - 1u);
  if (ub > limit / ua) {
    return false;
  }
  const std::uint64_t ur = ua * ub;
  out = negative ? static_cast<std::int64_t>(0u - ur) : static_cast<std::int64_t>(ur);
  return true;
}

[[nodiscard]] constexpr std::int64_t clamp_i64(std::int64_t v, std::int64_t lo,
                                               std::int64_t hi) noexcept {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

// Checked conversion from size_t to uint32 with explicit refusal.
[[nodiscard]] constexpr bool checked_u32(std::size_t v, std::uint32_t& out) noexcept {
  if (v > 0xFFFFFFFFull) {
    return false;
  }
  out = static_cast<std::uint32_t>(v);
  return true;
}

}  // namespace pff
