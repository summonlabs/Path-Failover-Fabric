// Path Failover Fabric - coordinator service and client.
//
// The service is a bounded, framed protocol over a loopback TCP socket. Shutdown
// releases blocked accepts and reads through socket shutdown and a wakeup
// connection rather than through timeouts, so teardown is prompt and
// deterministic.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pff/fabric.hpp"
#include "pff/protocol.hpp"

namespace pff {

// Shared, single-close ownership of a connected socket handle. It is published
// so that a session socket can be shared between the thread that serves it and
// the thread that tears the service down: the handle stays valid for every user,
// it is closed exactly once by the last owner, and a shutdown from another
// thread can never act on a recycled handle.
class SocketHandle {
 public:
  explicit SocketHandle(std::intptr_t handle) noexcept : handle_(handle) {}
  ~SocketHandle();
  SocketHandle(const SocketHandle&) = delete;
  SocketHandle& operator=(const SocketHandle&) = delete;
  SocketHandle(SocketHandle&&) = delete;
  SocketHandle& operator=(SocketHandle&&) = delete;

  [[nodiscard]] std::intptr_t native_handle() const noexcept { return handle_; }

 private:
  std::intptr_t handle_;
};

struct ServerOptions {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;  // 0 selects an ephemeral port
  std::size_t max_sessions = limits::max_sessions;
  std::uint64_t max_requests_per_session = limits::default_max_requests_per_session;
  bool allow_shutdown = true;
};

struct ServerStats {
  std::uint64_t connections_accepted = 0;
  std::uint64_t connections_rejected = 0;
  std::uint64_t handshakes_completed = 0;
  std::uint64_t requests_handled = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t sessions_closed = 0;
  std::uint64_t sessions_evicted = 0;
  std::uint64_t bytes_received = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t active_sessions = 0;
};

class Server {
 public:
  static Result<std::unique_ptr<Server>> start(const ServerOptions& options, Fabric& fabric);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Blocking accept loop. Returns once request_stop() has been observed and all
  // session threads have finished. The listener is closed by this thread only.
  Status run();
  void request_stop();
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] bool stopped() const noexcept { return stop_.load(); }
  [[nodiscard]] ServerStats stats() const;

 private:
  Server() = default;

  struct Session {
    explicit Session(std::size_t max_payload) : decoder(max_payload) {}
    // The socket is shared by ownership, so the shutdown path may call shutdown()
    // while the session thread is blocked in a send or a receive without any
    // risk of a double close or of acting on a recycled handle.
    std::shared_ptr<SocketHandle> socket;
    SessionId id;
    Epoch epoch;
    BootId boot;
    std::uint64_t requests_handled = 0;
    std::uint64_t last_request_sequence = 0;
    FrameDecoder decoder;
  };

  struct DispatchResult {
    MsgType response_type = MsgType::error_rsp;
    std::vector<std::byte> payload;
    bool close_session = false;
    bool stop_server = false;
  };

  void serve_session(const std::shared_ptr<Session>& session);
  DispatchResult dispatch(const std::shared_ptr<Session>& session, const Frame& frame);
  DispatchResult error_response(Code code, Reason reason, std::string detail);
  Status send_frame(const std::shared_ptr<Session>& session, MsgType type,
                    std::uint64_t sequence, std::span<const std::byte> payload);
  void release_blocked_io();
  void wait_for_sessions();
  void forget_session(const std::shared_ptr<Session>& session);
  [[nodiscard]] static std::shared_ptr<SocketHandle> make_socket(std::intptr_t handle);

  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> active_threads_{0};
  std::uint16_t port_ = 0;
  // Written by the accept loop and read by request_stop from other threads.
  std::atomic<std::intptr_t> listener_{-1};
  Fabric* fabric_ = nullptr;
  ServerOptions options_;

  mutable std::mutex mu_;
  std::condition_variable wait_cv_;
  std::vector<std::shared_ptr<Session>> sessions_;
  std::uint64_t session_sequence_ = 0;
  ServerStats stats_;
};

struct ClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string name = "pff-client";
  std::uint64_t nonce = 0;
};

class Client {
 public:
  static Result<std::unique_ptr<Client>> connect(const ClientOptions& options);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  [[nodiscard]] const HelloResponse& hello() const noexcept { return hello_; }
  [[nodiscard]] const SessionHeader& header() const noexcept { return header_; }

  Result<StatusResponse> status();
  Result<PromotionDecision> evaluate(const PromotionRequest& request);
  Result<PromoteResponse> promote(const PromotionRequest& request);
  Status set_policy(const FailoverPolicy& policy);
  Status set_obligations(const ServiceObligations& obligations);
  Status put_set(const AlternateSet& set);
  Status put_evidence(const PathEvidence& evidence);
  Status set_authority(const AuthorityVector& authority);
  Status record_effect(AttemptId attempt, const EffectEvidence& effect);
  Status abandon(AttemptId attempt, Reason reason);
  Status rollback(AttemptId attempt, const PathEvidence& evidence);
  Result<PromotionDecision> revert(AttemptId attempt, const PathEvidence& target);
  Status fence_path(PathId path, Reason reason);
  Result<AttemptResponse> attempt(AttemptId attempt);
  Result<LineageResponse> lineage(std::size_t limit);
  Status shutdown();

  [[nodiscard]] std::uint64_t requests_sent() const noexcept { return sequence_; }

 private:
  Client() = default;

  Result<Frame> transact(MsgType request, std::span<const std::byte> payload, MsgType expected);
  Result<Frame> round_trip(MsgType type, std::span<const std::byte> payload);
  Status expect_ok(MsgType request, std::span<const std::byte> payload);
  std::vector<std::byte> next_request(std::span<const std::byte> body);

  std::intptr_t socket_handle_ = -1;
  FrameDecoder decoder_;
  SessionHeader header_;
  HelloResponse hello_;
  std::uint64_t sequence_ = 0;
  bool closed_ = false;
};

}  // namespace pff
