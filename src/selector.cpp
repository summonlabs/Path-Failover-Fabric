// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/selector.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

#include "pff/checked.hpp"

namespace pff {
namespace {

struct RankedEntry {
  CandidateAssessment assessment;
  AuthorityComparison comparison;
  std::size_t candidate_index = 0;
  bool score_ok = true;
};

struct GateOutcome {
  CandidateVerdict verdict = CandidateVerdict::excluded;
  Reason reason = Reason::none;
};

bool contains(std::span<const PathId> paths, PathId path) noexcept {
  for (const PathId candidate : paths) {
    if (candidate == path) {
      return true;
    }
  }
  return false;
}

// Deterministic bottom-up merge sort. std::sort would also be correct because the
// order is total, but its comparison count is implementation defined; a fixed
// algorithm keeps the work counters reproducible across toolchains and standard
// libraries, which is what the complexity assertions rely on.
template <typename T, typename Less>
void deterministic_sort(std::vector<T>& data, std::vector<T>& scratch, Less less,
                        std::uint64_t& comparisons) {
  const std::size_t n = data.size();
  if (n < 2) {
    return;
  }
  scratch.resize(n);
  for (std::size_t width = 1; width < n; width *= 2) {
    for (std::size_t start = 0; start < n; start += 2 * width) {
      const std::size_t mid = std::min(start + width, n);
      const std::size_t end = std::min(start + 2 * width, n);
      std::size_t i = start;
      std::size_t j = mid;
      std::size_t k = start;
      while (i < mid && j < end) {
        ++comparisons;
        if (less(data[j], data[i])) {
          scratch[k++] = data[j++];
        } else {
          scratch[k++] = data[i++];
        }
      }
      while (i < mid) {
        scratch[k++] = data[i++];
      }
      while (j < end) {
        scratch[k++] = data[j++];
      }
      for (std::size_t index = start; index < end; ++index) {
        data[index] = scratch[index];
      }
    }
  }
}

GateOutcome apply_gates(const AlternateCandidate& candidate, const RankedEntry& entry,
                        const FailoverPolicy& policy, const ServiceObligations& obligations,
                        std::span<const PathId> fenced_paths, PathId incumbent,
                        SelectionStats& stats) {
  GateOutcome out;

  ++stats.gate_evaluations;
  if (candidate.path == incumbent) {
    out.reason = Reason::incumbent_is_self;
    return out;
  }

  ++stats.gate_evaluations;
  if (candidate.withdrawn) {
    out.reason = Reason::withdrawn;
    return out;
  }

  ++stats.fence_lookups;
  ++stats.gate_evaluations;
  if (contains(fenced_paths, candidate.path)) {
    out.reason = Reason::path_fenced;
    return out;
  }

  // Authority is checked before any quality gate: a candidate the current
  // generations do not cover cannot be promoted no matter how good it looks.
  ++stats.gate_evaluations;
  if (entry.comparison.overall == AuthorityClass::stale) {
    out.reason = Reason::authority_stale;
    return out;
  }
  ++stats.gate_evaluations;
  if (entry.comparison.overall == AuthorityClass::unknown) {
    out.verdict = CandidateVerdict::indeterminate;
    out.reason = Reason::authority_unknown;
    return out;
  }

  ++stats.gate_evaluations;
  if (!entry.score_ok) {
    out.reason = Reason::score_overflow;
    return out;
  }

  ++stats.gate_evaluations;
  switch (candidate.evidence.eligibility) {
    case Eligibility::eligible:
      break;
    case Eligibility::ineligible:
      out.reason = Reason::eligibility_ineligible;
      return out;
    case Eligibility::unknown:
      out.verdict = CandidateVerdict::indeterminate;
      out.reason = Reason::eligibility_unknown;
      return out;
  }

  if (obligations.require_fresh_evidence) {
    ++stats.gate_evaluations;
    switch (candidate.evidence.freshness) {
      case Freshness::fresh:
        break;
      case Freshness::stale:
        out.reason = Reason::evidence_stale;
        return out;
      case Freshness::unknown:
        out.verdict = CandidateVerdict::indeterminate;
        out.reason = Reason::evidence_unknown;
        return out;
    }
  }

  if (obligations.require_reachable) {
    ++stats.gate_evaluations;
    switch (candidate.evidence.reachability) {
      case Reachability::reachable:
        break;
      case Reachability::unreachable:
        out.reason = Reason::reachability_unreachable;
        return out;
      case Reachability::unknown:
        out.verdict = CandidateVerdict::indeterminate;
        out.reason = Reason::reachability_unknown;
        return out;
    }
  }

  if (obligations.require_capacity) {
    ++stats.gate_evaluations;
    switch (candidate.evidence.capacity) {
      case CapacityClass::sufficient:
        break;
      case CapacityClass::insufficient:
        out.reason = Reason::capacity_insufficient;
        return out;
      case CapacityClass::unknown:
        out.verdict = CandidateVerdict::indeterminate;
        out.reason = Reason::capacity_unknown;
        return out;
    }
  }

  ++stats.gate_evaluations;
  if ((candidate.evidence.capabilities & obligations.required_capabilities) !=
      obligations.required_capabilities) {
    out.reason = Reason::capability_missing;
    return out;
  }

  ++stats.gate_evaluations;
  if (candidate.evidence.health_ppm < obligations.min_health_ppm) {
    out.reason = Reason::health_below_obligation;
    return out;
  }

  ++stats.gate_evaluations;
  if (candidate.evidence.cost_units > obligations.max_cost_units) {
    out.reason = Reason::cost_above_obligation;
    return out;
  }

  ++stats.gate_evaluations;
  if (entry.assessment.terms.total < policy.min_score) {
    out.reason = Reason::score_below_policy;
    return out;
  }

  out.verdict = CandidateVerdict::promotable_not_selected;
  out.reason = Reason::promotable_not_selected;
  return out;
}

void explain(PromotionDecision& decision, Reason reason, std::int64_t value) {
  if (decision.explanation.size() < limits::max_explanation_steps) {
    decision.explanation.emplace_back(reason, value);
  }
}

PromotionDecision refuse(PromotionDecision decision, PromotionOutcome outcome, Reason reason,
                         std::int64_t detail) {
  decision.outcome = outcome;
  decision.reason = reason;
  decision.selected = PathId::absent();
  explain(decision, reason, detail);
  return decision;
}

}  // namespace

bool Selector::canonical_less(const CandidateAssessment& a, const CandidateAssessment& b) noexcept {
  // Score descending, then upstream rank ascending, then path identity
  // ascending. Path identity is unique inside a validated set, so this is a total
  // order and the sorted sequence is unique.
  if (a.terms.total != b.terms.total) {
    return a.terms.total > b.terms.total;
  }
  if (a.upstream_rank != b.upstream_rank) {
    return a.upstream_rank < b.upstream_rank;
  }
  return a.path < b.path;
}

Result<PromotionDecision> Selector::evaluate(const SelectionInput& input) {
  PromotionDecision decision;
  decision.id = input.decision_id;
  decision.incumbent = input.incumbent;
  decision.basis = input.current;
  decision.attempt = input.attempt;
  decision.provenance = input.provenance;
  decision.authority_kind = AuthorityKind::recommendation;
  decision.outcome = PromotionOutcome::unsupported;
  decision.reason = Reason::none;

  if (input.set == nullptr || input.policy == nullptr || input.obligations == nullptr) {
    return refuse(decision, PromotionOutcome::refused_invalid_input, Reason::invalid_payload, 0);
  }

  const AlternateSet& set = *input.set;
  const FailoverPolicy& policy = *input.policy;
  const ServiceObligations& obligations = *input.obligations;

  decision.set = set.id;
  decision.set_generation = set.generation;
  decision.policy_generation = policy.generation;
  decision.obligations_generation = obligations.generation;
  decision.supplied = set.candidates.size();
  decision.stats.candidates_supplied = set.candidates.size();
  decision.set_complete = set.completeness == SetCompleteness::complete;

  if (!policy.validate().ok()) {
    return refuse(decision, PromotionOutcome::refused_policy_invalid, Reason::policy_invalid, 0);
  }
  if (!obligations.validate().ok()) {
    return refuse(decision, PromotionOutcome::refused_obligations_invalid,
                  Reason::obligations_invalid, 0);
  }
  const Status set_status = set.validate();
  if (!set_status.ok()) {
    return refuse(decision, PromotionOutcome::refused_invalid_input, set_status.reason, 0);
  }
  if (!input.incumbent.valid()) {
    return refuse(decision, PromotionOutcome::refused_invalid_input, Reason::domain_invalid, 0);
  }

  // A decision is legal only against the generations the caller actually holds.
  // A mismatch is STALE (behind), CONFLICT (ahead, so caller and runtime
  // disagree about reality) or UNKNOWN (absent).
  const AuthorityClass policy_class = classify_generation(policy.generation, input.current.policy);
  if (policy_class != AuthorityClass::current) {
    const bool conflict = policy_class == AuthorityClass::conflict;
    const Reason reason = policy_class == AuthorityClass::stale
                              ? Reason::policy_generation_stale
                              : (conflict ? Reason::generation_conflict : Reason::authority_unknown);
    return refuse(decision,
                  conflict ? PromotionOutcome::refused_conflict
                           : PromotionOutcome::refused_stale_authority,
                  reason, 0);
  }
  const AuthorityClass obligations_class =
      classify_generation(obligations.generation, input.current.obligations);
  if (obligations_class != AuthorityClass::current) {
    const bool conflict = obligations_class == AuthorityClass::conflict;
    const Reason reason = obligations_class == AuthorityClass::stale
                              ? Reason::obligations_generation_stale
                              : (conflict ? Reason::generation_conflict : Reason::authority_unknown);
    return refuse(decision,
                  conflict ? PromotionOutcome::refused_conflict
                           : PromotionOutcome::refused_stale_authority,
                  reason, 0);
  }
  if (input.expected_set_generation.valid() &&
      input.expected_set_generation != set.generation) {
    // The caller believes it is deciding about a different set generation. If it
    // is behind, its input is stale; if it is ahead, caller and runtime disagree
    // about which generation exists at all, which is a conflict.
    const bool conflict = input.expected_set_generation > set.generation;
    return refuse(decision,
                  conflict ? PromotionOutcome::refused_conflict
                           : PromotionOutcome::refused_stale_authority,
                  conflict ? Reason::generation_conflict : Reason::set_generation_mismatch, 0);
  }

  std::vector<RankedEntry> entries;
  entries.reserve(set.candidates.size());
  for (std::size_t index = 0; index < set.candidates.size(); ++index) {
    const AlternateCandidate& candidate = set.candidates[index];
    RankedEntry entry;
    entry.candidate_index = index;
    entry.assessment.path = candidate.path;
    entry.assessment.upstream_rank = candidate.upstream_rank;
    const Result<ScoreTerms> terms =
        compute_score(policy, obligations, candidate.evidence, candidate.upstream_rank);
    ++decision.stats.score_evaluations;
    if (terms.ok()) {
      entry.assessment.terms = terms.value();
    } else {
      entry.score_ok = false;
    }
    entry.comparison = classify(candidate.authority, input.current);
    entries.push_back(entry);
  }

  std::vector<RankedEntry> scratch;
  deterministic_sort(
      entries, scratch,
      [](const RankedEntry& a, const RankedEntry& b) {
        return Selector::canonical_less(a.assessment, b.assessment);
      },
      decision.stats.ordering_comparisons);

  // The scan is bounded by policy. A bounded scan that finds nothing is an
  // indeterminate result, never a proof that no alternate exists.
  const std::size_t supplied = entries.size();
  const auto declared_limit = static_cast<std::size_t>(policy.search_limit);
  const std::size_t limit = std::min(supplied, declared_limit);
  decision.examined = limit;
  decision.search_limited = supplied > limit;
  decision.stats.candidates_considered = limit;

  std::size_t first_promotable = std::numeric_limits<std::size_t>::max();
  std::size_t first_indeterminate = std::numeric_limits<std::size_t>::max();
  std::size_t indeterminate_count = 0;
  std::size_t stale_exclusions = 0;
  std::size_t quality_exclusions = 0;
  bool fatal_conflict = false;

  decision.ranked.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    RankedEntry& entry = entries[i];
    const AlternateCandidate& candidate = set.candidates[entry.candidate_index];
    if (entry.comparison.overall == AuthorityClass::conflict) {
      fatal_conflict = true;
    }
    const GateOutcome outcome =
        apply_gates(candidate, entry, policy, obligations, input.fenced_paths, input.incumbent,
                    decision.stats);
    entry.assessment.verdict = outcome.verdict;
    entry.assessment.reason = outcome.reason;
    entry.assessment.authority = entry.comparison.overall;
    if (outcome.verdict == CandidateVerdict::promotable_not_selected &&
        first_promotable == std::numeric_limits<std::size_t>::max()) {
      first_promotable = i;
    }
    if (outcome.verdict == CandidateVerdict::indeterminate) {
      if (first_indeterminate == std::numeric_limits<std::size_t>::max()) {
        first_indeterminate = i;
      }
      ++indeterminate_count;
    } else if (outcome.verdict == CandidateVerdict::excluded) {
      if (outcome.reason == Reason::authority_stale) {
        ++stale_exclusions;
      } else {
        ++quality_exclusions;
      }
    }
    decision.ranked.push_back(entry.assessment);
  }

  for (const CandidateAssessment& assessment : decision.ranked) {
    if (assessment.verdict == CandidateVerdict::excluded) {
      ++decision.stats.definite_exclusions;
      explain(decision, assessment.reason, static_cast<std::int64_t>(assessment.path.value()));
    }
  }
  decision.stats.indeterminate_members = indeterminate_count;

  if (fatal_conflict) {
    return refuse(decision, PromotionOutcome::refused_conflict, Reason::generation_conflict,
                  static_cast<std::int64_t>(indeterminate_count));
  }

  if (first_promotable != std::numeric_limits<std::size_t>::max()) {
    if (first_indeterminate < first_promotable && !policy.allow_unknown_skip) {
      for (std::size_t i = 0; i < first_promotable && i < decision.ranked.size(); ++i) {
        if (decision.ranked[i].verdict == CandidateVerdict::indeterminate &&
            decision.indeterminate_above.size() < limits::max_indeterminate_reported) {
          decision.indeterminate_above.push_back(decision.ranked[i].path);
        }
      }
      return refuse(decision, PromotionOutcome::indeterminate_unknown_member,
                    Reason::eligibility_unknown, static_cast<std::int64_t>(indeterminate_count));
    }
    for (std::size_t i = 0; i < first_promotable; ++i) {
      if (decision.ranked[i].verdict == CandidateVerdict::indeterminate &&
          decision.indeterminate_above.size() < limits::max_indeterminate_reported) {
        decision.indeterminate_above.push_back(decision.ranked[i].path);
      }
    }
    decision.ranked[first_promotable].verdict = CandidateVerdict::selected;
    decision.ranked[first_promotable].reason = Reason::selected;
    decision.selected = decision.ranked[first_promotable].path;
    decision.outcome = PromotionOutcome::authorized;
    decision.reason = Reason::selected;
    return decision;
  }

  if (indeterminate_count > 0) {
    return refuse(decision, PromotionOutcome::indeterminate_unknown_member,
                  Reason::eligibility_unknown, static_cast<std::int64_t>(indeterminate_count));
  }
  if (decision.search_limited) {
    return refuse(decision, PromotionOutcome::indeterminate_search_limit,
                  Reason::search_limit_reached, static_cast<std::int64_t>(supplied));
  }
  if (set.completeness != SetCompleteness::complete) {
    return refuse(decision, PromotionOutcome::indeterminate_incomplete_set,
                  set.completeness == SetCompleteness::partial ? Reason::set_incomplete
                                                               : Reason::set_completeness_unknown,
                  static_cast<std::int64_t>(supplied));
  }
  if (stale_exclusions > 0) {
    // At least one member was excluded because the generations it was declared
    // under are no longer current. That is a statement about the supplied input,
    // not a proof about the candidate surface: had the set been re-supplied that
    // member might have been promotable, so no proof is claimed.
    (void)quality_exclusions;
    return refuse(decision, PromotionOutcome::refused_stale_authority, Reason::authority_stale,
                  static_cast<std::int64_t>(stale_exclusions));
  }
  return refuse(decision, PromotionOutcome::refused_proven_no_alternate,
                Reason::proven_no_alternate, static_cast<std::int64_t>(supplied));
}

}  // namespace pff
