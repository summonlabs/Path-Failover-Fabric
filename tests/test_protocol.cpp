// Protocol suite: bounded framed codec, adversarial frames, message payloads.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <string>
#include <vector>

#include "framework.hpp"
#include "pff/crc32c.hpp"
#include "pff/protocol.hpp"
#include "pff/version.hpp"
#include "support.hpp"

using namespace pff;

namespace {

std::vector<std::byte> payload_of(std::size_t size, std::byte fill) {
  return std::vector<std::byte>(size, fill);
}

std::vector<std::byte> corrupt_byte(std::vector<std::byte> bytes, std::size_t index,
                                    std::uint8_t mask) {
  if (index < bytes.size()) {
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(bytes[index]) ^ mask);
  }
  return bytes;
}

}  // namespace

PFF_TEST(protocol, frame_round_trip) {
  const std::vector<std::byte> payload = payload_of(37, std::byte{0xAB});
  const std::vector<std::byte> frame =
      encode_frame(MsgType::status_req, 42, 0, std::span<const std::byte>(payload.data(), payload.size()));
  PFF_REQUIRE(!frame.empty());
  PFF_CHECK_EQ(frame.size(), limits::frame_header_bytes + payload.size() + limits::frame_trailer_bytes);

  FrameDecoder decoder;
  PFF_CHECK(decoder.feed(std::span<const std::byte>(frame.data(), frame.size())).ok());
  PFF_CHECK(decoder.has_frame());
  auto decoded = decoder.pop();
  PFF_REQUIRE(decoded.ok());
  PFF_CHECK(decoded.value().type == MsgType::status_req);
  PFF_CHECK_EQ(decoded.value().sequence, std::uint64_t{42});
  PFF_CHECK_EQ(decoded.value().flags, std::uint32_t{0});
  PFF_CHECK_EQ(decoded.value().version, protocol_version);
  PFF_CHECK(decoded.value().payload == payload);
  PFF_CHECK(!decoder.has_frame());
  PFF_CHECK(!decoder.failed());
}

PFF_TEST(protocol, every_truncated_prefix_waits_for_more_bytes) {
  const std::vector<std::byte> payload = payload_of(64, std::byte{0x11});
  const std::vector<std::byte> frame =
      encode_frame(MsgType::hello, 7, 0, std::span<const std::byte>(payload.data(), payload.size()));
  for (std::size_t length = 0; length < frame.size(); ++length) {
    FrameDecoder decoder;
    PFF_CHECK(decoder.feed(std::span<const std::byte>(frame.data(), length)).ok());
    PFF_CHECK(!decoder.has_frame());
    auto decoded = decoder.pop();
    PFF_CHECK(!decoded.ok());
    PFF_CHECK(!decoder.failed());
    PFF_CHECK_EQ(decoded.status().reason, Reason::frame_truncated);
  }
  // Feeding the same frame one byte at a time also works.
  FrameDecoder incremental;
  for (std::size_t index = 0; index < frame.size(); ++index) {
    PFF_CHECK(incremental.feed(std::span<const std::byte>(frame.data() + index, 1)).ok());
  }
  auto decoded = incremental.pop();
  PFF_REQUIRE(decoded.ok());
  PFF_CHECK(decoded.value().payload == payload);
}

PFF_TEST(protocol, header_field_corruption_is_sticky) {
  const std::vector<std::byte> payload = payload_of(8, std::byte{0x22});
  const std::vector<std::byte> good =
      encode_frame(MsgType::lineage_req, 3, 0, std::span<const std::byte>(payload.data(), payload.size()));

  const auto expect_failure = [&](const std::vector<std::byte>& frame, Reason reason,
                                  const char* label) {
    FrameDecoder decoder;
    PFF_CHECK(decoder.feed(std::span<const std::byte>(frame.data(), frame.size())).ok());
    auto decoded = decoder.pop();
    PFF_CHECK_MSG(!decoded.ok(), label);
    if (decoded.ok()) {
      return;
    }
    PFF_CHECK_MSG(decoded.status().reason == reason, label);
    PFF_CHECK_MSG(decoder.failed(), label);
    // The failure is sticky: later feeds and pops keep failing identically.
    PFF_CHECK(!decoder.feed(std::span<const std::byte>(good.data(), good.size())).ok());
    PFF_CHECK(!decoder.pop().ok());
  };

  expect_failure(corrupt_byte(good, 0, 0xFF), Reason::frame_magic, "magic");
  expect_failure(corrupt_byte(good, 4, 0xFF), Reason::protocol_version_mismatch, "version");
  expect_failure(corrupt_byte(good, 6, 0xFF), Reason::enum_invalid, "type");
  expect_failure(corrupt_byte(good, 8, 0x01), Reason::domain_invalid, "flags");
  expect_failure(corrupt_byte(good, 24, 0xFF), Reason::corrupt_header, "header crc");
  expect_failure(corrupt_byte(good, good.size() - 1, 0x01), Reason::integrity_failure, "payload crc");
}

PFF_TEST(protocol, oversized_declared_length_is_refused_before_allocation) {
  ByteWriter writer(64, 128);
  writer.u32(frame_magic);
  writer.u16(protocol_version);
  writer.u16(static_cast<std::uint16_t>(MsgType::hello));
  writer.u32(0);
  writer.u64(1);
  writer.u32(0x7FFFFFFFu);
  writer.u32(crc32c(writer.data()));
  const std::vector<std::byte> frame = std::move(writer).take();

  FrameDecoder decoder;
  PFF_CHECK(decoder.feed(std::span<const std::byte>(frame.data(), frame.size())).ok());
  auto decoded = decoder.pop();
  PFF_CHECK(!decoded.ok());
  PFF_CHECK_EQ(decoded.status().reason, Reason::frame_oversize);
  PFF_CHECK(decoder.failed());

  // A decoder with a smaller bound refuses a frame that the default accepts.
  const std::vector<std::byte> payload = payload_of(4096, std::byte{0x33});
  const std::vector<std::byte> big =
      encode_frame(MsgType::hello, 1, 0, std::span<const std::byte>(payload.data(), payload.size()));
  FrameDecoder small(1024);
  PFF_CHECK(small.feed(std::span<const std::byte>(big.data(), big.size())).ok());
  PFF_CHECK_EQ(small.pop().status().reason, Reason::frame_oversize);
}

PFF_TEST(protocol, buffer_bound_is_enforced) {
  FrameDecoder decoder(64);
  const std::vector<std::byte> flood(limits::max_frame_bytes + 1, std::byte{0x00});
  const Status status = decoder.feed(std::span<const std::byte>(flood.data(), flood.size()));
  PFF_CHECK(!status.ok());
  PFF_CHECK_EQ(status.reason, Reason::frame_oversize);
  PFF_CHECK(decoder.failed());
}

PFF_TEST(protocol, trailing_bytes_form_the_next_frame) {
  const std::vector<std::byte> first = encode_frame(MsgType::hello, 1, 0, {});
  const std::vector<std::byte> second = encode_frame(MsgType::hello_ack, 2, 0, {});
  std::vector<std::byte> stream = first;
  stream.insert(stream.end(), second.begin(), second.end());
  FrameDecoder decoder;
  PFF_CHECK(decoder.feed(std::span<const std::byte>(stream.data(), stream.size())).ok());
  auto one = decoder.pop();
  PFF_REQUIRE(one.ok());
  PFF_CHECK(one.value().type == MsgType::hello);
  auto two = decoder.pop();
  PFF_REQUIRE(two.ok());
  PFF_CHECK(two.value().type == MsgType::hello_ack);
  PFF_CHECK(!decoder.has_frame());

  // Garbage that is too short to be a header is a truncated prefix, not silence.
  FrameDecoder garbage;
  const std::vector<std::byte> junk(5, std::byte{0x5A});
  PFF_CHECK(garbage.feed(std::span<const std::byte>(junk.data(), junk.size())).ok());
  auto none = garbage.pop();
  PFF_CHECK(!none.ok());
  PFF_CHECK_EQ(none.status().reason, Reason::frame_truncated);
  PFF_CHECK(!garbage.failed());
  const std::vector<std::byte> rest(23, std::byte{0x5A});
  PFF_CHECK(garbage.feed(std::span<const std::byte>(rest.data(), rest.size())).ok());
  PFF_CHECK_EQ(garbage.pop().status().reason, Reason::frame_magic);
  PFF_CHECK(garbage.failed());
}

PFF_TEST(protocol, zero_length_payload_is_valid) {
  const std::vector<std::byte> frame = encode_frame(MsgType::ok_rsp, 5, 0, {});
  FrameDecoder decoder;
  PFF_CHECK(decoder.feed(std::span<const std::byte>(frame.data(), frame.size())).ok());
  auto decoded = decoder.pop();
  PFF_REQUIRE(decoded.ok());
  PFF_CHECK(decoded.value().payload.empty());
  PFF_CHECK(decoded.value().type == MsgType::ok_rsp);
}

PFF_TEST(protocol, message_types_are_classified) {
  for (std::uint16_t raw = 0; raw <= 0x00FFu; ++raw) {
    if (is_known_msg_type(raw)) {
      const MsgType type = static_cast<MsgType>(raw);
      PFF_CHECK(is_request_type(type) != is_response_type(type));
      PFF_CHECK(std::string(to_string(type)) != "invalid");
    } else {
      PFF_CHECK(std::string(to_string(static_cast<MsgType>(raw))) == "invalid");
    }
  }
  PFF_CHECK(is_request_type(MsgType::evaluate_req));
  PFF_CHECK(is_response_type(MsgType::evaluate_rsp));
  PFF_CHECK(is_request_type(MsgType::hello));
  PFF_CHECK(is_response_type(MsgType::hello_ack));
}

PFF_TEST(protocol, message_payloads_round_trip) {
  {
    HelloRequest request;
    request.protocol = protocol_version;
    request.client = "unit-test";
    request.nonce = 0xDEADBEEF;
    const std::vector<std::byte> bytes = encode_hello_request(request);
    auto decoded = decode_hello_request(std::span<const std::byte>(bytes.data(), bytes.size()));
    PFF_REQUIRE(decoded.ok());
    PFF_CHECK_EQ(decoded.value().client, std::string("unit-test"));
    PFF_CHECK_EQ(decoded.value().nonce, request.nonce);
  }
  {
    HelloResponse response;
    response.session = SessionId::from_value(9);
    response.epoch = Epoch::from_value(4);
    response.boot = BootId::from_value(77);
    response.incarnation = IncarnationId::from_value(88);
    response.authority = pfftest::make_authority(1, 2, 3, 4, 4, 77);
    response.max_requests_per_session = 1000;
    const std::vector<std::byte> bytes = encode_hello_response(response);
    auto decoded = decode_hello_response(std::span<const std::byte>(bytes.data(), bytes.size()));
    PFF_REQUIRE(decoded.ok());
    PFF_CHECK(decoded.value().session == response.session);
    PFF_CHECK(decoded.value().authority == response.authority);
  }
  {
    ErrorResponse response;
    response.code = Code::stale;
    response.reason = Reason::session_stale;
    response.detail = "session authority is no longer current";
    const std::vector<std::byte> bytes = encode_error_response(response);
    auto decoded = decode_error_response(std::span<const std::byte>(bytes.data(), bytes.size()));
    PFF_REQUIRE(decoded.ok());
    PFF_CHECK(decoded.value().code == Code::stale);
    PFF_CHECK(decoded.value().reason == Reason::session_stale);
  }
  {
    StatusResponse response;
    response.incarnation = IncarnationId::from_value(3);
    response.epoch = Epoch::from_value(2);
    response.boot = BootId::from_value(5);
    response.authority = pfftest::make_authority(1, 1, 1, 1, 2, 5);
    response.stats.promotions_committed = 7;
    response.recovery.snapshot_loaded = true;
    response.recovery.records_replayed = 12;
    response.policy_installed = true;
    response.alternate_sets = 2;
    response.fencing_events = 3;
    const std::vector<std::byte> bytes = encode_status_response(response);
    auto decoded = decode_status_response(std::span<const std::byte>(bytes.data(), bytes.size()));
    PFF_REQUIRE(decoded.ok());
    PFF_CHECK_EQ(decoded.value().stats.promotions_committed, std::uint64_t{7});
    PFF_CHECK(decoded.value().recovery.snapshot_loaded);
    PFF_CHECK_EQ(decoded.value().recovery.records_replayed, std::size_t{12});
    PFF_CHECK(decoded.value().policy_installed);
  }
  {
    LineageResponse response;
    LineageEntry entry;
    entry.decision = DecisionId::from_value(1);
    entry.attempt = AttemptId::from_value(2);
    entry.epoch = Epoch::from_value(2);
    entry.boot = BootId::from_value(5);
    entry.outcome = PromotionOutcome::promoted;
    entry.incumbent = PathId::from_value(1);
    entry.selected = PathId::from_value(2);
    entry.set = AlternateSetId::from_value(3);
    entry.basis = pfftest::make_authority(1, 1, 1, 1, 2, 5);
    entry.reason = Reason::selected;
    entry.sequence = 9;
    response.entries.push_back(entry);
    response.truncated = true;
    const std::vector<std::byte> bytes = encode_lineage_response(response);
    auto decoded = decode_lineage_response(std::span<const std::byte>(bytes.data(), bytes.size()));
    PFF_REQUIRE(decoded.ok());
    PFF_CHECK_EQ(decoded.value().entries.size(), std::size_t{1});
    PFF_CHECK(decoded.value().truncated);
    PFF_CHECK(decoded.value().entries[0].selected == entry.selected);
  }
  {
    PromotionRequest request;
    request.incumbent = PathId::from_value(1);
    request.set = AlternateSetId::from_value(3);
    request.set_generation = Generation::from_value(4);
    request.policy_generation = Generation::from_value(5);
    request.obligations_generation = Generation::from_value(6);
    request.topology_generation = Generation::from_value(7);
    request.path_authority_generation = Generation::from_value(8);
    request.epoch = Epoch::from_value(9);
    request.boot = BootId::from_value(10);
    const std::vector<std::byte> body = encode_request_body(request);
    ByteReader reader(std::span<const std::byte>(body.data(), body.size()));
    auto decoded = decode_request_body(reader);
    PFF_REQUIRE(decoded.ok());
    PFF_CHECK(reader.at_end());
    PFF_CHECK(decoded.value().basis() == request.basis());
  }
  {
    SessionHeader header;
    header.session = SessionId::from_value(11);
    header.epoch = Epoch::from_value(12);
    header.boot = BootId::from_value(13);
    header.request_sequence = 14;
    const std::vector<std::byte> body = payload_of(3, std::byte{0x01});
    const std::vector<std::byte> bytes = encode_session_request(header, std::span<const std::byte>(body.data(), body.size()));
    ByteReader reader(std::span<const std::byte>(bytes.data(), bytes.size()));
    auto decoded = decode_session_header(reader);
    PFF_REQUIRE(decoded.ok());
    PFF_CHECK(decoded.value().session == header.session);
    PFF_CHECK_EQ(decoded.value().request_sequence, header.request_sequence);
    PFF_CHECK_EQ(reader.remaining(), std::size_t{3});
  }
}

PFF_TEST(protocol, request_bodies_reject_truncation_and_trailing_bytes) {
  const std::vector<std::byte> effect = encode_effect_body(
      AttemptId::from_value(4), EffectEvidence{EvidenceId::from_value(1), PathId::from_value(2), 3, true,
                                               pfftest::make_authority(1, 1, 1, 1, 1, 1),
                                               Provenance::synthetic});
  PFF_REQUIRE(!effect.empty());
  for (std::size_t length = 0; length < effect.size(); ++length) {
    ByteReader reader(std::span<const std::byte>(effect.data(), length));
    const auto decoded = decode_effect_body(reader);
    PFF_CHECK(!decoded.ok());
  }
  std::vector<std::byte> extended = effect;
  extended.push_back(std::byte{0x00});
  ByteReader reader(std::span<const std::byte>(extended.data(), extended.size()));
  PFF_CHECK(decode_effect_body(reader).ok());
  PFF_CHECK(!reader.require_end().ok());
  PFF_CHECK_EQ(reader.status().reason, Reason::trailing_garbage);

  const std::vector<std::byte> fence = encode_fence_body(PathId::from_value(9), Reason::path_fenced);
  for (std::size_t length = 0; length < fence.size(); ++length) {
    ByteReader truncated(std::span<const std::byte>(fence.data(), length));
    PFF_CHECK(!decode_fence_body(truncated).ok());
  }

  // Out of range reasons are refused.
  std::vector<std::byte> bad_reason = fence;
  bad_reason[8] = static_cast<std::byte>(0xFF);
  bad_reason[9] = static_cast<std::byte>(0xFF);
  ByteReader bad(std::span<const std::byte>(bad_reason.data(), bad_reason.size()));
  const auto decoded = decode_fence_body(bad);
  PFF_CHECK(!decoded.ok());
  PFF_CHECK_EQ(decoded.status().reason, Reason::enum_invalid);
}

PFF_TEST(protocol, lineage_and_attempt_bounds_are_enforced) {
  // A declared lineage entry count above the bound is refused before allocation.
  ByteWriter writer(64, 128);
  writer.boolean(false);
  writer.u32(0xFFFFFFFFu);
  const auto decoded = decode_lineage_response(writer.data());
  PFF_CHECK(!decoded.ok());
  PFF_CHECK_EQ(decoded.status().reason, Reason::length_invalid);

  const std::vector<std::byte> tiny = encode_lineage_body(0);
  ByteReader zero_reader(std::span<const std::byte>(tiny.data(), tiny.size()));
  PFF_CHECK(!decode_lineage_body(zero_reader).ok());

  const std::vector<std::byte> huge = encode_lineage_body(0xFFFFFFFFu);
  ByteReader huge_reader(std::span<const std::byte>(huge.data(), huge.size()));
  PFF_CHECK(!decode_lineage_body(huge_reader).ok());

  const std::vector<std::byte> attempt = encode_attempt_body(AttemptId::from_value(7));
  ByteReader attempt_reader(std::span<const std::byte>(attempt.data(), attempt.size()));
  const auto id = decode_attempt_body(attempt_reader);
  PFF_REQUIRE(id.ok());
  PFF_CHECK(id.value() == AttemptId::from_value(7));

  ByteReader short_reader(std::span<const std::byte>(attempt.data(), 4));
  PFF_CHECK(!decode_attempt_body(short_reader).ok());
}

PFF_TEST(protocol, error_response_detail_is_bounded) {
  ErrorResponse response;
  response.code = Code::refused;
  response.reason = Reason::message_unsupported;
  response.detail = std::string(4096, 'x');
  const std::vector<std::byte> bytes = encode_error_response(response);
  auto decoded = decode_error_response(std::span<const std::byte>(bytes.data(), bytes.size()));
  PFF_REQUIRE(decoded.ok());
  PFF_CHECK(decoded.value().detail.size() <= limits::max_reason_bytes);
}

PFF_TEST_MAIN()
