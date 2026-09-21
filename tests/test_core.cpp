// Core unit suite: identities, status vocabulary, canonical encoding, checked
// arithmetic, authority classification, model validation and wire round trips.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstring>
#include <string>
#include <vector>

#include "framework.hpp"
#include "pff/authority.hpp"
#include "pff/canonical.hpp"
#include "pff/checked.hpp"
#include "pff/crc32c.hpp"
#include "pff/ids.hpp"
#include "pff/model.hpp"
#include "pff/status.hpp"
#include "pff/wire.hpp"

using namespace pff;

namespace {

std::span<const std::byte> as_bytes(const std::string& text) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

AuthorityVector make_authority(std::uint64_t topology, std::uint64_t path_authority,
                               std::uint64_t policy, std::uint64_t obligations,
                               std::uint64_t epoch, std::uint64_t boot) {
  AuthorityVector authority;
  authority.topology = Generation::from_value(topology);
  authority.path_authority = Generation::from_value(path_authority);
  authority.policy = Generation::from_value(policy);
  authority.obligations = Generation::from_value(obligations);
  authority.epoch = Epoch::from_value(epoch);
  authority.boot = BootId::from_value(boot);
  return authority;
}

}  // namespace

PFF_TEST(core, crc32c_reference_vectors) {
  PFF_CHECK_EQ(crc32c(as_bytes("123456789")), 0xE3069283u);
  PFF_CHECK_EQ(crc32c(std::span<const std::byte>()), 0x00000000u);
  PFF_CHECK_EQ(crc32c(as_bytes("a")), 0xC1D04330u);
}

PFF_TEST(core, fnv1a_is_stable) {
  PFF_CHECK_EQ(fnv1a64(as_bytes("")), 1469598103934665603ull);
  PFF_CHECK(fnv1a64(as_bytes("a")) != fnv1a64(as_bytes("b")));
}

PFF_TEST(core, identity_semantics) {
  const PathId absent;
  PFF_CHECK(!absent.valid());
  const PathId one = PathId::from_value(1);
  PFF_CHECK(one.valid());
  PFF_CHECK(one != absent);
  PFF_CHECK(absent < one);
  PFF_CHECK_EQ(std::hash<PathId>{}(one), std::hash<PathId>{}(PathId::from_value(1)));
  PFF_CHECK_EQ(id_hex(PathId::from_value(0x2a)), std::string("000000000000002a"));
}

PFF_TEST(core, checked_arithmetic_detects_overflow) {
  std::int64_t out = 0;
  PFF_CHECK(checked_add(2, 3, out) && out == 5);
  PFF_CHECK(!checked_add(INT64_MAX, 1, out));
  PFF_CHECK(!checked_add(INT64_MIN, -1, out));
  PFF_CHECK(checked_mul(-4, 3, out) && out == -12);
  PFF_CHECK(!checked_mul(INT64_MAX, 2, out));
  PFF_CHECK(checked_mul(INT64_MIN, 1, out) && out == INT64_MIN);
  PFF_CHECK(!checked_mul(INT64_MIN, -1, out));
  PFF_CHECK(checked_mul(0, INT64_MIN, out) && out == 0);
  PFF_CHECK(!checked_sub(INT64_MIN, 1, out));
}

PFF_TEST(core, byte_writer_bounds_are_enforced) {
  ByteWriter writer(0, 4);
  writer.u32(7);
  PFF_CHECK(writer.ok());
  writer.u8(1);
  PFF_CHECK(!writer.ok());
  PFF_CHECK_EQ(writer.status().code, Code::exhausted);
}

PFF_TEST(core, byte_reader_rejects_truncation_and_trailing_bytes) {
  const std::uint8_t raw[5] = {1, 2, 0, 0, 0};
  ByteReader reader(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw), 3));
  PFF_CHECK_EQ(reader.u16(), 0x0201);
  PFF_CHECK(reader.ok());
  static_cast<void>(reader.u32());
  PFF_CHECK(!reader.ok());
  PFF_CHECK_EQ(reader.status().reason, Reason::length_invalid);

  ByteReader trailing(std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw), 5));
  PFF_CHECK_EQ(trailing.u8(), 1);
  PFF_CHECK(!trailing.require_end().ok());
  PFF_CHECK_EQ(trailing.status().reason, Reason::trailing_garbage);
}

PFF_TEST(core, byte_reader_rejects_boolean_and_string_abuse) {
  const std::uint8_t bad_bool[1] = {2};
  ByteReader reader(std::span<const std::byte>(reinterpret_cast<const std::byte*>(bad_bool), 1));
  static_cast<void>(reader.boolean());
  PFF_CHECK(!reader.ok());
  PFF_CHECK_EQ(reader.status().reason, Reason::enum_invalid);

  ByteWriter writer(16, 64);
  writer.u16(5000);
  writer.u8(0);
  ByteReader string_reader(writer.data());
  const auto value = string_reader.string(16);
  PFF_CHECK(!value.ok());
  PFF_CHECK_EQ(value.status().reason, Reason::length_invalid);
}

PFF_TEST(core, authority_classification) {
  const AuthorityVector current = make_authority(5, 6, 7, 8, 3, 0xAA);
  PFF_CHECK_EQ(classify(current, current).overall, AuthorityClass::current);

  AuthorityVector stale = current;
  stale.path_authority = Generation::from_value(4);
  const AuthorityComparison stale_result = classify(stale, current);
  PFF_CHECK_EQ(stale_result.overall, AuthorityClass::stale);
  PFF_CHECK_EQ(stale_result.reason, Reason::path_authority_generation_stale);

  AuthorityVector future = current;
  future.policy = Generation::from_value(9);
  PFF_CHECK_EQ(classify(future, current).overall, AuthorityClass::conflict);

  AuthorityVector unknown = current;
  unknown.obligations = Generation::absent();
  const AuthorityComparison unknown_result = classify(unknown, current);
  PFF_CHECK_EQ(unknown_result.overall, AuthorityClass::unknown);
  PFF_CHECK_EQ(unknown_result.reason, Reason::authority_unknown);

  AuthorityVector other_boot = current;
  other_boot.boot = BootId::from_value(0xBB);
  PFF_CHECK_EQ(classify(other_boot, current).overall, AuthorityClass::stale);
  PFF_CHECK_EQ(classify(other_boot, current).reason, Reason::boot_mismatch);

  AuthorityVector other_epoch = current;
  other_epoch.epoch = Epoch::from_value(2);
  PFF_CHECK_EQ(classify(other_epoch, current).overall, AuthorityClass::stale);
  PFF_CHECK_EQ(classify(other_epoch, current).reason, Reason::epoch_stale);

  // Conflict outranks unknown, which outranks stale.
  AuthorityVector mixed = current;
  mixed.policy = Generation::from_value(9);
  mixed.topology = Generation::absent();
  PFF_CHECK_EQ(classify(mixed, current).overall, AuthorityClass::conflict);
}

PFF_TEST(core, policy_validation_rejects_unbounded_values) {
  FailoverPolicy policy;
  policy.generation = Generation::from_value(1);
  PFF_CHECK(policy.validate().ok());

  FailoverPolicy no_generation;
  PFF_CHECK(!no_generation.validate().ok());

  FailoverPolicy heavy = policy;
  heavy.w_health = limits::max_weight + 1;
  PFF_CHECK(!heavy.validate().ok());

  FailoverPolicy no_search = policy;
  no_search.search_limit = 0;
  PFF_CHECK(!no_search.validate().ok());

  FailoverPolicy huge_search = policy;
  huge_search.search_limit = limits::max_search_limit + 1;
  PFF_CHECK(!huge_search.validate().ok());
}

PFF_TEST(core, obligations_and_evidence_validation) {
  ServiceObligations obligations;
  PFF_CHECK(!obligations.validate().ok());
  obligations.generation = Generation::from_value(1);
  PFF_CHECK(obligations.validate().ok());
  obligations.min_health_ppm = limits::health_scale + 1;
  PFF_CHECK(!obligations.validate().ok());

  PathEvidence evidence;
  PFF_CHECK(!evidence.validate().ok());
  evidence.path = PathId::from_value(1);
  PFF_CHECK(evidence.validate().ok());
  evidence.cost_units = limits::cost_scale + 1;
  PFF_CHECK(!evidence.validate().ok());
}

PFF_TEST(core, alternate_set_validation_rejects_duplicates_and_mismatch) {
  AlternateSet set;
  set.id = AlternateSetId::from_value(1);
  set.generation = Generation::from_value(1);
  PFF_CHECK(set.validate().ok());

  AlternateCandidate first;
  first.path = PathId::from_value(10);
  first.evidence.path = first.path;
  AlternateCandidate duplicate = first;
  set.candidates = {first, duplicate};
  PFF_CHECK_EQ(set.validate().reason, Reason::duplicate_path);

  AlternateCandidate mismatched = first;
  mismatched.path = PathId::from_value(11);
  mismatched.evidence.path = PathId::from_value(12);
  set.candidates = {mismatched};
  const Status status = set.validate();
  PFF_CHECK(!status.ok());
  PFF_CHECK_EQ(status.reason, Reason::domain_invalid);

  AlternateCandidate high_rank = first;
  high_rank.upstream_rank = static_cast<std::uint32_t>(limits::rank_scale) + 1u;
  set.candidates = {high_rank};
  PFF_CHECK_EQ(set.validate().reason, Reason::rank_out_of_range);
}

PFF_TEST(core, objective_is_exact_and_checked) {
  FailoverPolicy policy;
  policy.generation = Generation::from_value(1);
  policy.w_health = 2;
  policy.w_cost = 3;
  policy.w_rank = 5;
  policy.w_capability = 7;

  ServiceObligations obligations;
  obligations.generation = Generation::from_value(1);
  obligations.required_capabilities = 0x3u;

  PathEvidence evidence;
  evidence.path = PathId::from_value(1);
  evidence.health_ppm = 1000;
  evidence.cost_units = 10;
  evidence.capabilities = 0x1u;

  auto terms = compute_score(policy, obligations, evidence, 4);
  PFF_CHECK(terms.ok());
  PFF_CHECK_EQ(terms.value().health, 2000);
  PFF_CHECK_EQ(terms.value().cost, 3 * (limits::cost_scale - 10));
  PFF_CHECK_EQ(terms.value().rank, 5 * (limits::rank_scale - 4));
  PFF_CHECK_EQ(terms.value().capability, 7);
  PFF_CHECK_EQ(terms.value().total,
               terms.value().health + terms.value().cost + terms.value().rank +
                   terms.value().capability);

  // A policy whose weights are inside the validated bound can never overflow for
  // in-range evidence; the checked arithmetic is still proven to fire when a
  // caller reaches this lower layer with an out-of-range weight.
  FailoverPolicy overflowing;
  overflowing.generation = Generation::from_value(1);
  overflowing.w_health = INT64_MAX / 2;
  const auto overflow = compute_score(overflowing, obligations, evidence, 0);
  PFF_CHECK(!overflow.ok());
  PFF_CHECK_EQ(overflow.status().reason, Reason::score_overflow);
}

PFF_TEST(core, every_enumerator_has_a_stable_name) {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(Code::table_full); ++raw) {
    const std::string name = to_string(static_cast<Code>(raw));
    PFF_CHECK(name != "invalid_code");
  }
  for (std::uint16_t raw = 0; raw <= static_cast<std::uint16_t>(Reason::authority_fenced); ++raw) {
    const std::string name = to_string(static_cast<Reason>(raw));
    PFF_CHECK(name != "invalid_reason");
  }
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(AttemptState::interrupted); ++raw) {
    PFF_CHECK(std::string(to_string(static_cast<AttemptState>(raw))) != "INVALID");
  }
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(PromotionOutcome::rolled_back); ++raw) {
    PFF_CHECK(std::string(to_string(static_cast<PromotionOutcome>(raw))) != "INVALID_OUTCOME");
  }
  PFF_CHECK(std::string(to_string(static_cast<PromotionOutcome>(0x7F))) == "INVALID_OUTCOME");
  PFF_CHECK(std::string(to_string(static_cast<CandidateVerdict>(0x7F))) == "INVALID_VERDICT");
  PFF_CHECK(std::string(to_string(Provenance::real)) == "REAL");
  PFF_CHECK(std::string(to_string(Provenance::synthetic)) == "SYNTHETIC");
  PFF_CHECK(std::string(to_string(Provenance::unsupported)) == "UNSUPPORTED");
  PFF_CHECK(weakest(Provenance::real, Provenance::synthetic) == Provenance::synthetic);
  PFF_CHECK(weakest(Provenance::synthetic, Provenance::unsupported) == Provenance::unsupported);
}

PFF_TEST(core, wire_round_trip_every_type) {
  const AuthorityVector authority = make_authority(1, 2, 3, 4, 5, 6);

  PathEvidence evidence;
  evidence.id = EvidenceId::from_value(9);
  evidence.path = PathId::from_value(11);
  evidence.freshness = Freshness::fresh;
  evidence.eligibility = Eligibility::eligible;
  evidence.reachability = Reachability::reachable;
  evidence.capacity = CapacityClass::sufficient;
  evidence.capabilities = 0x5u;
  evidence.health_ppm = 900000;
  evidence.cost_units = 42;
  evidence.observation_seq = 3;
  evidence.observed_under = authority;
  evidence.provenance = Provenance::real;

  {
    ByteWriter writer(256, limits::max_record_payload);
    wire::put(writer, evidence);
    PFF_CHECK(writer.ok());
    ByteReader reader(writer.data());
    auto decoded = wire::get_evidence(reader);
    PFF_CHECK(decoded.ok());
    PFF_CHECK(reader.at_end());
    PFF_CHECK(decoded.value().path == evidence.path);
    PFF_CHECK_EQ(decoded.value().health_ppm, evidence.health_ppm);
    PFF_CHECK(decoded.value().observed_under == authority);
    PFF_CHECK(decoded.value().provenance == Provenance::real);
  }
  {
    FailoverPolicy policy;
    policy.generation = Generation::from_value(2);
    policy.w_health = -7;
    policy.min_score = -12345;
    policy.allow_unknown_skip = true;
    ByteWriter writer(128, limits::max_record_payload);
    wire::put(writer, policy);
    ByteReader reader(writer.data());
    auto decoded = wire::get_policy(reader);
    PFF_CHECK(decoded.ok());
    PFF_CHECK(reader.at_end());
    PFF_CHECK(decoded.value() == policy);
  }
  {
    ServiceObligations obligations;
    obligations.generation = Generation::from_value(4);
    obligations.required_capabilities = 0x9u;
    obligations.min_health_ppm = 123;
    obligations.max_cost_units = 456;
    ByteWriter writer(64, limits::max_record_payload);
    wire::put(writer, obligations);
    ByteReader reader(writer.data());
    auto decoded = wire::get_obligations(reader);
    PFF_CHECK(decoded.ok());
    PFF_CHECK(decoded.value() == obligations);
  }
  {
    AlternateSet set;
    set.id = AlternateSetId::from_value(3);
    set.generation = Generation::from_value(9);
    set.completeness = SetCompleteness::partial;
    AlternateCandidate candidate;
    candidate.path = PathId::from_value(77);
    candidate.upstream_rank = 2;
    candidate.evidence = evidence;
    candidate.evidence.path = candidate.path;
    candidate.authority = authority;
    set.candidates.push_back(candidate);
    ByteWriter writer(512, limits::max_record_payload);
    wire::put(writer, set);
    ByteReader reader(writer.data());
    auto decoded = wire::get_set(reader);
    PFF_CHECK(decoded.ok());
    PFF_CHECK(reader.at_end());
    PFF_CHECK_EQ(decoded.value().candidates.size(), std::size_t{1});
    PFF_CHECK(decoded.value().candidates[0].path == candidate.path);
    PFF_CHECK(decoded.value().completeness == SetCompleteness::partial);
  }
}

PFF_TEST(core, wire_rejects_invalid_enums_and_counts) {
  ByteWriter writer(64, limits::max_record_payload);
  wire::put(writer, make_authority(1, 1, 1, 1, 1, 1));
  std::vector<std::byte> bytes(writer.data().begin(), writer.data().end());
  // Corrupt the provenance byte of a path evidence record instead.
  PathEvidence evidence;
  evidence.path = PathId::from_value(1);
  ByteWriter evidence_writer(64, limits::max_record_payload);
  wire::put(evidence_writer, evidence);
  std::vector<std::byte> evidence_bytes(evidence_writer.data().begin(),
                                        evidence_writer.data().end());
  evidence_bytes.back() = static_cast<std::byte>(0x7F);
  ByteReader reader(std::span<const std::byte>(evidence_bytes.data(), evidence_bytes.size()));
  const auto decoded = wire::get_evidence(reader);
  PFF_CHECK(!decoded.ok());
  PFF_CHECK_EQ(decoded.status().reason, Reason::enum_invalid);

  // A declared candidate count above the bound must be refused before allocation.
  ByteWriter count_writer(32, 64);
  count_writer.u32(0xFFFFFFFFu);
  ByteReader count_reader(count_writer.data());
  const auto count = wire::get_count(count_reader, limits::max_candidates_per_set);
  PFF_CHECK(!count.ok());
  PFF_CHECK_EQ(count.status().reason, Reason::length_invalid);
}

PFF_TEST(core, score_terms_must_be_self_consistent) {
  CandidateAssessment assessment;
  assessment.path = PathId::from_value(1);
  assessment.terms.health = 5;
  assessment.terms.total = 5;
  ByteWriter writer(128, limits::max_record_payload);
  wire::put(writer, assessment);
  std::vector<std::byte> bytes(writer.data().begin(), writer.data().end());
  // The total field is the fifth i64 after path (8) + rank (4) + 4 terms.
  const std::size_t total_offset = 8 + 4 + 8 * 4;
  bytes[total_offset] = static_cast<std::byte>(0x42);
  ByteReader reader(std::span<const std::byte>(bytes.data(), bytes.size()));
  const auto decoded = wire::get_assessment(reader);
  PFF_CHECK(!decoded.ok());
  PFF_CHECK_EQ(decoded.status().reason, Reason::domain_invalid);
}

PFF_TEST_MAIN()
