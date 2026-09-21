// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "net.hpp"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pff::net {
namespace {

#ifdef _WIN32
using native_socket = SOCKET;
constexpr native_socket kInvalid = INVALID_SOCKET;

native_socket to_native(handle_t handle) noexcept {
  return static_cast<native_socket>(static_cast<std::uintptr_t>(handle));
}
handle_t from_native(native_socket socket) noexcept {
  return static_cast<handle_t>(static_cast<std::intptr_t>(socket));
}
#else
using native_socket = int;
constexpr native_socket kInvalid = -1;

native_socket to_native(handle_t handle) noexcept { return static_cast<native_socket>(handle); }
handle_t from_native(native_socket socket) noexcept { return static_cast<handle_t>(socket); }
#endif

std::once_flag g_init_once;
bool g_init_ok = false;

void initialize_once() {
#ifdef _WIN32
  WSADATA data = {};
  g_init_ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  g_init_ok = true;
#endif
}

}  // namespace

bool initialize() noexcept {
  std::call_once(g_init_once, initialize_once);
  return g_init_ok;
}

std::string last_error_text() {
#ifdef _WIN32
  return std::string("socket error ") + std::to_string(::WSAGetLastError());
#else
  return std::string("socket error ") + std::to_string(errno);
#endif
}

Result<handle_t> listen_loopback(std::uint16_t port, std::uint16_t& bound_port) {
  if (!initialize()) {
    return Status(Code::unsupported, Reason::io_failure, "socket library unavailable");
  }
  const native_socket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalid) {
    return Status(Code::io_failure, Reason::io_failure, last_error_text());
  }
#ifndef _WIN32
  int reuse = 1;
  static_cast<void>(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                                 reinterpret_cast<const char*>(&reuse), sizeof(reuse)));
#endif
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    static_cast<void>(close_handle(from_native(listener)));
    return Status(Code::io_failure, Reason::io_failure, last_error_text());
  }
  if (::listen(listener, 32) != 0) {
    static_cast<void>(close_handle(from_native(listener)));
    return Status(Code::io_failure, Reason::io_failure, last_error_text());
  }
  sockaddr_in bound = {};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(bound));
#else
  socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    static_cast<void>(close_handle(from_native(listener)));
    return Status(Code::io_failure, Reason::io_failure, last_error_text());
  }
  bound_port = ntohs(bound.sin_port);
  return from_native(listener);
}

Result<handle_t> accept_one(handle_t listener) {
  const native_socket accepted = ::accept(to_native(listener), nullptr, nullptr);
  if (accepted == kInvalid) {
    return Status(Code::io_failure, Reason::io_failure, last_error_text());
  }
  int nodelay = 1;
  static_cast<void>(::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay)));
  return from_native(accepted);
}

Result<handle_t> connect_loopback(const std::string& host, std::uint16_t port) {
  if (!initialize()) {
    return Status(Code::unsupported, Reason::io_failure, "socket library unavailable");
  }
  const native_socket client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == kInvalid) {
    return Status(Code::io_failure, Reason::io_failure, last_error_text());
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    static_cast<void>(close_handle(from_native(client)));
    return Status(Code::invalid, Reason::domain_invalid, "bind address must be a literal IPv4 address");
  }
  if (::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    static_cast<void>(close_handle(from_native(client)));
    return Status(Code::io_failure, Reason::io_failure, last_error_text());
  }
  int nodelay = 1;
  static_cast<void>(::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&nodelay), sizeof(nodelay)));
  return from_native(client);
}

Status close_handle(handle_t handle) noexcept {
  if (handle == invalid_handle) {
    return ok_status();
  }
#ifdef _WIN32
  if (::closesocket(to_native(handle)) != 0) {
    return Status(Code::io_failure, Reason::io_failure, "closesocket failed");
  }
#else
  if (::close(to_native(handle)) != 0) {
    return Status(Code::io_failure, Reason::io_failure, "close failed");
  }
#endif
  return ok_status();
}

Status shutdown_handle(handle_t handle) noexcept {
  if (handle == invalid_handle) {
    return ok_status();
  }
#ifdef _WIN32
  static_cast<void>(::shutdown(to_native(handle), SD_BOTH));
#else
  static_cast<void>(::shutdown(to_native(handle), SHUT_RDWR));
#endif
  return ok_status();
}

Status send_all(handle_t handle, std::span<const std::byte> data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t chunk = data.size() - sent;
    const int request = static_cast<int>(chunk > 1u << 20 ? (1u << 20) : chunk);
#ifdef _WIN32
    const int written = ::send(to_native(handle), reinterpret_cast<const char*>(data.data() + sent),
                               request, 0);
#else
    const ssize_t written =
        ::send(to_native(handle), data.data() + sent, static_cast<std::size_t>(request), 0);
#endif
    if (written <= 0) {
      return Status(Code::io_failure, Reason::io_failure, last_error_text());
    }
    sent += static_cast<std::size_t>(written);
  }
  return ok_status();
}

Result<std::size_t> recv_some(handle_t handle, std::span<std::byte> buffer) {
  if (buffer.empty()) {
    return static_cast<std::size_t>(0);
  }
  const int request = static_cast<int>(buffer.size() > (1u << 20) ? (1u << 20) : buffer.size());
#ifdef _WIN32
  const int got = ::recv(to_native(handle), reinterpret_cast<char*>(buffer.data()), request, 0);
#else
  const ssize_t got = ::recv(to_native(handle), buffer.data(), static_cast<std::size_t>(request), 0);
#endif
  if (got < 0) {
    return Status(Code::io_failure, Reason::io_failure, last_error_text());
  }
  return static_cast<std::size_t>(got);
}

}  // namespace pff::net
