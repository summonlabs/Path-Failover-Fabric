// Selector suite: determinism, canonical ordering, gate semantics, the
// ordering-versus-authority distinction, proof carrying refusals and a
// differential comparison against the independent reference solver.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "framework.hpp"
#include "pff/selector.hpp"
#include "support.hpp"

using namespace pff;
using pfftest::make_authority;
using pfftest::make_candidate;
using pfftest::make_obligations;
using pfftest::make_policy;
using pfftest::make_scenario;
using pfftest::Rng;
using pfftest::Scenario;

namespace {

Scenario base_scenario(std::size_t count) {
  Scenario scenario;
  scenario.current = make_authority(11, 12, 13, 14, 21, 0x1234);
  scenario.policy = make_policy(13);
  scenario.obligations = make_obligations(14);
  scenario.incumbent = PathId::from_value(1);
  scenario.set.id = AlternateSetId::from_value(5);
  scenario.set.generation = Generation::from_value(6);
  scenario.set.completeness = SetCompleteness::complete;
  for (std::size_t index = 0; index < count; ++index) {
    scenario.set.candidates.push_back(make_candidate(PathId::from_value(100 + index),
                                                     static_cast<std::uint32_t>(index),
                                                     scenario.current));
  }
  return scenario;
}

PromotionDecision solve(const Scenario& scenario, std::uint64_t id = 1) {
  auto result = Selector::evaluate(pfftest::make_input(scenario, id));
  PFF_CHECK(result.ok());
  if (!result.ok()) {
    return PromotionDecision{};
  }
  return result.value();
}

std::vector<PathId> ranked_paths(const PromotionDecision& decision) {
  std::vector<PathId> paths;
  paths.reserve(decision.ranked.size());
  for (const CandidateAssessment& assessment : decision.ranked) {
    paths.push_back(assessment.path);
  }
  return paths;
}

}  // namespace

PFF_TEST(selector, canonical_order_is_permutation_independent) {
  // Deliberately colliding scores: health and cost cancel out exactly, so every
  // tie break level is exercised.
  Scenario scenario = base_scenario(6);
  for (std::size_t index = 0; index < scenario.set.candidates.size(); ++index) {
    AlternateCandidate& candidate = scenario.set.candidates[index];
    candidate.evidence.health_ppm = 900000;
    candidate.evidence.cost_units = 1000;
    candidate.upstream_rank = static_cast<std::uint32_t>(index % 2);
  }

  std::vector<std::size_t> order(scenario.set.candidates.size());
  std::iota(order.begin(), order.end(), 0);
  const PromotionDecision reference = solve(scenario);
  const std::vector<std::byte> reference_bytes = canonical_bytes(reference);
  const std::vector<PathId> reference_order = ranked_paths(reference);

  std::size_t permutations = 0;
  do {
    AlternateSet permuted = scenario.set;
    for (std::size_t index = 0; index < order.size(); ++index) {
      permuted.candidates[index] = scenario.set.candidates[order[index]];
    }
    Scenario copy = scenario;
    copy.set = permuted;
    const PromotionDecision decision = solve(copy);
    PFF_CHECK(ranked_paths(decision) == reference_order);
    PFF_CHECK(canonical_bytes(decision) == reference_bytes);
    PFF_CHECK(decision.selected == reference.selected);
    PFF_CHECK_EQ(decision_digest(decision), decision_digest(reference));
    ++permutations;
    // The diagnostics counters may legitimately differ between permutations,
    // which is exactly why they are excluded from the canonical document.
    PFF_CHECK_EQ(decision.stats.candidates_supplied, reference.stats.candidates_supplied);
  } while (std::next_permutation(order.begin(), order.end()));
  PFF_CHECK_EQ(permutations, std::size_t{720});
}

PFF_TEST(selector, large_permutation_sample_is_stable) {
  Rng rng(0xC0FFEEu);
  for (int trial = 0; trial < 40; ++trial) {
    Scenario scenario = make_scenario(rng, 12, 0);
    const PromotionDecision reference = solve(scenario);
    const std::vector<std::byte> reference_bytes = canonical_bytes(reference);
    for (int shuffle = 0; shuffle < 25; ++shuffle) {
      Scenario copy = scenario;
      rng.shuffle(copy.set.candidates);
      const PromotionDecision decision = solve(copy);
      PFF_CHECK(canonical_bytes(decision) == reference_bytes);
    }
  }
}

PFF_TEST(selector, ordering_is_not_authority) {
  Scenario scenario = base_scenario(3);
  // The best looking member is declared under generations that are no longer
  // current. A selector that promoted the head of the sorted list would promote
  // it; the runtime must not.
  scenario.set.candidates[0].evidence.health_ppm = 999999;
  scenario.set.candidates[0].authority.path_authority = Generation::from_value(11);
  scenario.set.candidates[1].evidence.health_ppm = 900000;
  scenario.set.candidates[2].evidence.health_ppm = 800000;

  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::authorized);
  PFF_CHECK(decision.selected == scenario.set.candidates[0].path ||
            decision.selected != scenario.set.candidates[0].path);
  PFF_CHECK(decision.selected == scenario.set.candidates[1].path);
  PFF_CHECK(decision.ranked[0].path == scenario.set.candidates[0].path);
  PFF_CHECK(decision.ranked[0].verdict == CandidateVerdict::excluded);
  PFF_CHECK(decision.ranked[0].reason == Reason::authority_stale);
  PFF_CHECK(decision.ranked[0].authority == AuthorityClass::stale);
}

PFF_TEST(selector, equal_scores_break_by_rank_then_identity) {
  Scenario scenario = base_scenario(3);
  for (AlternateCandidate& candidate : scenario.set.candidates) {
    candidate.evidence.health_ppm = 900000;
    candidate.evidence.cost_units = 1000;
  }
  scenario.set.candidates[0].upstream_rank = 5;
  scenario.set.candidates[1].upstream_rank = 1;
  scenario.set.candidates[2].upstream_rank = 1;

  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.selected == PathId::from_value(101));
  PFF_CHECK(ranked_paths(decision)[0] == PathId::from_value(101));
  PFF_CHECK(ranked_paths(decision)[1] == PathId::from_value(102));
  PFF_CHECK(ranked_paths(decision)[2] == PathId::from_value(100));
}

PFF_TEST(selector, every_gate_excludes_definitely) {
  struct GateCase {
    const char* name;
    Reason reason;
  };
  const GateCase cases[] = {
      {"withdrawn", Reason::withdrawn},
      {"ineligible", Reason::eligibility_ineligible},
      {"stale_evidence", Reason::evidence_stale},
      {"unreachable", Reason::reachability_unreachable},
      {"insufficient_capacity", Reason::capacity_insufficient},
      {"missing_capability", Reason::capability_missing},
      {"health", Reason::health_below_obligation},
      {"cost", Reason::cost_above_obligation},
  };
  for (const GateCase& gate : cases) {
    Scenario scenario = base_scenario(2);
    AlternateCandidate& broken = scenario.set.candidates[0];
    broken.evidence.health_ppm = 999999;
    broken.upstream_rank = 0;
    const std::string name = gate.name;
    if (name == "withdrawn") {
      broken.withdrawn = true;
    } else if (name == "ineligible") {
      broken.evidence.eligibility = Eligibility::ineligible;
    } else if (name == "stale_evidence") {
      broken.evidence.freshness = Freshness::stale;
    } else if (name == "unreachable") {
      broken.evidence.reachability = Reachability::unreachable;
    } else if (name == "insufficient_capacity") {
      broken.evidence.capacity = CapacityClass::insufficient;
    } else if (name == "missing_capability") {
      broken.evidence.capabilities = 0x1u;
    } else if (name == "health") {
      broken.evidence.health_ppm = 1000;
    } else if (name == "cost") {
      broken.evidence.cost_units = limits::cost_scale;
    }
    const PromotionDecision decision = solve(scenario);
    PFF_CHECK_MSG(decision.outcome == PromotionOutcome::authorized, name);
    const CandidateAssessment* broken_assessment = nullptr;
    for (const CandidateAssessment& assessment : decision.ranked) {
      if (assessment.path == broken.path) {
        broken_assessment = &assessment;
      }
    }
    PFF_CHECK_MSG(broken_assessment != nullptr, name);
    if (broken_assessment != nullptr) {
      PFF_CHECK_MSG(broken_assessment->verdict == CandidateVerdict::excluded, name);
      PFF_CHECK_MSG(broken_assessment->reason == gate.reason, name);
    }
    PFF_CHECK_MSG(decision.selected == PathId::from_value(101), name);
  }
}

PFF_TEST(selector, unknown_member_blocks_unless_policy_allows_skipping) {
  Scenario scenario = base_scenario(2);
  scenario.set.candidates[0].evidence.health_ppm = 999999;
  scenario.set.candidates[0].evidence.eligibility = Eligibility::unknown;
  scenario.set.candidates[0].upstream_rank = 0;

  PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::indeterminate_unknown_member);
  PFF_CHECK(!decision.selected.valid());
  PFF_CHECK_EQ(decision.indeterminate_above.size(), std::size_t{1});

  scenario.policy.allow_unknown_skip = true;
  decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::authorized);
  PFF_CHECK(decision.selected == PathId::from_value(101));
  PFF_CHECK_EQ(decision.indeterminate_above.size(), std::size_t{1});
  PFF_CHECK(decision.indeterminate_above[0] == PathId::from_value(100));
}

PFF_TEST(selector, indeterminate_member_below_the_winner_does_not_block) {
  Scenario scenario = base_scenario(2);
  scenario.set.candidates[0].evidence.health_ppm = 999999;
  scenario.set.candidates[1].evidence.health_ppm = 100000;
  scenario.set.candidates[1].evidence.eligibility = Eligibility::unknown;

  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::authorized);
  PFF_CHECK(decision.selected == PathId::from_value(100));
  PFF_CHECK(decision.indeterminate_above.empty());
}

PFF_TEST(selector, conflicting_generation_member_refuses_the_decision) {
  Scenario scenario = base_scenario(2);
  scenario.set.candidates[0].authority.policy = Generation::from_value(99);
  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::refused_conflict);
  PFF_CHECK(!decision.selected.valid());
}

PFF_TEST(selector, proven_no_alternate_requires_a_complete_surface) {
  Scenario scenario = base_scenario(3);
  for (AlternateCandidate& candidate : scenario.set.candidates) {
    candidate.evidence.health_ppm = 1000;
  }
  PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::refused_proven_no_alternate);
  PFF_CHECK(decision.set_complete);
  PFF_CHECK(!decision.search_limited);
  PFF_CHECK_EQ(decision.examined, std::size_t{3});

  scenario.set.completeness = SetCompleteness::partial;
  decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::indeterminate_incomplete_set);

  scenario.set.completeness = SetCompleteness::unknown;
  decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::indeterminate_incomplete_set);
  PFF_CHECK(decision.reason == Reason::set_completeness_unknown);

  // An empty complete set is a genuine proof: there is nothing to promote and
  // nothing was left unexamined.
  Scenario empty = base_scenario(0);
  decision = solve(empty);
  PFF_CHECK(decision.outcome == PromotionOutcome::refused_proven_no_alternate);
}

PFF_TEST(selector, bounded_search_is_indeterminate_not_absence) {
  Scenario scenario = base_scenario(40);
  for (AlternateCandidate& candidate : scenario.set.candidates) {
    candidate.evidence.health_ppm = 1000;
  }
  scenario.policy.search_limit = 8;
  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::indeterminate_search_limit);
  PFF_CHECK(decision.search_limited);
  PFF_CHECK_EQ(decision.examined, std::size_t{8});
  PFF_CHECK_EQ(decision.supplied, std::size_t{40});

  // A winner inside the examined prefix is still a determinate answer.
  Scenario promoted = base_scenario(40);
  promoted.policy.search_limit = 8;
  for (std::size_t index = 8; index < promoted.set.candidates.size(); ++index) {
    promoted.set.candidates[index].evidence.health_ppm = 1000;
  }
  promoted.set.candidates[3].evidence.health_ppm = 999000;
  const PromotionDecision decision2 = solve(promoted);
  PFF_CHECK(decision2.outcome == PromotionOutcome::authorized);
  PFF_CHECK(decision2.selected == PathId::from_value(103));
  PFF_CHECK(decision2.search_limited);
}

PFF_TEST(selector, stale_surface_is_reported_as_stale_not_as_absence) {
  Scenario scenario = base_scenario(3);
  for (AlternateCandidate& candidate : scenario.set.candidates) {
    candidate.authority.topology = Generation::from_value(10);
  }
  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::refused_stale_authority);
  PFF_CHECK(decision.reason == Reason::authority_stale);
}

PFF_TEST(selector, fenced_path_is_excluded_and_supports_a_proof) {
  Scenario scenario = base_scenario(2);
  scenario.set.candidates[0].evidence.health_ppm = 999999;
  scenario.fences.push_back(scenario.set.candidates[0].path);
  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::authorized);
  PFF_CHECK(decision.selected == PathId::from_value(101));
  PFF_CHECK(decision.ranked[0].reason == Reason::path_fenced);

  scenario.fences.push_back(scenario.set.candidates[1].path);
  const PromotionDecision fenced = solve(scenario);
  PFF_CHECK(fenced.outcome == PromotionOutcome::refused_proven_no_alternate);
}

PFF_TEST(selector, incumbent_candidate_is_never_promoted) {
  Scenario scenario = base_scenario(2);
  scenario.incumbent = PathId::from_value(100);
  scenario.set.candidates[0].evidence.health_ppm = 999999;
  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.outcome == PromotionOutcome::authorized);
  PFF_CHECK(decision.selected == PathId::from_value(101));
  PFF_CHECK(decision.ranked[0].reason == Reason::incumbent_is_self);
}

PFF_TEST(selector, malformed_input_is_refused_explicitly) {
  Scenario scenario = base_scenario(2);
  SelectionInput input = pfftest::make_input(scenario, 1);
  input.set = nullptr;
  auto result = Selector::evaluate(input);
  PFF_CHECK(result.ok());
  PFF_CHECK(result.value().outcome == PromotionOutcome::refused_invalid_input);

  input = pfftest::make_input(scenario, 1);
  scenario.policy.search_limit = 0;
  Scenario broken = scenario;
  input.policy = &broken.policy;
  result = Selector::evaluate(input);
  PFF_CHECK(result.value().outcome == PromotionOutcome::refused_policy_invalid);

  Scenario mismatched = base_scenario(2);
  SelectionInput generation_input = pfftest::make_input(mismatched, 1);
  generation_input.expected_set_generation = Generation::from_value(99);
  result = Selector::evaluate(generation_input);
  PFF_CHECK(result.value().outcome == PromotionOutcome::refused_conflict);

  generation_input.expected_set_generation = Generation::from_value(1);
  result = Selector::evaluate(generation_input);
  PFF_CHECK(result.value().outcome == PromotionOutcome::refused_stale_authority);
}

PFF_TEST(selector, differential_against_reference_solver) {
  std::size_t mismatches = 0;
  for (std::uint64_t seed = 1; seed <= 1200; ++seed) {
    Rng rng(seed);
    const std::size_t count = 1 + static_cast<std::size_t>(rng.below(9));
    const std::uint32_t flags = static_cast<std::uint32_t>(rng.below(8));
    Scenario scenario = make_scenario(rng, count, flags);
    if (rng.chance(20)) {
      scenario.policy.allow_unknown_skip = true;
    }
    if (rng.chance(15)) {
      scenario.policy.search_limit = static_cast<std::uint32_t>(1 + rng.below(4));
    }
    const SelectionInput input = pfftest::make_input(scenario, 99);
    const auto result = Selector::evaluate(input);
    PFF_CHECK(result.ok());
    if (!result.ok()) {
      continue;
    }
    const PromotionDecision& decision = result.value();
    const pfftest::ReferenceOutcome expected = pfftest::reference_solve(input);

    const std::vector<PathId> expected_prefix(expected.order.begin(),
                                             expected.order.begin() +
                                                 static_cast<std::ptrdiff_t>(expected.examined));
    const char* divergence = nullptr;
    if (decision.outcome != expected.outcome) {
      divergence = "outcome";
    } else if (decision.reason != expected.reason) {
      divergence = "reason";
    } else if (decision.selected != expected.selected) {
      divergence = "selected";
    } else if (ranked_paths(decision) != expected_prefix) {
      divergence = "order";
    } else if (decision.search_limited != expected.search_limited) {
      divergence = "search_limited";
    } else if (decision.examined != expected.examined) {
      divergence = "examined";
    } else if (decision.indeterminate_above != expected.indeterminate_above) {
      divergence = "indeterminate_above";
    } else if (decision.ranked.size() != expected.examined) {
      divergence = "ranked size";
    } else {
      for (std::size_t index = 0; index < decision.ranked.size(); ++index) {
        if (decision.ranked[index].verdict != expected.verdicts[index]) {
          divergence = "verdict";
          break;
        }
        if (decision.ranked[index].reason != expected.reasons[index]) {
          divergence = "candidate reason";
          break;
        }
      }
    }
    if (divergence != nullptr) {
      ++mismatches;
      PFF_CHECK_MSG(false, std::string("seed ") + std::to_string(seed) + " " + divergence +
                               " runtime " + to_string(decision.outcome) + "/" +
                               to_string(decision.reason) + " reference " +
                               to_string(expected.outcome) + "/" + to_string(expected.reason));
    }
  }
  PFF_CHECK_EQ(mismatches, std::size_t{0});
}

PFF_TEST(selector, decision_document_is_bounded) {
  Scenario scenario = base_scenario(limits::max_candidates_per_set);
  for (AlternateCandidate& candidate : scenario.set.candidates) {
    candidate.evidence.health_ppm = 1000;
  }
  const PromotionDecision decision = solve(scenario);
  PFF_CHECK(decision.explanation.size() <= limits::max_explanation_steps);
  PFF_CHECK(decision.ranked.size() <= limits::max_candidates_per_set);
  const std::vector<std::byte> bytes = canonical_bytes(decision);
  PFF_CHECK(!bytes.empty());
  PFF_CHECK(bytes.size() <= limits::max_record_payload);
}

PFF_TEST_MAIN()
