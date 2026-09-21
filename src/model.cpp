// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/model.hpp"

#include <bit>

#include "pff/checked.hpp"

namespace pff {

const char* to_string(Provenance p) noexcept {
  switch (p) {
    case Provenance::real:
      return "REAL";
    case Provenance::synthetic:
      return "SYNTHETIC";
    case Provenance::unsupported:
      return "UNSUPPORTED";
  }
  return "UNSUPPORTED";
}

Provenance weakest(Provenance a, Provenance b) noexcept {
  return static_cast<std::uint8_t>(a) >= static_cast<std::uint8_t>(b) ? a : b;
}

const char* to_string(Freshness v) noexcept {
  switch (v) {
    case Freshness::fresh:
      return "FRESH";
    case Freshness::stale:
      return "STALE";
    case Freshness::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* to_string(Eligibility v) noexcept {
  switch (v) {
    case Eligibility::eligible:
      return "ELIGIBLE";
    case Eligibility::ineligible:
      return "INELIGIBLE";
    case Eligibility::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* to_string(Reachability v) noexcept {
  switch (v) {
    case Reachability::reachable:
      return "REACHABLE";
    case Reachability::unreachable:
      return "UNREACHABLE";
    case Reachability::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* to_string(CapacityClass v) noexcept {
  switch (v) {
    case CapacityClass::sufficient:
      return "SUFFICIENT";
    case CapacityClass::insufficient:
      return "INSUFFICIENT";
    case CapacityClass::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* to_string(SetCompleteness c) noexcept {
  switch (c) {
    case SetCompleteness::complete:
      return "COMPLETE";
    case SetCompleteness::partial:
      return "PARTIAL";
    case SetCompleteness::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* to_string(IncumbentCondition c) noexcept {
  switch (c) {
    case IncumbentCondition::failed:
      return "FAILED";
    case IncumbentCondition::invalidated:
      return "INVALIDATED";
    case IncumbentCondition::degraded:
      return "DEGRADED";
  }
  return "INVALID";
}

Status PathEvidence::validate() const {
  if (!path.valid()) {
    return Status(Code::invalid, Reason::domain_invalid, "evidence path identity absent");
  }
  if (!in_range(health_ppm, 0, limits::health_scale)) {
    return Status(Code::invalid, Reason::domain_invalid, "health_ppm out of range");
  }
  if (!in_range(cost_units, 0, limits::cost_scale)) {
    return Status(Code::invalid, Reason::domain_invalid, "cost_units out of range");
  }
  return ok_status();
}

Status EffectEvidence::validate() const {
  if (!path.valid()) {
    return Status(Code::invalid, Reason::domain_invalid, "effect path identity absent");
  }
  if (!effect_verified) {
    return Status(Code::invalid, Reason::effect_unverified,
                  "effect evidence must be verified by an external observer");
  }
  return ok_status();
}

Status ServiceObligations::validate() const {
  if (!generation.valid()) {
    return Status(Code::invalid, Reason::obligations_invalid, "obligations generation absent");
  }
  if (!in_range(min_health_ppm, 0, limits::health_scale)) {
    return Status(Code::invalid, Reason::obligations_invalid, "min_health_ppm out of range");
  }
  if (!in_range(max_cost_units, 0, limits::cost_scale)) {
    return Status(Code::invalid, Reason::obligations_invalid, "max_cost_units out of range");
  }
  return ok_status();
}

Status FailoverPolicy::validate() const {
  if (!generation.valid()) {
    return Status(Code::invalid, Reason::policy_invalid, "policy generation absent");
  }
  const std::int64_t weights[4] = {w_health, w_cost, w_rank, w_capability};
  for (const std::int64_t weight : weights) {
    if (weight < -limits::max_weight || weight > limits::max_weight) {
      return Status(Code::invalid, Reason::policy_invalid, "policy weight out of range");
    }
  }
  if (search_limit == 0 || search_limit > limits::max_search_limit) {
    return Status(Code::invalid, Reason::policy_invalid, "search_limit out of range");
  }
  if (max_retained_attempts == 0 || max_retained_attempts > limits::max_attempts) {
    return Status(Code::invalid, Reason::policy_invalid, "max_retained_attempts out of range");
  }
  if (max_revalidations > 1024) {
    return Status(Code::invalid, Reason::policy_invalid, "max_revalidations out of range");
  }
  if (min_score < -(INT64_MAX / 4) || min_score > (INT64_MAX / 4)) {
    return Status(Code::invalid, Reason::policy_invalid, "min_score out of range");
  }
  return ok_status();
}

Result<ScoreTerms> compute_score(const FailoverPolicy& policy, const ServiceObligations& obligations,
                                 const PathEvidence& evidence, std::uint32_t upstream_rank) noexcept {
  ScoreTerms terms;
  const std::int64_t health = clamp_i64(evidence.health_ppm, 0, limits::health_scale);
  const std::int64_t cost = clamp_i64(evidence.cost_units, 0, limits::cost_scale);
  const std::int64_t rank =
      clamp_i64(static_cast<std::int64_t>(upstream_rank), 0, limits::rank_scale);
  const auto satisfied = std::popcount(evidence.capabilities & obligations.required_capabilities);
  const auto capability = static_cast<std::int64_t>(satisfied);

  // Cost and rank are "smaller is better", so the objective rewards the
  // complement against the fixed scale. Every term is therefore non-negative for
  // non-negative weights, and the objective is maximised.
  if (!checked_mul(policy.w_health, health, terms.health) ||
      !checked_mul(policy.w_cost, limits::cost_scale - cost, terms.cost) ||
      !checked_mul(policy.w_rank, limits::rank_scale - rank, terms.rank) ||
      !checked_mul(policy.w_capability, capability, terms.capability)) {
    return Status(Code::invalid, Reason::score_overflow, "objective term overflow");
  }

  std::int64_t total = 0;
  if (!checked_add(terms.health, terms.cost, total) ||
      !checked_add(total, terms.rank, total) ||
      !checked_add(total, terms.capability, total)) {
    return Status(Code::invalid, Reason::score_overflow, "objective total overflow");
  }
  terms.total = total;
  return terms;
}

Status AlternateCandidate::validate() const {
  if (!path.valid()) {
    return Status(Code::invalid, Reason::domain_invalid, "candidate path identity absent");
  }
  if (upstream_rank > static_cast<std::uint32_t>(limits::rank_scale)) {
    return Status(Code::invalid, Reason::rank_out_of_range, "upstream rank out of range");
  }
  if (evidence.path != path) {
    return Status(Code::invalid, Reason::domain_invalid,
                  "candidate evidence does not belong to the candidate path");
  }
  return evidence.validate();
}

Status AlternateSet::validate() const {
  if (!id.valid()) {
    return Status(Code::invalid, Reason::domain_invalid, "alternate set identity absent");
  }
  if (!generation.valid()) {
    return Status(Code::invalid, Reason::domain_invalid, "alternate set generation absent");
  }
  if (candidates.size() > limits::max_candidates_per_set) {
    return Status(Code::invalid, Reason::candidates_too_many, "alternate set exceeds bound");
  }
  for (const AlternateCandidate& candidate : candidates) {
    const Status status = candidate.validate();
    if (!status.ok()) {
      return status;
    }
  }
  // Duplicate members are contradictory rather than merely redundant: the fabric
  // cannot tell which of two entries is authoritative, so it refuses the set.
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    for (std::size_t j = i + 1; j < candidates.size(); ++j) {
      if (candidates[i].path == candidates[j].path) {
        return Status(Code::invalid, Reason::duplicate_path, "duplicate candidate path");
      }
    }
  }
  return ok_status();
}

AuthorityVector PromotionRequest::basis() const noexcept {
  AuthorityVector out;
  out.topology = topology_generation;
  out.path_authority = path_authority_generation;
  out.policy = policy_generation;
  out.obligations = obligations_generation;
  out.epoch = epoch;
  out.boot = boot;
  return out;
}

}  // namespace pff
