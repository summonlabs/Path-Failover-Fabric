// Path Failover Fabric - bounded framed protocol.
//
// Every frame carries magic, version, type, flags, a monotonic per-session
// sequence, an authenticated payload length and two integrity checks. The
// decoder refuses truncated prefixes, oversized declared payloads, corrupt
// integrity fields, invalid enums, replay/regressed sequences, contradictory
// authority vectors and trailing bytes, and a decode failure is sticky.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "pff/canonical.hpp"
#include "pff/decision.hpp"
#include "pff/fabric.hpp"
#include "pff/model.hpp"
#include "pff/records.hpp"
#include "pff/version.hpp"

namespace pff {

enum class MsgType : std::uint16_t {
  hello = 1,
  hello_ack = 2,
  error_rsp = 3,
  ok_rsp = 4,
  status_req = 5,
  status_rsp = 6,
  set_policy_req = 7,
  set_obligations_req = 8,
  put_set_req = 9,
  put_evidence_req = 10,
  set_authority_req = 11,
  evaluate_req = 12,
  evaluate_rsp = 13,
  promote_req = 14,
  promote_rsp = 15,
  effect_req = 16,
  abandon_req = 17,
  rollback_req = 18,
  revert_req = 19,
  fence_req = 20,
  attempt_req = 21,
  attempt_rsp = 22,
  lineage_req = 23,
  lineage_rsp = 24,
  shutdown_req = 25,
  shutdown_rsp = 26,
};

const char* to_string(MsgType t) noexcept;
[[nodiscard]] bool is_known_msg_type(std::uint16_t raw) noexcept;
[[nodiscard]] bool is_request_type(MsgType t) noexcept;
[[nodiscard]] bool is_response_type(MsgType t) noexcept;

inline constexpr std::uint32_t frame_magic = 0x31464650u;  // "PFF1"
inline constexpr std::uint32_t max_frame_flags = 0;

struct Frame {
  std::uint16_t version = 0;
  MsgType type = MsgType::error_rsp;
  std::uint32_t flags = 0;
  std::uint64_t sequence = 0;
  std::vector<std::byte> payload;
};

[[nodiscard]] std::vector<std::byte> encode_frame(MsgType type, std::uint64_t sequence,
                                                  std::uint32_t flags,
                                                  std::span<const std::byte> payload);

// Streaming decoder. feed() never buffers more than one maximum frame; pop()
// returns exactly one frame at a time. Any failure is sticky for the life of the
// object, so a session that desynchronises can never resynchronise on garbage.
class FrameDecoder {
 public:
  explicit FrameDecoder(std::size_t max_payload = limits::max_frame_payload);

  Status feed(std::span<const std::byte> data);
  // True only when one complete, structurally valid frame is buffered.
  [[nodiscard]] bool has_frame() const;
  Result<Frame> pop();
  [[nodiscard]] bool failed() const noexcept { return !status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] std::size_t buffered() const noexcept { return available(); }

 private:
  [[nodiscard]] std::size_t available() const noexcept { return buffer_.size() - consumed_; }
  void compact();
  Status fail(Code code, Reason reason, const char* detail);

  std::vector<std::byte> buffer_;
  std::size_t consumed_ = 0;
  std::size_t max_payload_;
  Status status_;
};

// ---- session and message payloads -----------------------------------------
struct HelloRequest {
  std::uint16_t protocol = protocol_version;
  std::string client;
  std::uint64_t nonce = 0;
};

struct HelloResponse {
  std::uint16_t protocol = protocol_version;
  SessionId session;
  Epoch epoch;
  BootId boot;
  IncarnationId incarnation;
  AuthorityVector authority;
  std::uint64_t max_requests_per_session = 0;
};

// Every request after hello is bound to the session authority established at
// handshake time. A request that carries another session's identity, boot or
// epoch is refused: one session can never act under another's authority.
struct SessionHeader {
  SessionId session;
  Epoch epoch;
  BootId boot;
  std::uint64_t request_sequence = 0;
};

struct ErrorResponse {
  Code code = Code::ok;
  Reason reason = Reason::none;
  std::string detail;
};

struct StatusResponse {
  IncarnationId incarnation;
  Epoch epoch;
  BootId boot;
  AuthorityVector authority;
  FabricStats stats;
  RecoveryReport recovery;
  std::uint64_t active_sessions = 0;
  std::uint64_t requests_handled = 0;
  bool policy_installed = false;
  bool obligations_installed = false;
  std::uint64_t alternate_sets = 0;
  std::uint64_t active_attempts = 0;
  std::uint64_t evidence_entries = 0;
  std::uint64_t fencing_events = 0;
};

struct PromoteResponse {
  PromotionDecision decision;
  GrantId grant;
  AttemptId attempt;
};

struct AttemptResponse {
  bool found = false;
  AttemptRecord record;
};

struct LineageResponse {
  bool truncated = false;
  std::vector<LineageEntry> entries;
};

// ---- codecs ----------------------------------------------------------------
[[nodiscard]] std::vector<std::byte> encode_hello_request(const HelloRequest& value);
[[nodiscard]] Result<HelloRequest> decode_hello_request(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_hello_response(const HelloResponse& value);
[[nodiscard]] Result<HelloResponse> decode_hello_response(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_error_response(const ErrorResponse& value);
[[nodiscard]] Result<ErrorResponse> decode_error_response(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_status_response(const StatusResponse& value);
[[nodiscard]] Result<StatusResponse> decode_status_response(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_promote_response(const PromoteResponse& value);
[[nodiscard]] Result<PromoteResponse> decode_promote_response(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_attempt_response(const AttemptResponse& value);
[[nodiscard]] Result<AttemptResponse> decode_attempt_response(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode_lineage_response(const LineageResponse& value);
[[nodiscard]] Result<LineageResponse> decode_lineage_response(std::span<const std::byte> bytes);

// A request payload is the session header followed by an operation body.
[[nodiscard]] std::vector<std::byte> encode_session_request(const SessionHeader& header,
                                                            std::span<const std::byte> body);
[[nodiscard]] Result<SessionHeader> decode_session_header(ByteReader& reader);

[[nodiscard]] std::vector<std::byte> encode_request_body(const PromotionRequest& value);
[[nodiscard]] Result<PromotionRequest> decode_request_body(ByteReader& reader);
[[nodiscard]] std::vector<std::byte> encode_effect_body(AttemptId attempt,
                                                        const EffectEvidence& effect);
[[nodiscard]] Result<std::pair<AttemptId, EffectEvidence>> decode_effect_body(ByteReader& reader);
[[nodiscard]] std::vector<std::byte> encode_abandon_body(AttemptId attempt, Reason reason);
[[nodiscard]] Result<std::pair<AttemptId, Reason>> decode_abandon_body(ByteReader& reader);
[[nodiscard]] std::vector<std::byte> encode_rollback_body(AttemptId attempt,
                                                          const PathEvidence& evidence);
[[nodiscard]] Result<std::pair<AttemptId, PathEvidence>> decode_rollback_body(ByteReader& reader);
[[nodiscard]] std::vector<std::byte> encode_fence_body(PathId path, Reason reason);
[[nodiscard]] Result<std::pair<PathId, Reason>> decode_fence_body(ByteReader& reader);
[[nodiscard]] std::vector<std::byte> encode_attempt_body(AttemptId attempt);
[[nodiscard]] Result<AttemptId> decode_attempt_body(ByteReader& reader);
[[nodiscard]] std::vector<std::byte> encode_lineage_body(std::uint32_t limit);
[[nodiscard]] Result<std::uint32_t> decode_lineage_body(ByteReader& reader);

}  // namespace pff
