// Path Failover Fabric - the supplied inputs of a promotion decision.
//
// The fabric owns promotion of an already-supplied, already-legality-governed
// alternate path set. It does not discover paths. Everything in this header is an
// observation, a definition or an eligibility statement supplied from outside;
// none of it is authority by itself.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "pff/authority.hpp"
#include "pff/ids.hpp"
#include "pff/limits.hpp"
#include "pff/status.hpp"

namespace pff {

// Where a datum came from. The fabric labels every decision with the weakest
// provenance present in its basis so that synthetic fixtures can never be
// mistaken for physical fabric observation.
enum class Provenance : std::uint8_t {
  real = 0,
  synthetic = 1,
  unsupported = 2,
};

const char* to_string(Provenance p) noexcept;
Provenance weakest(Provenance a, Provenance b) noexcept;

// ---- tri-state observations ------------------------------------------------
enum class Freshness : std::uint8_t { fresh = 0, stale = 1, unknown = 2 };
enum class Eligibility : std::uint8_t { eligible = 0, ineligible = 1, unknown = 2 };
enum class Reachability : std::uint8_t { reachable = 0, unreachable = 1, unknown = 2 };
enum class CapacityClass : std::uint8_t { sufficient = 0, insufficient = 1, unknown = 2 };

const char* to_string(Freshness v) noexcept;
const char* to_string(Eligibility v) noexcept;
const char* to_string(Reachability v) noexcept;
const char* to_string(CapacityClass v) noexcept;

using CapabilityMask = std::uint32_t;

// A pure observation about a path. It is explicitly not authority: a FRESH
// observation does not authorise anything, and an observation recorded under
// generation G is only meaningful while generation G is current.
struct PathEvidence {
  EvidenceId id;
  PathId path;
  Freshness freshness = Freshness::unknown;
  Eligibility eligibility = Eligibility::unknown;
  Reachability reachability = Reachability::unknown;
  CapacityClass capacity = CapacityClass::unknown;
  CapabilityMask capabilities = 0;
  std::int64_t health_ppm = 0;
  std::int64_t cost_units = 0;
  std::uint64_t observation_seq = 0;
  AuthorityVector observed_under;
  Provenance provenance = Provenance::synthetic;

  [[nodiscard]] Status validate() const;
};

// Evidence that an authorized promotion actually took effect. Acknowledgement of
// a request is not verified effect; this record is accepted only when an
// external observer reports the effect under the current authority vector.
struct EffectEvidence {
  EvidenceId id;
  PathId path;
  std::uint64_t observation_seq = 0;
  bool effect_verified = false;
  AuthorityVector observed_under;
  Provenance provenance = Provenance::synthetic;

  [[nodiscard]] Status validate() const;
};

// ---- service obligations ---------------------------------------------------
struct ServiceObligations {
  Generation generation;
  CapabilityMask required_capabilities = 0;
  std::int64_t min_health_ppm = 0;
  std::int64_t max_cost_units = limits::cost_scale;
  bool require_reachable = true;
  bool require_capacity = true;
  bool require_fresh_evidence = true;

  [[nodiscard]] Status validate() const;

  friend bool operator==(const ServiceObligations& a, const ServiceObligations& b) noexcept {
    return a.generation == b.generation && a.required_capabilities == b.required_capabilities &&
           a.min_health_ppm == b.min_health_ppm && a.max_cost_units == b.max_cost_units &&
           a.require_reachable == b.require_reachable &&
           a.require_capacity == b.require_capacity &&
           a.require_fresh_evidence == b.require_fresh_evidence;
  }
};

// ---- promotion policy ------------------------------------------------------
struct FailoverPolicy {
  Generation generation;
  std::int64_t w_health = 1;
  std::int64_t w_cost = 1;
  std::int64_t w_rank = 1;
  std::int64_t w_capability = 1;
  std::int64_t min_score = 0;
  std::uint32_t search_limit = limits::default_search_limit;
  std::uint32_t max_retained_attempts = 256;
  std::uint32_t max_revalidations = 4;
  // Fail-closed default: a member whose eligibility cannot be established blocks
  // the decision instead of being silently skipped.
  bool allow_unknown_skip = false;
  bool require_verified_effect = true;

  [[nodiscard]] Status validate() const;

  friend bool operator==(const FailoverPolicy& a, const FailoverPolicy& b) noexcept {
    return a.generation == b.generation && a.w_health == b.w_health && a.w_cost == b.w_cost &&
           a.w_rank == b.w_rank && a.w_capability == b.w_capability &&
           a.min_score == b.min_score && a.search_limit == b.search_limit &&
           a.max_retained_attempts == b.max_retained_attempts &&
           a.max_revalidations == b.max_revalidations &&
           a.allow_unknown_skip == b.allow_unknown_skip &&
           a.require_verified_effect == b.require_verified_effect;
  }
};

// The four objective terms, kept separately so that explanations and
// differential tests can compare term by term rather than only in aggregate.
struct ScoreTerms {
  std::int64_t health = 0;
  std::int64_t cost = 0;
  std::int64_t rank = 0;
  std::int64_t capability = 0;
  std::int64_t total = 0;

  [[nodiscard]] std::array<std::int64_t, 4> as_array() const noexcept {
    return {health, cost, rank, capability};
  }
};

// Deterministic total objective. Checked arithmetic throughout; a term that
// would overflow is reported as score_overflow instead of wrapping.
[[nodiscard]] Result<ScoreTerms> compute_score(const FailoverPolicy& policy,
                                               const ServiceObligations& obligations,
                                               const PathEvidence& evidence,
                                               std::uint32_t upstream_rank) noexcept;

// ---- supplied alternate set ------------------------------------------------
enum class SetCompleteness : std::uint8_t {
  complete = 0,   // upstream asserts this is the whole legal alternate surface
  partial = 1,    // upstream knows members are missing
  unknown = 2,    // upstream cannot say
};

const char* to_string(SetCompleteness c) noexcept;

struct AlternateCandidate {
  PathId path;
  // Ordering hint supplied by the upstream path authority. It participates in
  // the canonical order and is never treated as authority to promote.
  std::uint32_t upstream_rank = 0;
  bool withdrawn = false;
  PathEvidence evidence;
  // The generations under which this member was declared a legal alternate.
  AuthorityVector authority;

  [[nodiscard]] Status validate() const;
};

struct AlternateSet {
  AlternateSetId id;
  Generation generation;
  SetCompleteness completeness = SetCompleteness::unknown;
  std::vector<AlternateCandidate> candidates;

  [[nodiscard]] Status validate() const;
};

// Why the incumbent is being replaced. The fabric never promotes an alternate
// without a stated condition for the incumbent.
enum class IncumbentCondition : std::uint8_t {
  failed = 0,
  invalidated = 1,
  degraded = 2,
};

const char* to_string(IncumbentCondition c) noexcept;

struct PromotionRequest {
  PathId incumbent;
  IncumbentCondition condition = IncumbentCondition::failed;
  AlternateSetId set;
  Generation set_generation;
  Generation policy_generation;
  Generation obligations_generation;
  Generation topology_generation;
  Generation path_authority_generation;
  Epoch epoch;
  BootId boot;
  AttemptId attempt;

  [[nodiscard]] AuthorityVector basis() const noexcept;
};

}  // namespace pff
