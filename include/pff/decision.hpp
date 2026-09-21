// Path Failover Fabric - decisions, assessments and explanations.
//
// A decision is a total, canonical, reproducible document: the same inputs always
// produce the same bytes. It states the exact subject, the generations that made
// it legal, what it authorises (if anything), and why every other member lost.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pff/authority.hpp"
#include "pff/ids.hpp"
#include "pff/model.hpp"
#include "pff/status.hpp"

namespace pff {

// The externally visible outcome of an evaluation or promotion attempt.
enum class PromotionOutcome : std::uint8_t {
  // Positive authority: a grant exists and the caller may apply the transition.
  authorized = 0,
  // The transition was applied and a verified effect was recorded.
  promoted = 1,
  // Proof-carrying refusal: the whole supplied legal surface was examined, no
  // member was indeterminate, and nothing qualified.
  refused_proven_no_alternate = 2,
  refused_invalid_input = 3,
  refused_policy_invalid = 4,
  refused_obligations_invalid = 5,
  refused_stale_authority = 6,
  refused_conflict = 7,
  refused_fenced = 8,
  refused_resource_exhausted = 9,
  refused_read_only = 10,
  refused_already_owned = 11,
  refused_superseded_attempt = 12,
  // Indeterminate: the fabric cannot say whether an alternate exists.
  indeterminate_incomplete_set = 13,
  indeterminate_unknown_member = 14,
  indeterminate_search_limit = 15,
  unsupported = 16,
  // An explicit post-commit reversion to a previously displaced path completed.
  reverted = 17,
  // A pre-commit undo completed after fresh validation of the incumbent.
  rolled_back = 18,
};

const char* to_string(PromotionOutcome o) noexcept;
[[nodiscard]] constexpr bool is_positive_authority(PromotionOutcome o) noexcept {
  return o == PromotionOutcome::authorized || o == PromotionOutcome::promoted;
}
[[nodiscard]] constexpr bool is_refusal(PromotionOutcome o) noexcept {
  return o == PromotionOutcome::refused_proven_no_alternate ||
         o == PromotionOutcome::refused_invalid_input ||
         o == PromotionOutcome::refused_policy_invalid ||
         o == PromotionOutcome::refused_obligations_invalid ||
         o == PromotionOutcome::refused_stale_authority ||
         o == PromotionOutcome::refused_conflict ||
         o == PromotionOutcome::refused_fenced ||
         o == PromotionOutcome::refused_resource_exhausted ||
         o == PromotionOutcome::refused_read_only ||
         o == PromotionOutcome::refused_already_owned ||
         o == PromotionOutcome::refused_superseded_attempt;
}
// True when the transition has been applied (not merely authorised).
[[nodiscard]] constexpr bool is_transition_applied(PromotionOutcome o) noexcept {
  return o == PromotionOutcome::promoted || o == PromotionOutcome::reverted;
}
[[nodiscard]] constexpr bool is_indeterminate(PromotionOutcome o) noexcept {
  return o == PromotionOutcome::indeterminate_incomplete_set ||
         o == PromotionOutcome::indeterminate_unknown_member ||
         o == PromotionOutcome::indeterminate_search_limit;
}

// Which epistemic category an externally visible document belongs to. These are
// distinct on purpose: an acknowledgement is not a verified effect, an
// eligibility statement is not a grant, and a recommendation is not authority.
enum class AuthorityKind : std::uint8_t {
  observation = 0,
  eligibility = 1,
  recommendation = 2,
  acknowledgement = 3,
  grant = 4,
  verified_effect = 5,
};

const char* to_string(AuthorityKind k) noexcept;

enum class CandidateVerdict : std::uint8_t {
  selected = 0,
  promotable_not_selected = 1,
  excluded = 2,       // definite: proven not promotable
  indeterminate = 3,  // blocked: promotability could not be established
};

const char* to_string(CandidateVerdict v) noexcept;

struct CandidateAssessment {
  PathId path;
  std::uint32_t upstream_rank = 0;
  ScoreTerms terms;
  CandidateVerdict verdict = CandidateVerdict::excluded;
  Reason reason = Reason::none;
  AuthorityClass authority = AuthorityClass::unknown;

  friend bool operator==(const CandidateAssessment& a, const CandidateAssessment& b) noexcept {
    return a.path == b.path && a.upstream_rank == b.upstream_rank &&
           a.terms.total == b.terms.total && a.verdict == b.verdict && a.reason == b.reason &&
           a.authority == b.authority;
  }
};

// Deterministic work counters. They are part of the decision document so that
// complexity can be asserted without depending on wall-clock timing.
struct SelectionStats {
  std::uint64_t candidates_supplied = 0;
  std::uint64_t candidates_considered = 0;
  std::uint64_t score_evaluations = 0;
  std::uint64_t ordering_comparisons = 0;
  std::uint64_t gate_evaluations = 0;
  std::uint64_t definite_exclusions = 0;
  std::uint64_t indeterminate_members = 0;
  std::uint64_t fence_lookups = 0;

  friend bool operator==(const SelectionStats& a, const SelectionStats& b) noexcept {
    return a.candidates_supplied == b.candidates_supplied &&
           a.candidates_considered == b.candidates_considered &&
           a.score_evaluations == b.score_evaluations &&
           a.ordering_comparisons == b.ordering_comparisons &&
           a.gate_evaluations == b.gate_evaluations &&
           a.definite_exclusions == b.definite_exclusions &&
           a.indeterminate_members == b.indeterminate_members &&
           a.fence_lookups == b.fence_lookups;
  }
};

struct PromotionDecision {
  DecisionId id;
  PromotionOutcome outcome = PromotionOutcome::unsupported;
  AuthorityKind authority_kind = AuthorityKind::recommendation;

  PathId incumbent;
  AlternateSetId set;
  Generation set_generation;
  Generation policy_generation;
  Generation obligations_generation;
  AuthorityVector basis;
  AttemptId attempt;

  PathId selected;  // absent when nothing may be promoted

  std::vector<CandidateAssessment> ranked;
  std::vector<PathId> indeterminate_above;
  std::vector<std::pair<Reason, std::int64_t>> explanation;

  std::size_t examined = 0;
  std::size_t supplied = 0;
  bool search_limited = false;
  bool set_complete = false;
  Reason reason = Reason::none;
  Provenance provenance = Provenance::synthetic;
  SelectionStats stats;

  [[nodiscard]] bool selected_something() const noexcept { return selected.valid(); }
};

// Canonical, deterministic serialization of a decision. Byte-identical inputs
// produce byte-identical output regardless of container, hash or discovery order.
[[nodiscard]] std::vector<std::byte> canonical_bytes(const PromotionDecision& decision);

// Integrity digest of the canonical encoding (FNV-1a 64 over the canonical bytes).
[[nodiscard]] std::uint64_t decision_digest(const PromotionDecision& decision) noexcept;

[[nodiscard]] std::string describe(const PromotionDecision& decision);

}  // namespace pff
