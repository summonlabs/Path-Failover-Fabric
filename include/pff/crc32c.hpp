// Path Failover Fabric - CRC-32C (Castagnoli) integrity checks.
//
// A fixed, dependency-free, well-specified 32-bit integrity check used for every
// durable record, snapshot and protocol frame. It detects accidental corruption
// and truncation. It is not a cryptographic authenticator.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <span>
#include <cstddef>

namespace pff {

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t crc32c_update(std::uint32_t seed,
                                          std::span<const std::byte> data) noexcept;

// FNV-1a 64 over a byte range. Used for decision digests, where a compact
// deterministic fingerprint of a canonical document is required.
[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::byte> data) noexcept;

}  // namespace pff
