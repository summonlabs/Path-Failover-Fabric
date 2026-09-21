// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/server.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <utility>

#include "net.hpp"
#include "pff/wire.hpp"

namespace pff {
namespace {

constexpr std::size_t kRecvChunk = std::size_t{64} * 1024;

}  // namespace

SocketHandle::~SocketHandle() {
  if (handle_ != -1) {
    static_cast<void>(net::close_handle(handle_));
  }
}

std::shared_ptr<SocketHandle> Server::make_socket(std::intptr_t handle) {
  return std::make_shared<SocketHandle>(handle);
}

Server::~Server() {
  // A destructor must not let an exception escape. Every step below is
  // allocation free and reports failures through status values.
  try {
    request_stop();
    wait_for_sessions();
  } catch (...) {
    // Last resort if locking itself failed: close the listener directly. This
    // cannot allocate and releases any blocked accept.
    if (listener_.load() != -1) {
      static_cast<void>(net::close_handle(listener_.exchange(-1)));
    }
  }
}

Result<std::unique_ptr<Server>> Server::start(const ServerOptions& options, Fabric& fabric) {
  try {
    if (options.max_sessions == 0 || options.max_sessions > limits::max_sessions) {
      return Status(Code::invalid, Reason::domain_invalid, "max_sessions out of range");
    }
    if (!net::initialize()) {
      return Status(Code::unsupported, Reason::io_failure, "socket library unavailable");
    }
    auto server = std::unique_ptr<Server>(new Server());
    server->options_ = options;
    server->fabric_ = &fabric;
    std::uint16_t bound = 0;
    auto listener = net::listen_loopback(options.port, bound);
    if (!listener.ok()) {
      return listener.status();
    }
    server->listener_.store(listener.value());
    server->port_ = bound;
    return server;
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

void Server::request_stop() {
  if (stop_.exchange(true)) {
    return;
  }
  // Wake the blocked accept by connecting to ourselves. The accept loop owns the
  // listener and closes it, so no thread ever closes a socket another thread is
  // blocked on.
  if (listener_.load() != -1) {
    auto wakeup = net::connect_loopback("127.0.0.1", port_);
    if (wakeup.ok()) {
      static_cast<void>(net::close_handle(wakeup.value()));
    }
  }
  release_blocked_io();
}

void Server::release_blocked_io() {
  // The session table is bounded, so this never allocates and can be called from
  // the destructor. Taking a shared reference keeps each handle valid for the
  // whole call, so a concurrent close by the owning session thread cannot race.
  std::array<std::shared_ptr<SocketHandle>, limits::max_sessions> handles;
  std::size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& session : sessions_) {
      if (count == handles.size()) {
        break;
      }
      handles[count] = session->socket;
      ++count;
    }
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (handles[index] != nullptr) {
      static_cast<void>(net::shutdown_handle(handles[index]->native_handle()));
    }
  }
}

void Server::wait_for_sessions() {
  std::unique_lock<std::mutex> lock(mu_);
  wait_cv_.wait(lock, [this] { return active_threads_.load() == 0; });
}

ServerStats Server::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  ServerStats out = stats_;
  out.active_sessions = sessions_.size();
  return out;
}

void Server::forget_session(const std::shared_ptr<Session>& session) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = std::find(sessions_.begin(), sessions_.end(), session);
  if (it != sessions_.end()) {
    sessions_.erase(it);
    ++stats_.sessions_closed;
  }
}

Status Server::send_frame(const std::shared_ptr<Session>& session, MsgType type,
                          std::uint64_t sequence, std::span<const std::byte> payload) {
  const std::vector<std::byte> frame = encode_frame(type, sequence, 0, payload);
  if (frame.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode response frame");
  }
  const std::shared_ptr<SocketHandle> socket = session->socket;
  if (socket == nullptr) {
    return Status(Code::io_failure, Reason::io_failure, "session has no socket");
  }
  // No lock is held across the blocking write. The shared handle keeps the
  // socket valid while this call is in flight, and the shutdown path releases a
  // blocked send through shutdown() rather than by closing another thread's
  // handle.
  const Status status = net::send_all(socket->native_handle(),
                                      std::span<const std::byte>(frame.data(), frame.size()));
  if (status.ok()) {
    std::lock_guard<std::mutex> stats_lock(mu_);
    stats_.bytes_sent += frame.size();
  }
  return status;
}

Server::DispatchResult Server::error_response(Code code, Reason reason, std::string detail) {
  DispatchResult result;
  ErrorResponse error;
  error.code = code;
  error.reason = reason;
  error.detail = std::move(detail);
  if (error.detail.size() > limits::max_reason_bytes) {
    error.detail.resize(limits::max_reason_bytes);
  }
  result.response_type = MsgType::error_rsp;
  result.payload = encode_error_response(error);
  return result;
}

Server::DispatchResult Server::dispatch(const std::shared_ptr<Session>& session, const Frame& frame) {
  DispatchResult result;
  ByteReader reader(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  auto header = decode_session_header(reader);
  if (!header.ok()) {
    result = error_response(Code::invalid, header.status().reason, "session header invalid");
    result.close_session = true;
    return result;
  }
  if (header.value().session != session->id) {
    // One session can never act under another session's identity.
    result = error_response(Code::refused, Reason::session_unknown,
                            "request session does not match the established session");
    result.close_session = true;
    return result;
  }
  if (header.value().epoch != session->epoch || header.value().boot != session->boot) {
    result = error_response(Code::stale, Reason::session_stale,
                            "session authority is no longer current");
    result.close_session = true;
    return result;
  }
  if (header.value().request_sequence == 0 ||
      header.value().request_sequence <= session->last_request_sequence) {
    result = error_response(Code::conflict, Reason::sequence_regression,
                            "request sequence replayed or regressed");
    result.close_session = true;
    return result;
  }
  if (header.value().request_sequence > options_.max_requests_per_session) {
    result = error_response(Code::exhausted, Reason::request_limit,
                            "session request budget exhausted");
    result.close_session = true;
    return result;
  }
  session->last_request_sequence = header.value().request_sequence;

  // Successful mutations answer with an empty acknowledgement; a failure answers
  // with the exact code, reason and detail. An ok status must never be turned
  // into an error response.
  const auto from_status = [&](const Status& status) {
    if (status.ok()) {
      DispatchResult acknowledgement;
      acknowledgement.response_type = MsgType::ok_rsp;
      return acknowledgement;
    }
    return error_response(status.code, status.reason, status.detail);
  };

  switch (frame.type) {
    case MsgType::status_req: {
      StatusResponse response;
      const FabricIdentity identity = fabric_->identity();
      const FabricStats stats = fabric_->stats();
      response.incarnation = identity.incarnation;
      response.epoch = identity.epoch;
      response.boot = identity.boot;
      response.authority = fabric_->authority();
      response.stats = stats;
      response.recovery = fabric_->recovery();
      response.policy_installed = fabric_->policy_installed();
      response.obligations_installed = fabric_->obligations_installed();
      response.alternate_sets = fabric_->alternate_set_count();
      response.active_attempts = fabric_->active_attempt_count();
      {
        std::lock_guard<std::mutex> lock(mu_);
        response.active_sessions = sessions_.size();
        response.requests_handled = stats_.requests_handled;
      }
      response.fencing_events = stats.fences_recorded;
      std::vector<std::byte> payload = encode_status_response(response);
      if (payload.empty()) {
        return error_response(Code::invalid, Reason::invalid_payload, "cannot encode status");
      }
      result.response_type = MsgType::status_rsp;
      result.payload = std::move(payload);
      return result;
    }
    case MsgType::set_policy_req: {
      auto policy = wire::get_policy(reader);
      if (!policy.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "policy payload invalid");
      }
      return from_status(fabric_->apply_policy(policy.value()));
    }
    case MsgType::set_obligations_req: {
      auto obligations = wire::get_obligations(reader);
      if (!obligations.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "obligations payload invalid");
      }
      return from_status(fabric_->apply_obligations(obligations.value()));
    }
    case MsgType::put_set_req: {
      auto set = wire::get_set(reader);
      if (!set.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "alternate set payload invalid");
      }
      return from_status(fabric_->apply_alternate_set(set.value()));
    }
    case MsgType::put_evidence_req: {
      auto evidence = wire::get_evidence(reader);
      if (!evidence.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "evidence payload invalid");
      }
      return from_status(fabric_->apply_evidence(evidence.value()));
    }
    case MsgType::set_authority_req: {
      auto authority = wire::get_authority(reader);
      if (!authority.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "authority payload invalid");
      }
      return from_status(fabric_->set_authority(authority.value()));
    }
    case MsgType::evaluate_req:
    case MsgType::promote_req: {
      auto request = decode_request_body(reader);
      if (!request.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "request payload invalid");
      }
      const PromotionRequest& value = request.value();
      if (frame.type == MsgType::evaluate_req) {
        auto decision = fabric_->evaluate(value.incumbent, value.condition, value.set);
        if (!decision.ok()) {
          return from_status(decision.status());
        }
        PromoteResponse response;
        response.decision = std::move(decision).value();
        response.grant = GrantId::absent();
        response.attempt = AttemptId::absent();
        std::vector<std::byte> payload = encode_promote_response(response);
        if (payload.empty()) {
          return error_response(Code::invalid, Reason::invalid_payload, "cannot encode decision");
        }
        result.response_type = MsgType::evaluate_rsp;
        result.payload = std::move(payload);
        return result;
      }
      auto authorization = fabric_->begin_promotion(value.incumbent, value.condition, value.set);
      if (!authorization.ok()) {
        return from_status(authorization.status());
      }
      PromoteResponse response;
      response.decision = authorization.value().decision;
      response.grant = authorization.value().grant.id;
      response.attempt = authorization.value().grant.attempt;
      if (!authorization.value().authorized()) {
        response.grant = GrantId::absent();
        response.attempt = authorization.value().decision.attempt;
      }
      std::vector<std::byte> payload = encode_promote_response(response);
      if (payload.empty()) {
        return error_response(Code::invalid, Reason::invalid_payload, "cannot encode grant");
      }
      result.response_type = MsgType::promote_rsp;
      result.payload = std::move(payload);
      return result;
    }
    case MsgType::effect_req: {
      auto body = decode_effect_body(reader);
      if (!body.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "effect payload invalid");
      }
      return from_status(fabric_->record_effect(body.value().first, body.value().second));
    }
    case MsgType::abandon_req: {
      auto body = decode_abandon_body(reader);
      if (!body.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "abandon payload invalid");
      }
      return from_status(fabric_->abandon_attempt(body.value().first, body.value().second));
    }
    case MsgType::rollback_req:
    case MsgType::revert_req: {
      auto body = decode_rollback_body(reader);
      if (!body.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "rollback payload invalid");
      }
      if (frame.type == MsgType::rollback_req) {
        return from_status(fabric_->rollback(body.value().first, body.value().second));
      }
      auto decision = fabric_->revert(body.value().first, body.value().second);
      if (!decision.ok()) {
        return from_status(decision.status());
      }
      PromoteResponse response;
      response.decision = std::move(decision).value();
      response.grant = GrantId::absent();
      response.attempt = response.decision.attempt;
      std::vector<std::byte> payload = encode_promote_response(response);
      if (payload.empty()) {
        return error_response(Code::invalid, Reason::invalid_payload, "cannot encode reversion");
      }
      result.response_type = MsgType::evaluate_rsp;
      result.payload = std::move(payload);
      return result;
    }
    case MsgType::fence_req: {
      auto body = decode_fence_body(reader);
      if (!body.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "fence payload invalid");
      }
      return from_status(fabric_->fence_path(body.value().first, body.value().second));
    }
    case MsgType::attempt_req: {
      auto attempt = decode_attempt_body(reader);
      if (!attempt.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "attempt payload invalid");
      }
      AttemptResponse response;
      auto record = fabric_->attempt(attempt.value());
      response.found = record.ok();
      if (record.ok()) {
        response.record = record.value();
      } else {
        response.record.id = attempt.value();
      }
      std::vector<std::byte> payload = encode_attempt_response(response);
      if (payload.empty()) {
        return error_response(Code::invalid, Reason::invalid_payload, "cannot encode attempt");
      }
      result.response_type = MsgType::attempt_rsp;
      result.payload = std::move(payload);
      return result;
    }
    case MsgType::lineage_req: {
      auto limit = decode_lineage_body(reader);
      if (!limit.ok() || !reader.at_end()) {
        return error_response(Code::invalid, Reason::invalid_payload, "lineage payload invalid");
      }
      LineageResponse response;
      response.entries = fabric_->lineage(limit.value());
      response.truncated = response.entries.size() >= limit.value();
      std::vector<std::byte> payload = encode_lineage_response(response);
      if (payload.empty()) {
        return error_response(Code::invalid, Reason::invalid_payload, "cannot encode lineage");
      }
      result.response_type = MsgType::lineage_rsp;
      result.payload = std::move(payload);
      return result;
    }
    case MsgType::shutdown_req: {
      if (!options_.allow_shutdown) {
        return error_response(Code::refused, Reason::message_unsupported,
                              "shutdown is disabled for this service");
      }
      result.response_type = MsgType::shutdown_rsp;
      result.payload.clear();
      result.stop_server = true;
      return result;
    }
    default:
      return error_response(Code::unsupported, Reason::message_unsupported,
                            "message type is not a supported request");
  }
}

void Server::serve_session(const std::shared_ptr<Session>& session) {
  // A local shared reference keeps the socket alive for the whole session, so
  // the shutdown path may call shutdown() at any moment without invalidating it.
  const std::shared_ptr<SocketHandle> socket = session->socket;
  std::vector<std::byte> buffer(kRecvChunk);
  bool handshake_done = false;
  bool keep_going = socket != nullptr;

  // ---- handshake: establish the session authority boundary ------------------
  while (keep_going && !handshake_done && !stop_.load()) {
    auto received = net::recv_some(socket->native_handle(),
                                   std::span<std::byte>(buffer.data(), buffer.size()));
    if (!received.ok() || received.value() == 0) {
      keep_going = false;
      break;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      stats_.bytes_received += received.value();
    }
    static_cast<void>(
        session->decoder.feed(std::span<const std::byte>(buffer.data(), received.value())));
    while (!session->decoder.failed()) {
      auto frame = session->decoder.pop();
      if (!frame.ok()) {
        break;
      }
      if (frame.value().type != MsgType::hello) {
        static_cast<void>(send_frame(
            session, MsgType::error_rsp, frame.value().sequence,
            encode_error_response(ErrorResponse{Code::refused, Reason::message_unsupported,
                                                "handshake required"})));
        {
          std::lock_guard<std::mutex> lock(mu_);
          ++stats_.frames_rejected;
        }
        keep_going = false;
        break;
      }
      auto hello = decode_hello_request(std::span<const std::byte>(frame.value().payload.data(),
                                                                   frame.value().payload.size()));
      if (!hello.ok()) {
        keep_going = false;
        break;
      }
      if (hello.value().protocol != protocol_version) {
        static_cast<void>(send_frame(
            session, MsgType::error_rsp, frame.value().sequence,
            encode_error_response(ErrorResponse{Code::version_unsupported,
                                                Reason::protocol_version_mismatch,
                                                "protocol version mismatch"})));
        keep_going = false;
        break;
      }
      const FabricIdentity identity = fabric_->identity();
      HelloResponse response;
      response.protocol = protocol_version;
      response.epoch = identity.epoch;
      response.boot = identity.boot;
      response.incarnation = identity.incarnation;
      response.authority = fabric_->authority();
      response.max_requests_per_session = options_.max_requests_per_session;
      {
        std::lock_guard<std::mutex> lock(mu_);
        session->id = SessionId::from_value(++session_sequence_);
        session->epoch = identity.epoch;
        session->boot = identity.boot;
        ++stats_.handshakes_completed;
      }
      response.session = session->id;
      const std::vector<std::byte> ack = encode_hello_response(response);
      if (ack.empty() || !send_frame(session, MsgType::hello_ack, frame.value().sequence,
                                     std::span<const std::byte>(ack.data(), ack.size()))
                             .ok()) {
        keep_going = false;
        break;
      }
      handshake_done = true;
      break;
    }
    if (session->decoder.failed()) {
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.frames_rejected;
      keep_going = false;
    }
  }

  // ---- request loop ---------------------------------------------------------
  while (keep_going && handshake_done && !stop_.load()) {
    auto received = net::recv_some(socket->native_handle(),
                                   std::span<std::byte>(buffer.data(), buffer.size()));
    if (!received.ok() || received.value() == 0) {
      break;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      stats_.bytes_received += received.value();
    }
    const Status fed =
        session->decoder.feed(std::span<const std::byte>(buffer.data(), received.value()));
    if (!fed.ok()) {
      std::lock_guard<std::mutex> lock(mu_);
      ++stats_.frames_rejected;
    }
    while (keep_going && !session->decoder.failed()) {
      auto frame = session->decoder.pop();
      if (!frame.ok()) {
        break;
      }
      if (!is_request_type(frame.value().type)) {
        static_cast<void>(send_frame(
            session, MsgType::error_rsp, frame.value().sequence,
            encode_error_response(ErrorResponse{Code::refused, Reason::message_unsupported,
                                                "response type is not a request"})));
        keep_going = false;
        break;
      }
      DispatchResult dispatched = dispatch(session, frame.value());
      ++session->requests_handled;
      {
        std::lock_guard<std::mutex> lock(mu_);
        ++stats_.requests_handled;
        if (dispatched.response_type == MsgType::error_rsp) {
          ++stats_.frames_rejected;
        }
      }
      const Status sent =
          send_frame(session, dispatched.response_type, frame.value().sequence,
                     std::span<const std::byte>(dispatched.payload.data(),
                                                dispatched.payload.size()));
      if (!sent.ok() || dispatched.close_session) {
        keep_going = false;
      }
      if (dispatched.stop_server) {
        request_stop();
        keep_going = false;
      }
    }
  }

  // The last reference to the socket is released here, which closes it exactly
  // once. No other thread ever closes a session socket.
  forget_session(session);
  {
    std::lock_guard<std::mutex> lock(mu_);
    active_threads_.fetch_sub(1);
  }
  wait_cv_.notify_all();
}

Status Server::run() {
  while (!stop_.load()) {
    const std::intptr_t listener = listener_.load();
    if (listener == -1) {
      break;
    }
    auto accepted = net::accept_one(listener);
    if (!accepted.ok()) {
      if (stop_.load()) {
        break;
      }
      return accepted.status();
    }
    if (stop_.load()) {
      static_cast<void>(net::close_handle(accepted.value()));
      break;
    }

    auto session = std::make_shared<Session>(limits::max_frame_payload);
    session->socket = make_socket(accepted.value());
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (sessions_.size() >= options_.max_sessions) {
        ++stats_.connections_rejected;
        continue;  // the session goes out of scope and closes the socket
      }
      ++stats_.connections_accepted;
      sessions_.push_back(session);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      active_threads_.fetch_add(1);
    }
    try {
      std::thread([this, session]() { serve_session(session); }).detach();
    } catch (const std::system_error&) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        active_threads_.fetch_sub(1);
      }
      forget_session(session);
      wait_cv_.notify_all();
      return Status(Code::exhausted, Reason::allocation_denied, "cannot start session thread");
    }
  }
  const std::intptr_t listener = listener_.exchange(-1);
  if (listener != -1) {
    static_cast<void>(net::close_handle(listener));
  }
  wait_for_sessions();
  return ok_status();
}

}  // namespace pff
