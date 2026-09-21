// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/server.hpp"

#include <new>
#include <utility>

#include "net.hpp"
#include "pff/wire.hpp"

namespace pff {
namespace {

constexpr std::size_t kRecvChunk = std::size_t{64} * 1024;

}  // namespace

Client::~Client() {
  if (!closed_ && socket_handle_ != -1) {
    static_cast<void>(net::close_handle(socket_handle_));
    socket_handle_ = -1;
    closed_ = true;
  }
}

Result<std::unique_ptr<Client>> Client::connect(const ClientOptions& options) {
  try {
    if (!net::initialize()) {
      return Status(Code::unsupported, Reason::io_failure, "socket library unavailable");
    }
    auto handle = net::connect_loopback(options.host, options.port);
    if (!handle.ok()) {
      return handle.status();
    }
    auto client = std::unique_ptr<Client>(new Client());
    client->socket_handle_ = handle.value();

    HelloRequest hello;
    hello.protocol = protocol_version;
    hello.client = options.name.empty() ? std::string("pff-client") : options.name;
    if (hello.client.size() > limits::max_name_bytes) {
      hello.client.resize(limits::max_name_bytes);
    }
    hello.nonce = options.nonce;
    const std::vector<std::byte> payload = encode_hello_request(hello);
    if (payload.empty()) {
      return Status(Code::invalid, Reason::invalid_payload, "cannot encode hello");
    }
    auto response = client->round_trip(MsgType::hello, std::span<const std::byte>(payload.data(),
                                                                                payload.size()));
    if (!response.ok()) {
      return response.status();
    }
    if (response.value().type != MsgType::hello_ack) {
      auto error = decode_error_response(std::span<const std::byte>(response.value().payload.data(),
                                                                    response.value().payload.size()));
      if (error.ok()) {
        return Status(error.value().code, error.value().reason, error.value().detail);
      }
      return Status(Code::refused, Reason::message_unsupported, "handshake refused");
    }
    auto decoded = decode_hello_response(std::span<const std::byte>(response.value().payload.data(),
                                                                    response.value().payload.size()));
    if (!decoded.ok()) {
      return decoded.status();
    }
    client->hello_ = decoded.value();
    if (client->hello_.protocol != protocol_version) {
      return Status(Code::version_unsupported, Reason::protocol_version_mismatch,
                    "coordinator protocol version mismatch");
    }
    client->header_.session = client->hello_.session;
    client->header_.epoch = client->hello_.epoch;
    client->header_.boot = client->hello_.boot;
    client->header_.request_sequence = 0;
    return client;
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Result<Frame> Client::round_trip(MsgType type, std::span<const std::byte> payload) {
  const std::uint64_t sequence = sequence_ + 1;
  const std::vector<std::byte> frame = encode_frame(type, sequence, 0, payload);
  if (frame.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode request frame");
  }
  const Status sent =
      net::send_all(socket_handle_, std::span<const std::byte>(frame.data(), frame.size()));
  if (!sent.ok()) {
    return sent;
  }
  sequence_ = sequence;

  std::vector<std::byte> buffer(kRecvChunk);
  while (true) {
    auto popped = decoder_.pop();
    if (popped.ok()) {
      if (popped.value().sequence != sequence) {
        return Status(Code::conflict, Reason::sequence_regression,
                      "response sequence does not match the request sequence");
      }
      return popped;
    }
    if (decoder_.failed()) {
      return decoder_.status();
    }
    auto received = net::recv_some(socket_handle_, std::span<std::byte>(buffer.data(), buffer.size()));
    if (!received.ok()) {
      return received.status();
    }
    if (received.value() == 0) {
      return Status(Code::io_failure, Reason::io_failure, "coordinator closed the connection");
    }
    const Status fed =
        decoder_.feed(std::span<const std::byte>(buffer.data(), received.value()));
    if (!fed.ok()) {
      return fed;
    }
  }
}

Result<Frame> Client::transact(MsgType request, std::span<const std::byte> payload, MsgType expected) {
  auto response = round_trip(request, payload);
  if (!response.ok()) {
    return response.status();
  }
  if (response.value().type == MsgType::error_rsp) {
    auto error = decode_error_response(std::span<const std::byte>(response.value().payload.data(),
                                                                  response.value().payload.size()));
    if (!error.ok()) {
      return error.status();
    }
    return Status(error.value().code, error.value().reason, error.value().detail);
  }
  if (response.value().type != expected) {
    return Status(Code::invalid, Reason::message_unsupported,
                  "coordinator returned an unexpected response type");
  }
  return response;
}

Status Client::expect_ok(MsgType request, std::span<const std::byte> payload) {
  auto response = transact(request, payload, MsgType::ok_rsp);
  if (!response.ok()) {
    return response.status();
  }
  if (!response.value().payload.empty()) {
    return Status(Code::invalid, Reason::trailing_garbage, "ok response carried a payload");
  }
  return ok_status();
}

std::vector<std::byte> Client::next_request(std::span<const std::byte> body) {
  header_.request_sequence += 1;
  return encode_session_request(header_, body);
}

Result<StatusResponse> Client::status() {
  header_.request_sequence += 1;
  const std::vector<std::byte> payload = encode_session_request(header_, {});
  if (payload.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode status request");
  }
  auto response = transact(MsgType::status_req, std::span<const std::byte>(payload.data(),
                                                                          payload.size()),
                           MsgType::status_rsp);
  if (!response.ok()) {
    return response.status();
  }
  return decode_status_response(std::span<const std::byte>(response.value().payload.data(),
                                                          response.value().payload.size()));
}

Status Client::set_policy(const FailoverPolicy& policy) {
  const std::vector<std::byte> body = [&policy] {
    ByteWriter writer(64, limits::max_record_payload);
    wire::put(writer, policy);
    if (!writer.ok()) {
      return std::vector<std::byte>{};
    }
    return std::move(writer).take();
  }();
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode policy");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::set_policy_req, std::span<const std::byte>(payload.data(), payload.size()));
}

Status Client::set_obligations(const ServiceObligations& obligations) {
  const std::vector<std::byte> body = [&obligations] {
    ByteWriter writer(64, limits::max_record_payload);
    wire::put(writer, obligations);
    if (!writer.ok()) {
      return std::vector<std::byte>{};
    }
    return std::move(writer).take();
  }();
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode obligations");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::set_obligations_req,
                   std::span<const std::byte>(payload.data(), payload.size()));
}

Status Client::put_set(const AlternateSet& set) {
  const std::vector<std::byte> body = [&set] {
    ByteWriter writer(256, limits::max_record_payload);
    wire::put(writer, set);
    if (!writer.ok()) {
      return std::vector<std::byte>{};
    }
    return std::move(writer).take();
  }();
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode alternate set");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::put_set_req, std::span<const std::byte>(payload.data(), payload.size()));
}

Status Client::put_evidence(const PathEvidence& evidence) {
  const std::vector<std::byte> body = [&evidence] {
    ByteWriter writer(160, limits::max_record_payload);
    wire::put(writer, evidence);
    if (!writer.ok()) {
      return std::vector<std::byte>{};
    }
    return std::move(writer).take();
  }();
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode evidence");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::put_evidence_req, std::span<const std::byte>(payload.data(), payload.size()));
}

Status Client::set_authority(const AuthorityVector& authority) {
  const std::vector<std::byte> body = [&authority] {
    ByteWriter writer(64, limits::max_record_payload);
    wire::put(writer, authority);
    if (!writer.ok()) {
      return std::vector<std::byte>{};
    }
    return std::move(writer).take();
  }();
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode authority");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::set_authority_req, std::span<const std::byte>(payload.data(), payload.size()));
}

Status Client::record_effect(AttemptId attempt, const EffectEvidence& effect) {
  const std::vector<std::byte> body = encode_effect_body(attempt, effect);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode effect");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::effect_req, std::span<const std::byte>(payload.data(), payload.size()));
}

Status Client::abandon(AttemptId attempt, Reason reason) {
  const std::vector<std::byte> body = encode_abandon_body(attempt, reason);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode abandon");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::abandon_req, std::span<const std::byte>(payload.data(), payload.size()));
}

Status Client::rollback(AttemptId attempt, const PathEvidence& evidence) {
  const std::vector<std::byte> body = encode_rollback_body(attempt, evidence);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode rollback");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::rollback_req, std::span<const std::byte>(payload.data(), payload.size()));
}

Status Client::fence_path(PathId path, Reason reason) {
  const std::vector<std::byte> body = encode_fence_body(path, reason);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode fence");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  return expect_ok(MsgType::fence_req, std::span<const std::byte>(payload.data(), payload.size()));
}

Result<PromotionDecision> Client::evaluate(const PromotionRequest& request) {
  const std::vector<std::byte> body = encode_request_body(request);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode request");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  auto response = transact(MsgType::evaluate_req,
                           std::span<const std::byte>(payload.data(), payload.size()),
                           MsgType::evaluate_rsp);
  if (!response.ok()) {
    return response.status();
  }
  auto decoded = decode_promote_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.ok()) {
    return decoded.status();
  }
  return decoded.value().decision;
}

Result<PromoteResponse> Client::promote(const PromotionRequest& request) {
  const std::vector<std::byte> body = encode_request_body(request);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode request");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  auto response = transact(MsgType::promote_req,
                           std::span<const std::byte>(payload.data(), payload.size()),
                           MsgType::promote_rsp);
  if (!response.ok()) {
    return response.status();
  }
  return decode_promote_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
}

Result<PromotionDecision> Client::revert(AttemptId attempt, const PathEvidence& target) {
  const std::vector<std::byte> body = encode_rollback_body(attempt, target);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode revert");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  auto response = transact(MsgType::revert_req,
                           std::span<const std::byte>(payload.data(), payload.size()),
                           MsgType::evaluate_rsp);
  if (!response.ok()) {
    return response.status();
  }
  auto decoded = decode_promote_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
  if (!decoded.ok()) {
    return decoded.status();
  }
  return decoded.value().decision;
}

Result<AttemptResponse> Client::attempt(AttemptId attempt_id) {
  const std::vector<std::byte> body = encode_attempt_body(attempt_id);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode attempt query");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  auto response = transact(MsgType::attempt_req,
                           std::span<const std::byte>(payload.data(), payload.size()),
                           MsgType::attempt_rsp);
  if (!response.ok()) {
    return response.status();
  }
  return decode_attempt_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
}

Result<LineageResponse> Client::lineage(std::size_t limit) {
  const auto bounded = static_cast<std::uint32_t>(
      limit == 0 ? limits::max_lineage
                 : (limit > limits::max_lineage ? limits::max_lineage : limit));
  const std::vector<std::byte> body = encode_lineage_body(bounded);
  if (body.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode lineage query");
  }
  const std::vector<std::byte> payload = next_request(std::span<const std::byte>(body.data(), body.size()));
  auto response = transact(MsgType::lineage_req,
                           std::span<const std::byte>(payload.data(), payload.size()),
                           MsgType::lineage_rsp);
  if (!response.ok()) {
    return response.status();
  }
  return decode_lineage_response(
      std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
}

Status Client::shutdown() {
  header_.request_sequence += 1;
  const std::vector<std::byte> payload = encode_session_request(header_, {});
  if (payload.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode shutdown request");
  }
  auto response = transact(MsgType::shutdown_req,
                           std::span<const std::byte>(payload.data(), payload.size()),
                           MsgType::shutdown_rsp);
  if (!response.ok()) {
    return response.status();
  }
  closed_ = true;
  const Status status = net::close_handle(socket_handle_);
  socket_handle_ = -1;
  return status;
}

}  // namespace pff
