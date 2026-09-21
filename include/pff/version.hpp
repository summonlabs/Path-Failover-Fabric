// Path Failover Fabric - version information.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>

namespace pff {

inline constexpr std::uint32_t version_major = 1;
inline constexpr std::uint32_t version_minor = 0;
inline constexpr std::uint32_t version_patch = 0;

// Numeric release identifier: major * 10000 + minor * 100 + patch.
inline constexpr std::uint32_t version_number = 10000;

inline constexpr const char* version_string = "1.0.0";

// Durable format version for every on-disk artifact this release writes.
inline constexpr std::uint16_t durable_format_version = 1;

// Framed protocol version for the coordinator service.
inline constexpr std::uint16_t protocol_version = 1;

}  // namespace pff
