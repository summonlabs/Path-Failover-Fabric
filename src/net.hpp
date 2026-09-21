// Internal portable blocking socket helpers. Not installed.
//
// Deliberately minimal: blocking sockets, no timeouts, teardown through
// shutdown() so a blocked recv/accept is released rather than timed out.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "pff/status.hpp"

namespace pff::net {

using handle_t = std::intptr_t;
inline constexpr handle_t invalid_handle = -1;

// Initialises the platform socket library once per process.
[[nodiscard]] bool initialize() noexcept;

[[nodiscard]] Result<handle_t> listen_loopback(std::uint16_t port, std::uint16_t& bound_port);
[[nodiscard]] Result<handle_t> accept_one(handle_t listener);
[[nodiscard]] Result<handle_t> connect_loopback(const std::string& host, std::uint16_t port);

[[nodiscard]] Status close_handle(handle_t handle) noexcept;
[[nodiscard]] Status shutdown_handle(handle_t handle) noexcept;

// Sends the whole buffer or reports the failure; never partially succeeds.
[[nodiscard]] Status send_all(handle_t handle, std::span<const std::byte> data);
// One blocking receive. Zero means orderly shutdown by the peer.
[[nodiscard]] Result<std::size_t> recv_some(handle_t handle, std::span<std::byte> buffer);

[[nodiscard]] std::string last_error_text();

}  // namespace pff::net
