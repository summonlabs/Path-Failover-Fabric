// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/protocol.hpp"

#include <algorithm>
#include <cstring>

#include "pff/crc32c.hpp"
#include "pff/version.hpp"
#include "pff/wire.hpp"

namespace pff {
namespace {

constexpr std::uint16_t kMaxReason = max_reason_value;
constexpr std::uint8_t kMaxCode = max_code_value;

std::uint32_t read_u32_at(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + i])) << (8u * i);
  }
  return value;
}

std::vector<std::byte> finish(ByteWriter& writer) {
  if (!writer.ok()) {
    return {};
  }
  return std::move(writer).take();
}

Status finish_reader(ByteReader& reader) {
  if (!reader.ok()) {
    return reader.status();
  }
  return reader.require_end();
}

Result<Reason> read_reason(ByteReader& reader) {
  const std::uint16_t raw = reader.u16();
  if (!reader.ok()) {
    return reader.status();
  }
  if (raw > kMaxReason) {
    return Status(Code::invalid, Reason::enum_invalid, "reason out of range");
  }
  return static_cast<Reason>(raw);
}

void put_fabric_stats(ByteWriter& w, const FabricStats& s) {
  w.u64(s.promotions_authorized);
  w.u64(s.promotions_committed);
  w.u64(s.promotions_refused);
  w.u64(s.promotions_indeterminate);
  w.u64(s.fences_recorded);
  w.u64(s.stale_completions_rejected);
  w.u64(s.rollbacks);
  w.u64(s.reversions);
  w.u64(s.revalidations);
  w.u64(s.journal_records);
  w.u64(s.snapshots_written);
  w.u64(s.restarts);
  w.u64(s.evidence_accepted);
  w.u64(s.evidence_rejected);
  w.u64(s.attempts_dropped);
}

Result<FabricStats> get_fabric_stats(ByteReader& r) {
  FabricStats s;
  s.promotions_authorized = r.u64();
  s.promotions_committed = r.u64();
  s.promotions_refused = r.u64();
  s.promotions_indeterminate = r.u64();
  s.fences_recorded = r.u64();
  s.stale_completions_rejected = r.u64();
  s.rollbacks = r.u64();
  s.reversions = r.u64();
  s.revalidations = r.u64();
  s.journal_records = r.u64();
  s.snapshots_written = r.u64();
  s.restarts = r.u64();
  s.evidence_accepted = r.u64();
  s.evidence_rejected = r.u64();
  s.attempts_dropped = r.u64();
  if (!r.ok()) {
    return r.status();
  }
  return s;
}

void put_recovery(ByteWriter& w, const RecoveryReport& v) {
  w.boolean(v.snapshot_loaded);
  w.u64(v.snapshot_sequence);
  w.u64(v.records_replayed);
  w.u64(v.records_skipped);
  w.boolean(v.torn_tail_present);
  w.boolean(v.torn_tail_recovered);
  w.u64(v.truncated_bytes);
  w.boolean(v.inspection_only);
  w.boolean(v.journal_absent);
  w.u64(v.journal_bytes);
}

Result<RecoveryReport> get_recovery(ByteReader& r) {
  RecoveryReport v;
  v.snapshot_loaded = r.boolean();
  v.snapshot_sequence = r.u64();
  v.records_replayed = static_cast<std::size_t>(r.u64());
  v.records_skipped = static_cast<std::size_t>(r.u64());
  v.torn_tail_present = r.boolean();
  v.torn_tail_recovered = r.boolean();
  v.truncated_bytes = r.u64();
  v.inspection_only = r.boolean();
  v.journal_absent = r.boolean();
  v.journal_bytes = r.u64();
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

}  // namespace

const char* to_string(MsgType t) noexcept {
  switch (t) {
    case MsgType::hello:
      return "hello";
    case MsgType::hello_ack:
      return "hello_ack";
    case MsgType::error_rsp:
      return "error";
    case MsgType::ok_rsp:
      return "ok";
    case MsgType::status_req:
      return "status_req";
    case MsgType::status_rsp:
      return "status_rsp";
    case MsgType::set_policy_req:
      return "set_policy_req";
    case MsgType::set_obligations_req:
      return "set_obligations_req";
    case MsgType::put_set_req:
      return "put_set_req";
    case MsgType::put_evidence_req:
      return "put_evidence_req";
    case MsgType::set_authority_req:
      return "set_authority_req";
    case MsgType::evaluate_req:
      return "evaluate_req";
    case MsgType::evaluate_rsp:
      return "evaluate_rsp";
    case MsgType::promote_req:
      return "promote_req";
    case MsgType::promote_rsp:
      return "promote_rsp";
    case MsgType::effect_req:
      return "effect_req";
    case MsgType::abandon_req:
      return "abandon_req";
    case MsgType::rollback_req:
      return "rollback_req";
    case MsgType::revert_req:
      return "revert_req";
    case MsgType::fence_req:
      return "fence_req";
    case MsgType::attempt_req:
      return "attempt_req";
    case MsgType::attempt_rsp:
      return "attempt_rsp";
    case MsgType::lineage_req:
      return "lineage_req";
    case MsgType::lineage_rsp:
      return "lineage_rsp";
    case MsgType::shutdown_req:
      return "shutdown_req";
    case MsgType::shutdown_rsp:
      return "shutdown_rsp";
  }
  return "invalid";
}

bool is_known_msg_type(std::uint16_t raw) noexcept {
  return raw >= static_cast<std::uint16_t>(MsgType::hello) &&
         raw <= static_cast<std::uint16_t>(MsgType::shutdown_rsp);
}

bool is_request_type(MsgType t) noexcept {
  switch (t) {
    case MsgType::hello:
    case MsgType::status_req:
    case MsgType::set_policy_req:
    case MsgType::set_obligations_req:
    case MsgType::put_set_req:
    case MsgType::put_evidence_req:
    case MsgType::set_authority_req:
    case MsgType::evaluate_req:
    case MsgType::promote_req:
    case MsgType::effect_req:
    case MsgType::abandon_req:
    case MsgType::rollback_req:
    case MsgType::revert_req:
    case MsgType::fence_req:
    case MsgType::attempt_req:
    case MsgType::lineage_req:
    case MsgType::shutdown_req:
      return true;
    default:
      return false;
  }
}

bool is_response_type(MsgType t) noexcept { return !is_request_type(t); }

// ---- framing ---------------------------------------------------------------

std::vector<std::byte> encode_frame(MsgType type, std::uint64_t sequence, std::uint32_t flags,
                                    std::span<const std::byte> payload) {
  if (payload.size() > limits::max_frame_payload || flags > max_frame_flags) {
    return {};
  }
  ByteWriter writer(limits::frame_header_bytes + payload.size() + limits::frame_trailer_bytes,
                    limits::max_frame_bytes);
  writer.u32(frame_magic);
  writer.u16(protocol_version);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u32(flags);
  writer.u64(sequence);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  if (!writer.ok()) {
    return {};
  }
  writer.u32(crc32c(writer.data()));
  writer.bytes(payload);
  if (!writer.ok()) {
    return {};
  }
  writer.u32(crc32c(payload));
  return finish(writer);
}

FrameDecoder::FrameDecoder(std::size_t max_payload)
    : max_payload_(std::min(max_payload, limits::max_frame_payload)) {
  buffer_.reserve(std::min<std::size_t>(limits::max_frame_bytes, 65536));
}

Status FrameDecoder::fail(Code code, Reason reason, const char* detail) {
  if (status_.ok()) {
    status_ = Status(code, reason, detail == nullptr ? std::string() : std::string(detail));
  }
  return status_;
}

void FrameDecoder::compact() {
  if (consumed_ == 0) {
    return;
  }
  if (consumed_ >= buffer_.size()) {
    buffer_.clear();
    consumed_ = 0;
    return;
  }
  std::memmove(buffer_.data(), buffer_.data() + consumed_, buffer_.size() - consumed_);
  buffer_.resize(buffer_.size() - consumed_);
  consumed_ = 0;
}

Status FrameDecoder::feed(std::span<const std::byte> data) {
  if (!status_.ok()) {
    return status_;
  }
  if (data.empty()) {
    return ok_status();
  }
  if (data.size() > limits::max_frame_bytes - std::min(available(), limits::max_frame_bytes)) {
    return fail(Code::exhausted, Reason::frame_oversize, "frame buffer bound exceeded");
  }
  compact();
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  return ok_status();
}

bool FrameDecoder::has_frame() const {
  if (!status_.ok()) {
    return false;
  }
  const std::size_t available_bytes = available();
  if (available_bytes < limits::frame_header_bytes) {
    return false;
  }
  const std::span<const std::byte> window(buffer_.data() + consumed_, available_bytes);
  const std::span<const std::byte> header = window.first(limits::frame_header_bytes);
  const auto read_u16 = [](std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset]) |
                                      (static_cast<std::uint16_t>(
                                           static_cast<std::uint8_t>(bytes[offset + 1]))
                                       << 8));
  };
  if (read_u32_at(header, 0) != frame_magic || read_u16(header, 4) != protocol_version ||
      !is_known_msg_type(read_u16(header, 6)) || read_u32_at(header, 8) > max_frame_flags) {
    return false;
  }
  const std::uint32_t payload_length = read_u32_at(header, 20);
  if (payload_length > max_payload_) {
    return false;
  }
  if (crc32c(header.first(limits::frame_header_bytes - limits::frame_trailer_bytes)) !=
      read_u32_at(header, 24)) {
    return false;
  }
  const std::size_t total = limits::frame_header_bytes +
                            static_cast<std::size_t>(payload_length) +
                            limits::frame_trailer_bytes;
  return available_bytes >= total;
}

Result<Frame> FrameDecoder::pop() {
  if (!status_.ok()) {
    return status_;
  }
  const std::size_t available_bytes = available();
  if (available_bytes < limits::frame_header_bytes) {
    return Status(Code::unknown, Reason::frame_truncated, "frame header incomplete");
  }
  const std::span<const std::byte> window(buffer_.data() + consumed_, available_bytes);
  const std::span<const std::byte> header = window.first(limits::frame_header_bytes);
  ByteReader reader(header);
  const std::uint32_t magic = reader.u32();
  const std::uint16_t version = reader.u16();
  const std::uint16_t type = reader.u16();
  const std::uint32_t flags = reader.u32();
  const std::uint64_t sequence = reader.u64();
  const std::uint32_t payload_length = reader.u32();
  const std::uint32_t header_crc = reader.u32();
  if (!reader.ok() || !reader.at_end()) {
    return fail(Code::invalid, Reason::length_invalid, "frame header truncated");
  }
  if (magic != frame_magic) {
    return fail(Code::invalid, Reason::frame_magic, "frame magic mismatch");
  }
  if (version != protocol_version) {
    return fail(Code::version_unsupported, Reason::protocol_version_mismatch,
                "protocol version mismatch");
  }
  if (!is_known_msg_type(type)) {
    return fail(Code::invalid, Reason::enum_invalid, "frame type out of range");
  }
  if (flags > max_frame_flags) {
    return fail(Code::invalid, Reason::domain_invalid, "frame flags must be zero");
  }
  if (payload_length > max_payload_) {
    return fail(Code::invalid, Reason::frame_oversize, "declared payload exceeds bound");
  }
  if (crc32c(header.first(limits::frame_header_bytes - limits::frame_trailer_bytes)) !=
      header_crc) {
    return fail(Code::corrupt, Reason::corrupt_header, "frame header integrity failure");
  }
  const std::size_t total = limits::frame_header_bytes +
                            static_cast<std::size_t>(payload_length) +
                            limits::frame_trailer_bytes;
  if (available_bytes < total) {
    return Status(Code::unknown, Reason::frame_truncated, "frame payload incomplete");
  }
  const std::span<const std::byte> payload = window.subspan(limits::frame_header_bytes,
                                                            payload_length);
  const std::uint32_t stored_crc =
      read_u32_at(window, limits::frame_header_bytes + payload_length);
  if (stored_crc != crc32c(payload)) {
    return fail(Code::corrupt, Reason::integrity_failure, "frame payload integrity failure");
  }
  Frame frame;
  frame.version = version;
  frame.type = static_cast<MsgType>(type);
  frame.flags = flags;
  frame.sequence = sequence;
  frame.payload.assign(payload.begin(), payload.end());
  consumed_ += total;
  if (consumed_ == buffer_.size()) {
    buffer_.clear();
    consumed_ = 0;
  }
  return frame;
}

// ---- payload codecs --------------------------------------------------------

std::vector<std::byte> encode_hello_request(const HelloRequest& value) {
  ByteWriter writer(64, limits::max_record_payload);
  writer.u16(value.protocol);
  writer.string(value.client, limits::max_name_bytes);
  writer.u64(value.nonce);
  return finish(writer);
}

Result<HelloRequest> decode_hello_request(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  HelloRequest value;
  value.protocol = reader.u16();
  auto client = reader.string(limits::max_name_bytes);
  if (!client.ok()) {
    return client.status();
  }
  value.client.assign(client.value());
  value.nonce = reader.u64();
  const Status end = finish_reader(reader);
  if (!end.ok()) {
    return end;
  }
  return value;
}

std::vector<std::byte> encode_hello_response(const HelloResponse& value) {
  ByteWriter writer(128, limits::max_record_payload);
  writer.u16(value.protocol);
  writer.u64(value.session.value());
  writer.u64(value.epoch.value());
  writer.u64(value.boot.value());
  writer.u64(value.incarnation.value());
  wire::put(writer, value.authority);
  writer.u64(value.max_requests_per_session);
  return finish(writer);
}

Result<HelloResponse> decode_hello_response(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  HelloResponse value;
  value.protocol = reader.u16();
  value.session = SessionId::from_value(reader.u64());
  value.epoch = Epoch::from_value(reader.u64());
  value.boot = BootId::from_value(reader.u64());
  value.incarnation = IncarnationId::from_value(reader.u64());
  auto authority = wire::get_authority(reader);
  if (!authority.ok()) {
    return authority.status();
  }
  value.authority = authority.value();
  value.max_requests_per_session = reader.u64();
  const Status end = finish_reader(reader);
  if (!end.ok()) {
    return end;
  }
  return value;
}

std::vector<std::byte> encode_error_response(const ErrorResponse& value) {
  ByteWriter writer(64, limits::max_record_payload);
  writer.u8(static_cast<std::uint8_t>(value.code));
  writer.u16(static_cast<std::uint16_t>(value.reason));
  // The detail field is a bounded diagnostic. It is truncated to the documented
  // bound rather than allowed to fail the whole response.
  std::string detail = value.detail;
  if (detail.size() > limits::max_reason_bytes) {
    detail.resize(limits::max_reason_bytes);
  }
  writer.string(detail, limits::max_reason_bytes);
  return finish(writer);
}

Result<ErrorResponse> decode_error_response(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  ErrorResponse value;
  const std::uint8_t code = reader.u8();
  if (!reader.ok()) {
    return reader.status();
  }
  if (code > kMaxCode) {
    return Status(Code::invalid, Reason::enum_invalid, "error code out of range");
  }
  value.code = static_cast<Code>(code);
  auto reason = read_reason(reader);
  if (!reason.ok()) {
    return reason.status();
  }
  value.reason = reason.value();
  auto detail = reader.string(limits::max_reason_bytes);
  if (!detail.ok()) {
    return detail.status();
  }
  value.detail.assign(detail.value());
  const Status end = finish_reader(reader);
  if (!end.ok()) {
    return end;
  }
  return value;
}

std::vector<std::byte> encode_status_response(const StatusResponse& value) {
  ByteWriter writer(256, limits::max_record_payload);
  writer.u64(value.incarnation.value());
  writer.u64(value.epoch.value());
  writer.u64(value.boot.value());
  wire::put(writer, value.authority);
  put_fabric_stats(writer, value.stats);
  put_recovery(writer, value.recovery);
  writer.u64(value.active_sessions);
  writer.u64(value.requests_handled);
  writer.boolean(value.policy_installed);
  writer.boolean(value.obligations_installed);
  writer.u64(value.alternate_sets);
  writer.u64(value.active_attempts);
  writer.u64(value.evidence_entries);
  writer.u64(value.fencing_events);
  return finish(writer);
}

Result<StatusResponse> decode_status_response(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  StatusResponse value;
  value.incarnation = IncarnationId::from_value(reader.u64());
  value.epoch = Epoch::from_value(reader.u64());
  value.boot = BootId::from_value(reader.u64());
  auto authority = wire::get_authority(reader);
  if (!authority.ok()) {
    return authority.status();
  }
  value.authority = authority.value();
  auto stats = get_fabric_stats(reader);
  if (!stats.ok()) {
    return stats.status();
  }
  value.stats = stats.value();
  auto recovery = get_recovery(reader);
  if (!recovery.ok()) {
    return recovery.status();
  }
  value.recovery = recovery.value();
  value.active_sessions = reader.u64();
  value.requests_handled = reader.u64();
  value.policy_installed = reader.boolean();
  value.obligations_installed = reader.boolean();
  value.alternate_sets = reader.u64();
  value.active_attempts = reader.u64();
  value.evidence_entries = reader.u64();
  value.fencing_events = reader.u64();
  const Status end = finish_reader(reader);
  if (!end.ok()) {
    return end;
  }
  return value;
}

std::vector<std::byte> encode_promote_response(const PromoteResponse& value) {
  ByteWriter writer(512, limits::max_record_payload);
  wire::put(writer, value.decision);
  writer.u64(value.grant.value());
  writer.u64(value.attempt.value());
  return finish(writer);
}

Result<PromoteResponse> decode_promote_response(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  PromoteResponse value;
  auto decision = wire::get_decision(reader);
  if (!decision.ok()) {
    return decision.status();
  }
  value.decision = std::move(decision).value();
  value.grant = GrantId::from_value(reader.u64());
  value.attempt = AttemptId::from_value(reader.u64());
  const Status end = finish_reader(reader);
  if (!end.ok()) {
    return end;
  }
  return value;
}

std::vector<std::byte> encode_attempt_response(const AttemptResponse& value) {
  ByteWriter writer(160, limits::max_record_payload);
  writer.boolean(value.found);
  wire::put(writer, value.record);
  return finish(writer);
}

Result<AttemptResponse> decode_attempt_response(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  AttemptResponse value;
  value.found = reader.boolean();
  auto record = wire::get_attempt(reader);
  if (!record.ok()) {
    return record.status();
  }
  value.record = record.value();
  const Status end = finish_reader(reader);
  if (!end.ok()) {
    return end;
  }
  return value;
}

std::vector<std::byte> encode_lineage_response(const LineageResponse& value) {
  ByteWriter writer(512, limits::max_record_payload);
  writer.boolean(value.truncated);
  writer.u32(static_cast<std::uint32_t>(value.entries.size()));
  for (const LineageEntry& entry : value.entries) {
    wire::put(writer, entry);
  }
  return finish(writer);
}

Result<LineageResponse> decode_lineage_response(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  LineageResponse value;
  value.truncated = reader.boolean();
  auto count = wire::get_count(reader, limits::max_lineage);
  if (!count.ok()) {
    return count.status();
  }
  value.entries.reserve(count.value());
  for (std::size_t i = 0; i < count.value(); ++i) {
    auto entry = wire::get_lineage(reader);
    if (!entry.ok()) {
      return entry.status();
    }
    value.entries.push_back(entry.value());
  }
  const Status end = finish_reader(reader);
  if (!end.ok()) {
    return end;
  }
  return value;
}

std::vector<std::byte> encode_session_request(const SessionHeader& header,
                                              std::span<const std::byte> body) {
  ByteWriter writer(64 + body.size(), limits::max_record_payload);
  writer.u64(header.session.value());
  writer.u64(header.epoch.value());
  writer.u64(header.boot.value());
  writer.u64(header.request_sequence);
  writer.bytes(body);
  return finish(writer);
}

Result<SessionHeader> decode_session_header(ByteReader& reader) {
  SessionHeader header;
  header.session = SessionId::from_value(reader.u64());
  header.epoch = Epoch::from_value(reader.u64());
  header.boot = BootId::from_value(reader.u64());
  header.request_sequence = reader.u64();
  if (!reader.ok()) {
    return reader.status();
  }
  return header;
}

std::vector<std::byte> encode_request_body(const PromotionRequest& value) {
  ByteWriter writer(96, limits::max_record_payload);
  wire::put(writer, value);
  return finish(writer);
}

Result<PromotionRequest> decode_request_body(ByteReader& reader) {
  return wire::get_request(reader);
}

std::vector<std::byte> encode_effect_body(AttemptId attempt, const EffectEvidence& effect) {
  ByteWriter writer(96, limits::max_record_payload);
  writer.u64(attempt.value());
  wire::put(writer, effect);
  return finish(writer);
}

Result<std::pair<AttemptId, EffectEvidence>> decode_effect_body(ByteReader& reader) {
  const AttemptId attempt = AttemptId::from_value(reader.u64());
  auto effect = wire::get_effect(reader);
  if (!effect.ok()) {
    return effect.status();
  }
  if (!reader.ok()) {
    return reader.status();
  }
  return std::make_pair(attempt, effect.value());
}

std::vector<std::byte> encode_abandon_body(AttemptId attempt, Reason reason) {
  ByteWriter writer(16, limits::max_record_payload);
  writer.u64(attempt.value());
  writer.u16(static_cast<std::uint16_t>(reason));
  return finish(writer);
}

Result<std::pair<AttemptId, Reason>> decode_abandon_body(ByteReader& reader) {
  const AttemptId attempt = AttemptId::from_value(reader.u64());
  auto reason = read_reason(reader);
  if (!reason.ok()) {
    return reason.status();
  }
  return std::make_pair(attempt, reason.value());
}

std::vector<std::byte> encode_rollback_body(AttemptId attempt, const PathEvidence& evidence) {
  ByteWriter writer(160, limits::max_record_payload);
  writer.u64(attempt.value());
  wire::put(writer, evidence);
  return finish(writer);
}

Result<std::pair<AttemptId, PathEvidence>> decode_rollback_body(ByteReader& reader) {
  const AttemptId attempt = AttemptId::from_value(reader.u64());
  auto evidence = wire::get_evidence(reader);
  if (!evidence.ok()) {
    return evidence.status();
  }
  if (!reader.ok()) {
    return reader.status();
  }
  return std::make_pair(attempt, evidence.value());
}

std::vector<std::byte> encode_fence_body(PathId path, Reason reason) {
  ByteWriter writer(16, limits::max_record_payload);
  writer.u64(path.value());
  writer.u16(static_cast<std::uint16_t>(reason));
  return finish(writer);
}

Result<std::pair<PathId, Reason>> decode_fence_body(ByteReader& reader) {
  const PathId path = PathId::from_value(reader.u64());
  auto reason = read_reason(reader);
  if (!reason.ok()) {
    return reason.status();
  }
  return std::make_pair(path, reason.value());
}

std::vector<std::byte> encode_attempt_body(AttemptId attempt) {
  ByteWriter writer(8, limits::max_record_payload);
  writer.u64(attempt.value());
  return finish(writer);
}

Result<AttemptId> decode_attempt_body(ByteReader& reader) {
  const AttemptId attempt = AttemptId::from_value(reader.u64());
  if (!reader.ok()) {
    return reader.status();
  }
  return attempt;
}

std::vector<std::byte> encode_lineage_body(std::uint32_t limit) {
  ByteWriter writer(4, limits::max_record_payload);
  writer.u32(limit);
  return finish(writer);
}

Result<std::uint32_t> decode_lineage_body(ByteReader& reader) {
  const std::uint32_t limit = reader.u32();
  if (!reader.ok()) {
    return reader.status();
  }
  if (limit == 0 || limit > limits::max_lineage) {
    return Status(Code::invalid, Reason::domain_invalid, "lineage limit out of range");
  }
  return limit;
}

}  // namespace pff
