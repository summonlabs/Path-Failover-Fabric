// Scale and benchmark suite.
//
// Complexity is asserted with deterministic work counters rather than wall clock
// timing, so a regression cannot hide behind a fast machine. Timing is reported
// as information only.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "framework.hpp"
#include "pff/fabric.hpp"
#include "support.hpp"
#include "tmpdir.hpp"

using namespace pff;
using pfftest::Rng;
using pfftest::Scenario;
using pfftest::TempDir;

namespace {

Scenario scenario_of(std::size_t count) {
  Scenario scenario;
  scenario.current = pfftest::make_authority(11, 12, 13, 14, 21, 0x1234);
  scenario.policy = pfftest::make_policy(13);
  scenario.obligations = pfftest::make_obligations(14);
  scenario.incumbent = PathId::from_value(1);
  scenario.set.id = AlternateSetId::from_value(5);
  scenario.set.generation = Generation::from_value(6);
  scenario.set.completeness = SetCompleteness::complete;
  Rng rng(0x5EEDu + count);
  for (std::size_t index = 0; index < count; ++index) {
    AlternateCandidate candidate = pfftest::make_candidate(
        PathId::from_value(1000 + index), static_cast<std::uint32_t>(index), scenario.current);
    candidate.evidence.health_ppm = static_cast<std::int64_t>(500000 + rng.below(500000));
    candidate.evidence.cost_units = static_cast<std::int64_t>(rng.below(100000));
    candidate.upstream_rank = static_cast<std::uint32_t>(rng.below(64));
    scenario.set.candidates.push_back(candidate);
  }
  return scenario;
}

std::uint64_t millis_since(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                            start)
          .count());
}

}  // namespace

PFF_TEST(scale, selector_work_counters_are_sub_quadratic) {
  std::printf("    candidates   comparisons   gate_evals   score_evals   digest_ms\n");
  std::uint64_t previous_comparisons = 0;
  std::size_t previous_count = 0;
  for (const std::size_t count : {std::size_t{64}, std::size_t{256}, std::size_t{1024},
                                  std::size_t{4096}}) {
    const Scenario scenario = scenario_of(count);
    const auto start = std::chrono::steady_clock::now();
    const auto result = Selector::evaluate(pfftest::make_input(scenario, 1));
    const std::uint64_t elapsed = millis_since(start);
    PFF_REQUIRE(result.ok());
    const PromotionDecision& decision = result.value();
    PFF_CHECK_EQ(decision.stats.candidates_supplied, static_cast<std::uint64_t>(count));
    PFF_CHECK_EQ(decision.stats.score_evaluations, static_cast<std::uint64_t>(count));

    // A bottom-up merge sort performs at most n * ceil(log2 n) comparisons.
    std::size_t bound = 1;
    while (bound < count) {
      bound *= 2;
    }
    const auto levels = static_cast<std::uint64_t>(std::log2(static_cast<double>(bound)));
    PFF_CHECK(decision.stats.ordering_comparisons <=
              static_cast<std::uint64_t>(count) * (levels + 1));
    // Gates are evaluated at most once per candidate per gate.
    PFF_CHECK(decision.stats.gate_evaluations <= 16u * static_cast<std::uint64_t>(count));
    PFF_CHECK(decision.stats.fence_lookups <= static_cast<std::uint64_t>(count));

    std::printf("    %10zu   %11llu   %10llu   %11llu   %9llu\n", count,
                static_cast<unsigned long long>(decision.stats.ordering_comparisons),
                static_cast<unsigned long long>(decision.stats.gate_evaluations),
                static_cast<unsigned long long>(decision.stats.score_evaluations),
                static_cast<unsigned long long>(elapsed));

    if (previous_count != 0) {
      const double size_ratio =
          static_cast<double>(count) / static_cast<double>(previous_count);
      const double work_ratio = previous_comparisons == 0
                                    ? 1.0
                                    : static_cast<double>(decision.stats.ordering_comparisons) /
                                          static_cast<double>(previous_comparisons);
      // n log n grows far more slowly than n squared.
      PFF_CHECK(work_ratio < size_ratio * size_ratio);
      PFF_CHECK(work_ratio < size_ratio * 2.0);
    }
    previous_comparisons = decision.stats.ordering_comparisons;
    previous_count = count;
  }
}

PFF_TEST(scale, fabric_promotion_throughput_and_bounded_state) {
  TempDir dir("scale-fabric");
  FabricOptions options;
  options.directory = dir.path();
  options.provenance = Provenance::synthetic;
  options.max_retained_attempts = 64;
  options.compaction_journal_bytes = 1u << 20;
  auto fabric = Fabric::open(options);
  PFF_REQUIRE(fabric.ok());

  const AuthorityVector before = fabric.value()->authority();
  AuthorityVector advance;
  advance.topology = Generation::from_value(before.topology.value() + 1);
  advance.path_authority = Generation::from_value(before.path_authority.value() + 1);
  PFF_REQUIRE(fabric.value()->set_authority(advance).ok());
  PFF_REQUIRE(fabric.value()->apply_policy(pfftest::make_policy(before.policy.value() + 1)).ok());
  PFF_REQUIRE(fabric.value()
                  ->apply_obligations(pfftest::make_obligations(before.obligations.value() + 1))
                  .ok());

  const AlternateSetId set_id = AlternateSetId::from_value(21);
  const std::size_t members = 256;
  const AuthorityVector current = fabric.value()->authority();
  AlternateSet set;
  set.id = set_id;
  set.generation = Generation::from_value(1);
  set.completeness = SetCompleteness::complete;
  for (std::size_t index = 0; index < members; ++index) {
    AlternateCandidate candidate =
        pfftest::make_candidate(PathId::from_value(2000 + index),
                                static_cast<std::uint32_t>(index), current);
    candidate.evidence.health_ppm = static_cast<std::int64_t>(900000 - (index % 1000));
    set.candidates.push_back(candidate);
  }
  PFF_REQUIRE(fabric.value()->apply_alternate_set(set).ok());
  for (AlternateCandidate candidate : set.candidates) {
    const auto stored = fabric.value()->evidence(candidate.path);
    candidate.evidence.observation_seq = stored.ok() ? stored.value().observation_seq + 1 : 1;
    PFF_REQUIRE(fabric.value()->apply_evidence(candidate.evidence).ok());
  }

  constexpr int kPromotions = 200;
  std::uint64_t authorized = 0;
  std::uint64_t committed = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < kPromotions; ++iteration) {
    auto authorization = fabric.value()->begin_promotion(
        PathId::from_value(1), IncumbentCondition::failed, set_id);
    if (!authorization.ok() || !authorization.value().authorized()) {
      continue;
    }
    ++authorized;
    EffectEvidence effect;
    effect.id = EvidenceId::from_value(400000 + static_cast<std::uint64_t>(iteration));
    effect.path = authorization.value().grant.path;
    const auto stored = fabric.value()->evidence(effect.path);
    effect.observation_seq = (stored.ok() ? stored.value().observation_seq : 0) + 1;
    effect.effect_verified = true;
    effect.observed_under = fabric.value()->authority();
    if (fabric.value()->record_effect(authorization.value().grant.attempt, effect).ok()) {
      ++committed;
    }
    // An operator re-supplies the surface, which advances the generations.
    const AuthorityVector before_supply = fabric.value()->authority();
    AuthorityVector next;
    next.topology = Generation::from_value(before_supply.topology.value() + 1);
    next.path_authority = Generation::from_value(before_supply.path_authority.value() + 1);
    if (!fabric.value()->set_authority(next).ok()) {
      break;
    }
    const auto existing = fabric.value()->alternate_set(set_id);
    AlternateSet refreshed = set;
    refreshed.generation = Generation::from_value(existing.value().generation.value() + 1);
    const AuthorityVector now = fabric.value()->authority();
    for (AlternateCandidate& candidate : refreshed.candidates) {
      candidate.authority = now;
      candidate.evidence.observed_under = now;
    }
    if (!fabric.value()->apply_alternate_set(refreshed).ok()) {
      break;
    }
    for (AlternateCandidate candidate : refreshed.candidates) {
      const auto known = fabric.value()->evidence(candidate.path);
      candidate.evidence.observation_seq = known.ok() ? known.value().observation_seq + 1 : 1;
      if (!fabric.value()->apply_evidence(candidate.evidence).ok()) {
        break;
      }
    }
  }
  const std::uint64_t elapsed = millis_since(start);
  std::printf("    promotions=%d authorized=%llu committed=%llu elapsed_ms=%llu\n", kPromotions,
              static_cast<unsigned long long>(authorized),
              static_cast<unsigned long long>(committed),
              static_cast<unsigned long long>(elapsed));
  PFF_CHECK(authorized >= static_cast<std::uint64_t>(kPromotions) / 2);
  PFF_CHECK_EQ(committed, authorized);

  // Retained state stays bounded no matter how much work was completed.
  PFF_CHECK(fabric.value()->attempts().size() <= 64);
  PFF_CHECK(fabric.value()->lineage(limits::max_lineage).size() <= limits::max_lineage);
  PFF_CHECK(fabric.value()->fences().size() <= limits::max_fences);
  PFF_CHECK(fabric.value()->active_attempt_count() == 0);
  const FabricStats stats = fabric.value()->stats();
  PFF_CHECK(stats.promotions_committed == committed);
  PFF_CHECK(stats.journal_records >= committed);
  PFF_CHECK(stats.attempts_dropped <= stats.promotions_committed + stats.promotions_refused);
}

PFF_TEST(scale, decision_documents_stay_bounded_at_the_candidate_limit) {
  const Scenario scenario = scenario_of(limits::max_candidates_per_set);
  const auto start = std::chrono::steady_clock::now();
  const auto result = Selector::evaluate(pfftest::make_input(scenario, 1));
  const std::uint64_t elapsed = millis_since(start);
  PFF_REQUIRE(result.ok());
  const std::vector<std::byte> bytes = canonical_bytes(result.value());
  PFF_CHECK(!bytes.empty());
  PFF_CHECK(bytes.size() <= limits::max_record_payload);
  PFF_CHECK(result.value().explanation.size() <= limits::max_explanation_steps);
  std::printf("    limit=%zu document_bytes=%zu elapsed_ms=%llu\n",
              limits::max_candidates_per_set, bytes.size(),
              static_cast<unsigned long long>(elapsed));
}

PFF_TEST_MAIN()
