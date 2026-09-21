// Shared test support: deterministic pseudo random generation, synthetic fabric
// fixtures and an independent reference implementation of the selection
// specification.
//
// The reference solver is deliberately written in a different style from the
// runtime: it never sorts, it selects by repeated maximum scan, and it derives
// verdicts from an explicit ordered gate list. Agreement between the two is
// evidence that the runtime implements the specification rather than merely
// agreeing with itself.
//
// Every fixture produced here is SYNTHETIC. No physical fabric is involved.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "pff/decision.hpp"
#include "pff/model.hpp"
#include "pff/selector.hpp"

namespace pfftest {

// ---- deterministic pseudo random numbers ----------------------------------
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

  std::int64_t range(std::int64_t low, std::int64_t high) {
    if (high <= low) {
      return low;
    }
    return low + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(high - low + 1)));
  }

  bool chance(unsigned percent) { return below(100) < percent; }

  template <typename T>
  void shuffle(std::vector<T>& values) {
    for (std::size_t i = values.size(); i > 1; --i) {
      const std::size_t j = static_cast<std::size_t>(below(i));
      std::swap(values[i - 1], values[j]);
    }
  }

 private:
  std::uint64_t state_;
};

// ---- synthetic fixture construction ---------------------------------------
inline pff::AuthorityVector make_authority(std::uint64_t topology, std::uint64_t path_authority,
                                           std::uint64_t policy, std::uint64_t obligations,
                                           std::uint64_t epoch, std::uint64_t boot) {
  pff::AuthorityVector authority;
  authority.topology = pff::Generation::from_value(topology);
  authority.path_authority = pff::Generation::from_value(path_authority);
  authority.policy = pff::Generation::from_value(policy);
  authority.obligations = pff::Generation::from_value(obligations);
  authority.epoch = pff::Epoch::from_value(epoch);
  authority.boot = pff::BootId::from_value(boot);
  return authority;
}

inline pff::FailoverPolicy make_policy(std::uint64_t generation) {
  pff::FailoverPolicy policy;
  policy.generation = pff::Generation::from_value(generation);
  policy.w_health = 3;
  policy.w_cost = 1;
  policy.w_rank = 1;
  policy.w_capability = 1;
  policy.min_score = 0;
  policy.search_limit = pff::limits::default_search_limit;
  policy.max_retained_attempts = 64;
  policy.max_revalidations = 4;
  policy.allow_unknown_skip = false;
  policy.require_verified_effect = true;
  return policy;
}

inline pff::ServiceObligations make_obligations(std::uint64_t generation) {
  pff::ServiceObligations obligations;
  obligations.generation = pff::Generation::from_value(generation);
  obligations.required_capabilities = 0x3u;
  obligations.min_health_ppm = 500000;
  obligations.max_cost_units = 5000000;
  obligations.require_reachable = true;
  obligations.require_capacity = true;
  obligations.require_fresh_evidence = true;
  return obligations;
}

inline pff::AlternateCandidate make_candidate(pff::PathId path, std::uint32_t rank,
                                              const pff::AuthorityVector& authority) {
  pff::AlternateCandidate candidate;
  candidate.path = path;
  candidate.upstream_rank = rank;
  candidate.authority = authority;
  candidate.withdrawn = false;
  candidate.evidence.id = pff::EvidenceId::from_value(path.value());
  candidate.evidence.path = path;
  candidate.evidence.freshness = pff::Freshness::fresh;
  candidate.evidence.eligibility = pff::Eligibility::eligible;
  candidate.evidence.reachability = pff::Reachability::reachable;
  candidate.evidence.capacity = pff::CapacityClass::sufficient;
  candidate.evidence.capabilities = 0x3u;
  candidate.evidence.health_ppm = 900000;
  candidate.evidence.cost_units = 1000;
  candidate.evidence.observation_seq = 1;
  candidate.evidence.observed_under = authority;
  candidate.evidence.provenance = pff::Provenance::synthetic;
  return candidate;
}

struct Scenario {
  pff::AuthorityVector current;
  pff::FailoverPolicy policy;
  pff::ServiceObligations obligations;
  pff::AlternateSet set;
  pff::PathId incumbent;
  std::vector<pff::PathId> fences;
};

// Builds a randomised but always valid scenario. The generator can produce
// equal scores, stale and unknown generations, withdrawn members, unknown
// eligibility, fenced paths and incomplete sets.
inline Scenario make_scenario(Rng& rng, std::size_t candidate_count, std::uint32_t set_flags) {
  const bool stale_members = (set_flags & 1u) != 0;
  const bool unknown_members = (set_flags & 2u) != 0;
  const bool incomplete = (set_flags & 4u) != 0;

  Scenario scenario;
  scenario.current = make_authority(11, 12, 13, 14, 21, 0x1234);
  scenario.policy = make_policy(13);
  scenario.obligations = make_obligations(14);
  scenario.incumbent = pff::PathId::from_value(1);
  scenario.set.id = pff::AlternateSetId::from_value(5);
  scenario.set.generation = pff::Generation::from_value(6);
  scenario.set.completeness =
      incomplete ? pff::SetCompleteness::partial : pff::SetCompleteness::complete;

  for (std::size_t index = 0; index < candidate_count; ++index) {
    const pff::PathId path = pff::PathId::from_value(100 + index);
    pff::AlternateCandidate candidate =
        make_candidate(path, static_cast<std::uint32_t>(index), scenario.current);
    if (stale_members && rng.chance(25)) {
      candidate.authority.path_authority = pff::Generation::from_value(11);
    }
    if (unknown_members && rng.chance(15)) {
      candidate.authority.policy = pff::Generation::absent();
    }
    // Deliberately collide scores so that tie breaking is exercised.
    candidate.evidence.health_ppm = rng.chance(50) ? 900000 : 800000;
    candidate.evidence.cost_units = rng.chance(50) ? 1000 : 2000;
    candidate.upstream_rank = static_cast<std::uint32_t>(rng.below(3));
    if (rng.chance(8)) {
      candidate.withdrawn = true;
    }
    if (rng.chance(8)) {
      candidate.evidence.eligibility = pff::Eligibility::unknown;
    }
    if (rng.chance(6)) {
      candidate.evidence.eligibility = pff::Eligibility::ineligible;
    }
    if (rng.chance(6)) {
      candidate.evidence.freshness = pff::Freshness::stale;
    }
    if (rng.chance(6)) {
      candidate.evidence.reachability = pff::Reachability::unreachable;
    }
    if (rng.chance(6)) {
      candidate.evidence.capacity = pff::CapacityClass::insufficient;
    }
    if (rng.chance(6)) {
      candidate.evidence.capabilities = 0x1u;
    }
    if (rng.chance(6)) {
      candidate.evidence.health_ppm = 10;
    }
    if (rng.chance(6)) {
      candidate.evidence.cost_units = pff::limits::cost_scale;
    }
    scenario.set.candidates.push_back(candidate);
  }

  if (rng.chance(20) && !scenario.set.candidates.empty()) {
    scenario.fences.push_back(scenario.set.candidates[rng.below(scenario.set.candidates.size())].path);
  }
  return scenario;
}

inline pff::SelectionInput make_input(const Scenario& scenario, std::uint64_t decision_id) {
  pff::SelectionInput input;
  input.incumbent = scenario.incumbent;
  input.set = &scenario.set;
  input.policy = &scenario.policy;
  input.obligations = &scenario.obligations;
  input.current = scenario.current;
  input.expected_set_generation = scenario.set.generation;
  input.fenced_paths = std::span<const pff::PathId>(scenario.fences.data(), scenario.fences.size());
  input.decision_id = pff::DecisionId::from_value(decision_id);
  input.provenance = pff::Provenance::synthetic;
  return input;
}

// ---- independent reference solver ------------------------------------------
struct ReferenceOutcome {
  pff::PromotionOutcome outcome = pff::PromotionOutcome::unsupported;
  pff::Reason reason = pff::Reason::none;
  pff::PathId selected;
  std::vector<pff::PathId> order;
  std::vector<pff::CandidateVerdict> verdicts;
  std::vector<pff::Reason> reasons;
  std::vector<pff::PathId> indeterminate_above;
  bool search_limited = false;
  std::size_t examined = 0;
};

inline bool reference_key_less(std::int64_t score_a, std::uint32_t rank_a, pff::PathId path_a,
                               std::int64_t score_b, std::uint32_t rank_b, pff::PathId path_b) {
  if (score_a != score_b) {
    return score_a > score_b;
  }
  if (rank_a != rank_b) {
    return rank_a < rank_b;
  }
  return path_a < path_b;
}

inline ReferenceOutcome reference_solve(const pff::SelectionInput& input) {
  ReferenceOutcome out;
  const pff::AlternateSet& set = *input.set;
  const pff::FailoverPolicy& policy = *input.policy;
  const pff::ServiceObligations& obligations = *input.obligations;

  if (!policy.validate().ok()) {
    out.outcome = pff::PromotionOutcome::refused_policy_invalid;
    out.reason = pff::Reason::policy_invalid;
    return out;
  }
  if (!obligations.validate().ok()) {
    out.outcome = pff::PromotionOutcome::refused_obligations_invalid;
    out.reason = pff::Reason::obligations_invalid;
    return out;
  }
  const pff::Status set_status = set.validate();
  if (!set_status.ok()) {
    out.outcome = pff::PromotionOutcome::refused_invalid_input;
    out.reason = set_status.reason;
    return out;
  }
  const auto mismatch = [](pff::Generation supplied, pff::Generation current)
      -> pff::PromotionOutcome {
    const pff::AuthorityClass cls = pff::classify_generation(supplied, current);
    if (cls == pff::AuthorityClass::current) {
      return pff::PromotionOutcome::authorized;
    }
    return cls == pff::AuthorityClass::conflict ? pff::PromotionOutcome::refused_conflict
                                                : pff::PromotionOutcome::refused_stale_authority;
  };
  if (mismatch(policy.generation, input.current.policy) != pff::PromotionOutcome::authorized) {
    out.outcome = mismatch(policy.generation, input.current.policy);
    out.reason = pff::Reason::policy_generation_stale;
    return out;
  }
  if (mismatch(obligations.generation, input.current.obligations) !=
      pff::PromotionOutcome::authorized) {
    out.outcome = mismatch(obligations.generation, input.current.obligations);
    out.reason = pff::Reason::obligations_generation_stale;
    return out;
  }
  if (input.expected_set_generation.valid() &&
      input.expected_set_generation != set.generation) {
    out.outcome = input.expected_set_generation > set.generation
                      ? pff::PromotionOutcome::refused_conflict
                      : pff::PromotionOutcome::refused_stale_authority;
    out.reason = out.outcome == pff::PromotionOutcome::refused_conflict
                     ? pff::Reason::generation_conflict
                     : pff::Reason::set_generation_mismatch;
    return out;
  }

  struct Entry {
    const pff::AlternateCandidate* candidate;
    std::int64_t score;
    bool score_ok;
    pff::AuthorityClass authority;
  };
  std::vector<Entry> entries;
  entries.reserve(set.candidates.size());
  for (const pff::AlternateCandidate& candidate : set.candidates) {
    Entry entry;
    entry.candidate = &candidate;
    const auto terms = pff::compute_score(policy, obligations, candidate.evidence,
                                          candidate.upstream_rank);
    entry.score_ok = terms.ok();
    entry.score = terms.ok() ? terms.value().total : 0;
    entry.authority = pff::classify(candidate.authority, input.current).overall;
    entries.push_back(entry);
  }

  // Repeated maximum scan: no sorting anywhere in the reference implementation.
  std::vector<bool> taken(entries.size(), false);
  for (std::size_t position = 0; position < entries.size(); ++position) {
    std::size_t best = entries.size();
    for (std::size_t index = 0; index < entries.size(); ++index) {
      if (taken[index]) {
        continue;
      }
      if (best == entries.size() ||
          reference_key_less(entries[index].score, entries[index].candidate->upstream_rank,
                             entries[index].candidate->path, entries[best].score,
                             entries[best].candidate->upstream_rank, entries[best].candidate->path)) {
        best = index;
      }
    }
    taken[best] = true;
    out.order.push_back(entries[best].candidate->path);
  }

  const std::size_t limit =
      std::min(entries.size(), static_cast<std::size_t>(policy.search_limit));
  out.examined = limit;
  out.search_limited = entries.size() > limit;

  bool fatal = false;
  std::size_t first_promotable = entries.size();
  std::size_t first_indeterminate = entries.size();
  std::size_t indeterminate_count = 0;
  std::size_t stale_count = 0;

  for (std::size_t position = 0; position < limit; ++position) {
    const pff::PathId path = out.order[position];
    const Entry* entry = nullptr;
    for (const Entry& candidate : entries) {
      if (candidate.candidate->path == path) {
        entry = &candidate;
        break;
      }
    }
    const pff::AlternateCandidate& candidate = *entry->candidate;
    const bool fenced =
        std::find(input.fenced_paths.begin(), input.fenced_paths.end(), path) !=
        input.fenced_paths.end();

    pff::CandidateVerdict verdict = pff::CandidateVerdict::promotable_not_selected;
    pff::Reason reason = pff::Reason::promotable_not_selected;
    if (candidate.path == input.incumbent) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::incumbent_is_self;
    } else if (candidate.withdrawn) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::withdrawn;
    } else if (fenced) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::path_fenced;
    } else if (entry->authority == pff::AuthorityClass::conflict) {
      fatal = true;
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::authority_conflict;
    } else if (entry->authority == pff::AuthorityClass::stale) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::authority_stale;
    } else if (entry->authority == pff::AuthorityClass::unknown) {
      verdict = pff::CandidateVerdict::indeterminate;
      reason = pff::Reason::authority_unknown;
    } else if (!entry->score_ok) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::score_overflow;
    } else if (candidate.evidence.eligibility == pff::Eligibility::ineligible) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::eligibility_ineligible;
    } else if (candidate.evidence.eligibility == pff::Eligibility::unknown) {
      verdict = pff::CandidateVerdict::indeterminate;
      reason = pff::Reason::eligibility_unknown;
    } else if (obligations.require_fresh_evidence &&
               candidate.evidence.freshness == pff::Freshness::stale) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::evidence_stale;
    } else if (obligations.require_fresh_evidence &&
               candidate.evidence.freshness == pff::Freshness::unknown) {
      verdict = pff::CandidateVerdict::indeterminate;
      reason = pff::Reason::evidence_unknown;
    } else if (obligations.require_reachable &&
               candidate.evidence.reachability == pff::Reachability::unreachable) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::reachability_unreachable;
    } else if (obligations.require_reachable &&
               candidate.evidence.reachability == pff::Reachability::unknown) {
      verdict = pff::CandidateVerdict::indeterminate;
      reason = pff::Reason::reachability_unknown;
    } else if (obligations.require_capacity &&
               candidate.evidence.capacity == pff::CapacityClass::insufficient) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::capacity_insufficient;
    } else if (obligations.require_capacity &&
               candidate.evidence.capacity == pff::CapacityClass::unknown) {
      verdict = pff::CandidateVerdict::indeterminate;
      reason = pff::Reason::capacity_unknown;
    } else if ((candidate.evidence.capabilities & obligations.required_capabilities) !=
               obligations.required_capabilities) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::capability_missing;
    } else if (candidate.evidence.health_ppm < obligations.min_health_ppm) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::health_below_obligation;
    } else if (candidate.evidence.cost_units > obligations.max_cost_units) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::cost_above_obligation;
    } else if (entry->score < policy.min_score) {
      verdict = pff::CandidateVerdict::excluded;
      reason = pff::Reason::score_below_policy;
    }

    if (verdict == pff::CandidateVerdict::promotable_not_selected &&
        first_promotable == entries.size()) {
      first_promotable = position;
    }
    if (verdict == pff::CandidateVerdict::indeterminate) {
      if (first_indeterminate == entries.size()) {
        first_indeterminate = position;
      }
      ++indeterminate_count;
    }
    if (verdict == pff::CandidateVerdict::excluded && reason == pff::Reason::authority_stale) {
      ++stale_count;
    }
    out.verdicts.push_back(verdict);
    out.reasons.push_back(reason);
  }

  if (fatal) {
    out.outcome = pff::PromotionOutcome::refused_conflict;
    out.reason = pff::Reason::generation_conflict;
    return out;
  }
  if (first_promotable != entries.size()) {
    if (first_indeterminate < first_promotable && !policy.allow_unknown_skip) {
      for (std::size_t position = 0; position < first_promotable; ++position) {
        if (out.verdicts[position] == pff::CandidateVerdict::indeterminate) {
          out.indeterminate_above.push_back(out.order[position]);
        }
      }
      out.outcome = pff::PromotionOutcome::indeterminate_unknown_member;
      out.reason = pff::Reason::eligibility_unknown;
      return out;
    }
    for (std::size_t position = 0; position < first_promotable; ++position) {
      if (out.verdicts[position] == pff::CandidateVerdict::indeterminate) {
        out.indeterminate_above.push_back(out.order[position]);
      }
    }
    out.selected = out.order[first_promotable];
    out.verdicts[first_promotable] = pff::CandidateVerdict::selected;
    out.reasons[first_promotable] = pff::Reason::selected;
    out.outcome = pff::PromotionOutcome::authorized;
    out.reason = pff::Reason::selected;
    return out;
  }
  if (indeterminate_count > 0) {
    out.outcome = pff::PromotionOutcome::indeterminate_unknown_member;
    out.reason = pff::Reason::eligibility_unknown;
    return out;
  }
  if (out.search_limited) {
    out.outcome = pff::PromotionOutcome::indeterminate_search_limit;
    out.reason = pff::Reason::search_limit_reached;
    return out;
  }
  if (set.completeness != pff::SetCompleteness::complete) {
    out.outcome = pff::PromotionOutcome::indeterminate_incomplete_set;
    out.reason = set.completeness == pff::SetCompleteness::partial
                     ? pff::Reason::set_incomplete
                     : pff::Reason::set_completeness_unknown;
    return out;
  }
  if (stale_count > 0) {
    out.outcome = pff::PromotionOutcome::refused_stale_authority;
    out.reason = pff::Reason::authority_stale;
    return out;
  }
  out.outcome = pff::PromotionOutcome::refused_proven_no_alternate;
  out.reason = pff::Reason::proven_no_alternate;
  return out;
}

}  // namespace pfftest
