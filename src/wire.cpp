// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/wire.hpp"

#include "pff/checked.hpp"
#include "pff/records.hpp"

namespace pff::wire {
namespace {

constexpr std::uint8_t kProvenanceMax = static_cast<std::uint8_t>(Provenance::unsupported);
constexpr std::uint8_t kFreshnessMax = static_cast<std::uint8_t>(Freshness::unknown);
constexpr std::uint8_t kEligibilityMax = static_cast<std::uint8_t>(Eligibility::unknown);
constexpr std::uint8_t kReachabilityMax = static_cast<std::uint8_t>(Reachability::unknown);
constexpr std::uint8_t kCapacityMax = static_cast<std::uint8_t>(CapacityClass::unknown);
constexpr std::uint8_t kCompletenessMax = static_cast<std::uint8_t>(SetCompleteness::unknown);
constexpr std::uint8_t kConditionMax = static_cast<std::uint8_t>(IncumbentCondition::degraded);
constexpr std::uint8_t kOutcomeMax = static_cast<std::uint8_t>(PromotionOutcome::rolled_back);
constexpr std::uint8_t kAuthorityKindMax = static_cast<std::uint8_t>(AuthorityKind::verified_effect);
constexpr std::uint8_t kVerdictMax = static_cast<std::uint8_t>(CandidateVerdict::indeterminate);
constexpr std::uint8_t kAttemptStateMax = static_cast<std::uint8_t>(AttemptState::interrupted);
constexpr std::uint8_t kAuthorityClassMax = static_cast<std::uint8_t>(AuthorityClass::conflict);

void put_authority_class(ByteWriter& w, AuthorityClass c) { w.u8(static_cast<std::uint8_t>(c)); }

Result<AuthorityClass> get_authority_class(ByteReader& r) {
  return get_enum<AuthorityClass>(r, kAuthorityClassMax, Reason::enum_invalid);
}

void put_terms(ByteWriter& w, const ScoreTerms& t) {
  w.i64(t.health);
  w.i64(t.cost);
  w.i64(t.rank);
  w.i64(t.capability);
  w.i64(t.total);
}

Result<ScoreTerms> get_terms(ByteReader& r) {
  ScoreTerms t;
  t.health = r.i64();
  t.cost = r.i64();
  t.rank = r.i64();
  t.capability = r.i64();
  t.total = r.i64();
  if (!r.ok()) {
    return r.status();
  }
  // A total that does not equal the sum of its parts is a contradictory
  // document and is refused rather than normalised.
  std::int64_t sum = 0;
  if (!checked_add(t.health, t.cost, sum) || !checked_add(sum, t.rank, sum) ||
      !checked_add(sum, t.capability, sum)) {
    return Status(Code::invalid, Reason::domain_invalid, "score terms overflow");
  }
  if (sum != t.total) {
    return Status(Code::invalid, Reason::domain_invalid, "score total inconsistent with terms");
  }
  return t;
}

}  // namespace

Result<std::size_t> get_count(ByteReader& r, std::size_t max_count) {
  const std::uint32_t raw = r.u32();
  if (!r.ok()) {
    return r.status();
  }
  if (static_cast<std::size_t>(raw) > max_count) {
    return Status(Code::invalid, Reason::length_invalid, "declared count exceeds bound");
  }
  return static_cast<std::size_t>(raw);
}

void put(ByteWriter& w, const AuthorityVector& v) {
  w.u64(v.topology.value());
  w.u64(v.path_authority.value());
  w.u64(v.policy.value());
  w.u64(v.obligations.value());
  w.u64(v.epoch.value());
  w.u64(v.boot.value());
}

Result<AuthorityVector> get_authority(ByteReader& r) {
  AuthorityVector v;
  v.topology = Generation::from_value(r.u64());
  v.path_authority = Generation::from_value(r.u64());
  v.policy = Generation::from_value(r.u64());
  v.obligations = Generation::from_value(r.u64());
  v.epoch = Epoch::from_value(r.u64());
  v.boot = BootId::from_value(r.u64());
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

void put(ByteWriter& w, const PathEvidence& v) {
  w.u64(v.id.value());
  w.u64(v.path.value());
  put_enum(w, v.freshness);
  put_enum(w, v.eligibility);
  put_enum(w, v.reachability);
  put_enum(w, v.capacity);
  w.u32(v.capabilities);
  w.i64(v.health_ppm);
  w.i64(v.cost_units);
  w.u64(v.observation_seq);
  put(w, v.observed_under);
  put_enum(w, v.provenance);
}

Result<PathEvidence> get_evidence(ByteReader& r) {
  PathEvidence v;
  v.id = EvidenceId::from_value(r.u64());
  v.path = PathId::from_value(r.u64());
  auto freshness = get_enum<Freshness>(r, kFreshnessMax, Reason::enum_invalid);
  if (!freshness.ok()) {
    return freshness.status();
  }
  v.freshness = freshness.value();
  auto eligibility = get_enum<Eligibility>(r, kEligibilityMax, Reason::enum_invalid);
  if (!eligibility.ok()) {
    return eligibility.status();
  }
  v.eligibility = eligibility.value();
  auto reachability = get_enum<Reachability>(r, kReachabilityMax, Reason::enum_invalid);
  if (!reachability.ok()) {
    return reachability.status();
  }
  v.reachability = reachability.value();
  auto capacity = get_enum<CapacityClass>(r, kCapacityMax, Reason::enum_invalid);
  if (!capacity.ok()) {
    return capacity.status();
  }
  v.capacity = capacity.value();
  v.capabilities = r.u32();
  v.health_ppm = r.i64();
  v.cost_units = r.i64();
  v.observation_seq = r.u64();
  auto authority = get_authority(r);
  if (!authority.ok()) {
    return authority.status();
  }
  v.observed_under = authority.value();
  auto provenance = get_enum<Provenance>(r, kProvenanceMax, Reason::enum_invalid);
  if (!provenance.ok()) {
    return provenance.status();
  }
  v.provenance = provenance.value();
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

void put(ByteWriter& w, const EffectEvidence& v) {
  w.u64(v.id.value());
  w.u64(v.path.value());
  w.u64(v.observation_seq);
  w.boolean(v.effect_verified);
  put(w, v.observed_under);
  put_enum(w, v.provenance);
}

Result<EffectEvidence> get_effect(ByteReader& r) {
  EffectEvidence v;
  v.id = EvidenceId::from_value(r.u64());
  v.path = PathId::from_value(r.u64());
  v.observation_seq = r.u64();
  v.effect_verified = r.boolean();
  auto authority = get_authority(r);
  if (!authority.ok()) {
    return authority.status();
  }
  v.observed_under = authority.value();
  auto provenance = get_enum<Provenance>(r, kProvenanceMax, Reason::enum_invalid);
  if (!provenance.ok()) {
    return provenance.status();
  }
  v.provenance = provenance.value();
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

void put(ByteWriter& w, const ServiceObligations& v) {
  w.u64(v.generation.value());
  w.u32(v.required_capabilities);
  w.i64(v.min_health_ppm);
  w.i64(v.max_cost_units);
  w.boolean(v.require_reachable);
  w.boolean(v.require_capacity);
  w.boolean(v.require_fresh_evidence);
}

Result<ServiceObligations> get_obligations(ByteReader& r) {
  ServiceObligations v;
  v.generation = Generation::from_value(r.u64());
  v.required_capabilities = r.u32();
  v.min_health_ppm = r.i64();
  v.max_cost_units = r.i64();
  v.require_reachable = r.boolean();
  v.require_capacity = r.boolean();
  v.require_fresh_evidence = r.boolean();
  if (!r.ok()) {
    return r.status();
  }
  const Status status = v.validate();
  if (!status.ok()) {
    return status;
  }
  return v;
}

void put(ByteWriter& w, const FailoverPolicy& v) {
  w.u64(v.generation.value());
  w.i64(v.w_health);
  w.i64(v.w_cost);
  w.i64(v.w_rank);
  w.i64(v.w_capability);
  w.i64(v.min_score);
  w.u32(v.search_limit);
  w.u32(v.max_retained_attempts);
  w.u32(v.max_revalidations);
  w.boolean(v.allow_unknown_skip);
  w.boolean(v.require_verified_effect);
}

Result<FailoverPolicy> get_policy(ByteReader& r) {
  FailoverPolicy v;
  v.generation = Generation::from_value(r.u64());
  v.w_health = r.i64();
  v.w_cost = r.i64();
  v.w_rank = r.i64();
  v.w_capability = r.i64();
  v.min_score = r.i64();
  v.search_limit = r.u32();
  v.max_retained_attempts = r.u32();
  v.max_revalidations = r.u32();
  v.allow_unknown_skip = r.boolean();
  v.require_verified_effect = r.boolean();
  if (!r.ok()) {
    return r.status();
  }
  const Status status = v.validate();
  if (!status.ok()) {
    return status;
  }
  return v;
}

void put(ByteWriter& w, const AlternateCandidate& v) {
  w.u64(v.path.value());
  w.u32(v.upstream_rank);
  w.boolean(v.withdrawn);
  put(w, v.evidence);
  put(w, v.authority);
}

Result<AlternateCandidate> get_candidate(ByteReader& r) {
  AlternateCandidate v;
  v.path = PathId::from_value(r.u64());
  v.upstream_rank = r.u32();
  v.withdrawn = r.boolean();
  auto evidence = get_evidence(r);
  if (!evidence.ok()) {
    return evidence.status();
  }
  v.evidence = evidence.value();
  auto authority = get_authority(r);
  if (!authority.ok()) {
    return authority.status();
  }
  v.authority = authority.value();
  if (!r.ok()) {
    return r.status();
  }
  const Status status = v.validate();
  if (!status.ok()) {
    return status;
  }
  return v;
}

void put(ByteWriter& w, const AlternateSet& v) {
  w.u64(v.id.value());
  w.u64(v.generation.value());
  put_enum(w, v.completeness);
  w.u32(static_cast<std::uint32_t>(v.candidates.size()));
  for (const AlternateCandidate& candidate : v.candidates) {
    put(w, candidate);
  }
}

Result<AlternateSet> get_set(ByteReader& r) {
  AlternateSet v;
  v.id = AlternateSetId::from_value(r.u64());
  v.generation = Generation::from_value(r.u64());
  auto completeness = get_enum<SetCompleteness>(r, kCompletenessMax, Reason::enum_invalid);
  if (!completeness.ok()) {
    return completeness.status();
  }
  v.completeness = completeness.value();
  auto count = get_count(r, limits::max_candidates_per_set);
  if (!count.ok()) {
    return count.status();
  }
  v.candidates.reserve(count.value());
  for (std::size_t i = 0; i < count.value(); ++i) {
    auto candidate = get_candidate(r);
    if (!candidate.ok()) {
      return candidate.status();
    }
    v.candidates.push_back(std::move(candidate).value());
  }
  if (!r.ok()) {
    return r.status();
  }
  const Status status = v.validate();
  if (!status.ok()) {
    return status;
  }
  return v;
}

void put(ByteWriter& w, const CandidateAssessment& v) {
  w.u64(v.path.value());
  w.u32(v.upstream_rank);
  put_terms(w, v.terms);
  put_enum(w, v.verdict);
  w.u16(static_cast<std::uint16_t>(v.reason));
  put_authority_class(w, v.authority);
}

Result<CandidateAssessment> get_assessment(ByteReader& r) {
  CandidateAssessment v;
  v.path = PathId::from_value(r.u64());
  v.upstream_rank = r.u32();
  auto terms = get_terms(r);
  if (!terms.ok()) {
    return terms.status();
  }
  v.terms = terms.value();
  auto verdict = get_enum<CandidateVerdict>(r, kVerdictMax, Reason::enum_invalid);
  if (!verdict.ok()) {
    return verdict.status();
  }
  v.verdict = verdict.value();
  const std::uint16_t raw_reason = r.u16();
  if (!r.ok()) {
    return r.status();
  }
  if (raw_reason > max_reason_value) {
    return Status(Code::invalid, Reason::enum_invalid, "assessment reason out of range");
  }
  v.reason = static_cast<Reason>(raw_reason);
  auto authority = get_authority_class(r);
  if (!authority.ok()) {
    return authority.status();
  }
  v.authority = authority.value();
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

void put(ByteWriter& w, const PromotionDecision& v) {
  w.u64(v.id.value());
  put_enum(w, v.outcome);
  put_enum(w, v.authority_kind);
  w.u64(v.incumbent.value());
  w.u64(v.set.value());
  w.u64(v.set_generation.value());
  w.u64(v.policy_generation.value());
  w.u64(v.obligations_generation.value());
  put(w, v.basis);
  w.u64(v.attempt.value());
  w.u64(v.selected.value());
  w.u32(static_cast<std::uint32_t>(v.ranked.size()));
  for (const CandidateAssessment& assessment : v.ranked) {
    put(w, assessment);
  }
  w.u32(static_cast<std::uint32_t>(v.indeterminate_above.size()));
  for (const PathId path : v.indeterminate_above) {
    w.u64(path.value());
  }
  w.u32(static_cast<std::uint32_t>(v.explanation.size()));
  for (const auto& [reason, value] : v.explanation) {
    w.u16(static_cast<std::uint16_t>(reason));
    w.i64(value);
  }
  w.u64(v.examined);
  w.u64(v.supplied);
  w.boolean(v.search_limited);
  w.boolean(v.set_complete);
  w.u16(static_cast<std::uint16_t>(v.reason));
  put_enum(w, v.provenance);
  // SelectionStats is deliberately absent: comparison counts depend on the input
  // permutation and a canonical document must be byte identical for every
  // permutation of the same logical input. The counters stay available in the
  // in-memory decision for diagnostics and complexity assertions.
}

Result<PromotionDecision> get_decision(ByteReader& r) {
  PromotionDecision v;
  v.id = DecisionId::from_value(r.u64());
  auto outcome = get_enum<PromotionOutcome>(r, kOutcomeMax, Reason::enum_invalid);
  if (!outcome.ok()) {
    return outcome.status();
  }
  v.outcome = outcome.value();
  auto kind = get_enum<AuthorityKind>(r, kAuthorityKindMax, Reason::enum_invalid);
  if (!kind.ok()) {
    return kind.status();
  }
  v.authority_kind = kind.value();
  v.incumbent = PathId::from_value(r.u64());
  v.set = AlternateSetId::from_value(r.u64());
  v.set_generation = Generation::from_value(r.u64());
  v.policy_generation = Generation::from_value(r.u64());
  v.obligations_generation = Generation::from_value(r.u64());
  auto basis = get_authority(r);
  if (!basis.ok()) {
    return basis.status();
  }
  v.basis = basis.value();
  v.attempt = AttemptId::from_value(r.u64());
  v.selected = PathId::from_value(r.u64());
  auto ranked = get_count(r, limits::max_assessments_reported);
  if (!ranked.ok()) {
    return ranked.status();
  }
  v.ranked.reserve(ranked.value());
  for (std::size_t i = 0; i < ranked.value(); ++i) {
    auto assessment = get_assessment(r);
    if (!assessment.ok()) {
      return assessment.status();
    }
    v.ranked.push_back(std::move(assessment).value());
  }
  auto indeterminate = get_count(r, limits::max_indeterminate_reported);
  if (!indeterminate.ok()) {
    return indeterminate.status();
  }
  v.indeterminate_above.reserve(indeterminate.value());
  for (std::size_t i = 0; i < indeterminate.value(); ++i) {
    v.indeterminate_above.push_back(PathId::from_value(r.u64()));
  }
  auto explanation = get_count(r, limits::max_explanation_steps);
  if (!explanation.ok()) {
    return explanation.status();
  }
  v.explanation.reserve(explanation.value());
  for (std::size_t i = 0; i < explanation.value(); ++i) {
    const std::uint16_t raw_reason = r.u16();
    const std::int64_t value = r.i64();
    if (!r.ok()) {
      return r.status();
    }
    if (raw_reason > max_reason_value) {
      return Status(Code::invalid, Reason::enum_invalid, "explanation reason out of range");
    }
    v.explanation.emplace_back(static_cast<Reason>(raw_reason), value);
  }
  v.examined = static_cast<std::size_t>(r.u64());
  v.supplied = static_cast<std::size_t>(r.u64());
  v.search_limited = r.boolean();
  v.set_complete = r.boolean();
  const std::uint16_t raw_reason = r.u16();
  if (!r.ok()) {
    return r.status();
  }
  if (raw_reason > max_reason_value) {
    return Status(Code::invalid, Reason::enum_invalid, "decision reason out of range");
  }
  v.reason = static_cast<Reason>(raw_reason);
  auto provenance = get_enum<Provenance>(r, kProvenanceMax, Reason::enum_invalid);
  if (!provenance.ok()) {
    return provenance.status();
  }
  v.provenance = provenance.value();
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

void put(ByteWriter& w, const PromotionRequest& v) {
  w.u64(v.incumbent.value());
  put_enum(w, v.condition);
  w.u64(v.set.value());
  w.u64(v.set_generation.value());
  w.u64(v.policy_generation.value());
  w.u64(v.obligations_generation.value());
  w.u64(v.topology_generation.value());
  w.u64(v.path_authority_generation.value());
  w.u64(v.epoch.value());
  w.u64(v.boot.value());
  w.u64(v.attempt.value());
}

Result<PromotionRequest> get_request(ByteReader& r) {
  PromotionRequest v;
  v.incumbent = PathId::from_value(r.u64());
  auto condition = get_enum<IncumbentCondition>(r, kConditionMax, Reason::enum_invalid);
  if (!condition.ok()) {
    return condition.status();
  }
  v.condition = condition.value();
  v.set = AlternateSetId::from_value(r.u64());
  v.set_generation = Generation::from_value(r.u64());
  v.policy_generation = Generation::from_value(r.u64());
  v.obligations_generation = Generation::from_value(r.u64());
  v.topology_generation = Generation::from_value(r.u64());
  v.path_authority_generation = Generation::from_value(r.u64());
  v.epoch = Epoch::from_value(r.u64());
  v.boot = BootId::from_value(r.u64());
  v.attempt = AttemptId::from_value(r.u64());
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

void put(ByteWriter& w, const AttemptRecord& v) {
  w.u64(v.id.value());
  w.u64(v.epoch.value());
  w.u64(v.boot.value());
  w.u64(v.incumbent.value());
  w.u64(v.selected.value());
  w.u64(v.set.value());
  w.u64(v.set_generation.value());
  put(w, v.basis);
  put_enum(w, v.state);
  w.u16(static_cast<std::uint16_t>(v.terminal_reason));
  w.u64(v.begin_sequence);
  w.u64(v.terminal_sequence);
  w.u32(v.revalidations);
}

Result<AttemptRecord> get_attempt(ByteReader& r) {
  AttemptRecord v;
  v.id = AttemptId::from_value(r.u64());
  v.epoch = Epoch::from_value(r.u64());
  v.boot = BootId::from_value(r.u64());
  v.incumbent = PathId::from_value(r.u64());
  v.selected = PathId::from_value(r.u64());
  v.set = AlternateSetId::from_value(r.u64());
  v.set_generation = Generation::from_value(r.u64());
  auto basis = get_authority(r);
  if (!basis.ok()) {
    return basis.status();
  }
  v.basis = basis.value();
  auto state = get_enum<AttemptState>(r, kAttemptStateMax, Reason::enum_invalid);
  if (!state.ok()) {
    return state.status();
  }
  v.state = state.value();
  const std::uint16_t raw_reason = r.u16();
  if (!r.ok()) {
    return r.status();
  }
  if (raw_reason > max_reason_value) {
    return Status(Code::invalid, Reason::enum_invalid, "attempt reason out of range");
  }
  v.terminal_reason = static_cast<Reason>(raw_reason);
  v.begin_sequence = r.u64();
  v.terminal_sequence = r.u64();
  v.revalidations = r.u32();
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

void put(ByteWriter& w, const FenceRecord& v) {
  w.u64(v.id.value());
  w.u64(v.epoch.value());
  w.u64(v.boot.value());
  w.u64(v.attempt.value());
  w.u64(v.path.value());
  w.u16(static_cast<std::uint16_t>(v.reason));
  put(w, v.at);
  w.u64(v.sequence);
}

Result<FenceRecord> get_fence(ByteReader& r) {
  FenceRecord v;
  v.id = FenceId::from_value(r.u64());
  v.epoch = Epoch::from_value(r.u64());
  v.boot = BootId::from_value(r.u64());
  v.attempt = AttemptId::from_value(r.u64());
  v.path = PathId::from_value(r.u64());
  const std::uint16_t raw_reason = r.u16();
  if (!r.ok()) {
    return r.status();
  }
  if (raw_reason > max_reason_value) {
    return Status(Code::invalid, Reason::enum_invalid, "fence reason out of range");
  }
  v.reason = static_cast<Reason>(raw_reason);
  auto at = get_authority(r);
  if (!at.ok()) {
    return at.status();
  }
  v.at = at.value();
  v.sequence = r.u64();
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

void put(ByteWriter& w, const LineageEntry& v) {
  w.u64(v.decision.value());
  w.u64(v.attempt.value());
  w.u64(v.epoch.value());
  w.u64(v.boot.value());
  put_enum(w, v.outcome);
  w.u64(v.incumbent.value());
  w.u64(v.selected.value());
  w.u64(v.set.value());
  put(w, v.basis);
  w.u16(static_cast<std::uint16_t>(v.reason));
  w.u64(v.sequence);
}

Result<LineageEntry> get_lineage(ByteReader& r) {
  LineageEntry v;
  v.decision = DecisionId::from_value(r.u64());
  v.attempt = AttemptId::from_value(r.u64());
  v.epoch = Epoch::from_value(r.u64());
  v.boot = BootId::from_value(r.u64());
  auto outcome = get_enum<PromotionOutcome>(r, kOutcomeMax, Reason::enum_invalid);
  if (!outcome.ok()) {
    return outcome.status();
  }
  v.outcome = outcome.value();
  v.incumbent = PathId::from_value(r.u64());
  v.selected = PathId::from_value(r.u64());
  v.set = AlternateSetId::from_value(r.u64());
  auto basis = get_authority(r);
  if (!basis.ok()) {
    return basis.status();
  }
  v.basis = basis.value();
  const std::uint16_t raw_reason = r.u16();
  if (!r.ok()) {
    return r.status();
  }
  if (raw_reason > max_reason_value) {
    return Status(Code::invalid, Reason::enum_invalid, "lineage reason out of range");
  }
  v.reason = static_cast<Reason>(raw_reason);
  v.sequence = r.u64();
  if (!r.ok()) {
    return r.status();
  }
  return v;
}

}  // namespace pff::wire
