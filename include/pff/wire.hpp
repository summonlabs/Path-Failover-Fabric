// Path Failover Fabric - canonical encoders and decoders for domain values.
//
// Decoding is total and validating: every enum is range-checked, every count is
// bounded before allocation, and every decoder rejects trailing bytes.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <vector>

#include "pff/canonical.hpp"
#include "pff/decision.hpp"
#include "pff/model.hpp"
#include "pff/records.hpp"

namespace pff::wire {

template <typename E>
void put_enum(ByteWriter& w, E value) {
  w.u8(static_cast<std::uint8_t>(value));
}

// Reads an enum stored as u8 and rejects every value above max_inclusive.
template <typename E>
Result<E> get_enum(ByteReader& r, std::uint8_t max_inclusive, Reason invalid_reason) {
  const std::uint8_t raw = r.u8();
  if (!r.ok()) {
    return r.status();
  }
  if (raw > max_inclusive) {
    return Status(Code::invalid, invalid_reason);
  }
  return static_cast<E>(raw);
}

Result<std::size_t> get_count(ByteReader& r, std::size_t max_count);

void put(ByteWriter& w, const AuthorityVector& v);
Result<AuthorityVector> get_authority(ByteReader& r);

void put(ByteWriter& w, const PathEvidence& v);
Result<PathEvidence> get_evidence(ByteReader& r);

void put(ByteWriter& w, const EffectEvidence& v);
Result<EffectEvidence> get_effect(ByteReader& r);

void put(ByteWriter& w, const ServiceObligations& v);
Result<ServiceObligations> get_obligations(ByteReader& r);

void put(ByteWriter& w, const FailoverPolicy& v);
Result<FailoverPolicy> get_policy(ByteReader& r);

void put(ByteWriter& w, const AlternateCandidate& v);
Result<AlternateCandidate> get_candidate(ByteReader& r);

void put(ByteWriter& w, const AlternateSet& v);
Result<AlternateSet> get_set(ByteReader& r);

void put(ByteWriter& w, const CandidateAssessment& v);
Result<CandidateAssessment> get_assessment(ByteReader& r);

void put(ByteWriter& w, const PromotionDecision& v);
Result<PromotionDecision> get_decision(ByteReader& r);

void put(ByteWriter& w, const PromotionRequest& v);
Result<PromotionRequest> get_request(ByteReader& r);

void put(ByteWriter& w, const AttemptRecord& v);
Result<AttemptRecord> get_attempt(ByteReader& r);

void put(ByteWriter& w, const FenceRecord& v);
Result<FenceRecord> get_fence(ByteReader& r);

void put(ByteWriter& w, const LineageEntry& v);
Result<LineageEntry> get_lineage(ByteReader& r);

}  // namespace pff::wire
