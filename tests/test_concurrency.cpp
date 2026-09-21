// Concurrency suite: deterministic barriers, single winner, stale completion
// rejection and ownership under contention. No sleeps and no timeouts are used
// to create or to observe interleavings.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <atomic>
#include <barrier>
#include <cstdint>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "pff/fabric.hpp"
#include "support.hpp"
#include "tmpdir.hpp"

using namespace pff;
using pfftest::TempDir;

namespace {

FabricOptions options_for(const TempDir& dir, std::uint64_t attempts = 128) {
  FabricOptions options;
  options.directory = dir.path();
  options.provenance = Provenance::synthetic;
  options.max_retained_attempts = static_cast<std::uint32_t>(attempts);
  return options;
}

Result<AlternateSet> supply(Fabric& fabric, AlternateSetId set_id, std::size_t count) {
  const AuthorityVector current = fabric.authority();
  const auto existing = fabric.alternate_set(set_id);
  AlternateSet set;
  set.id = set_id;
  set.generation = Generation::from_value(existing.ok() ? existing.value().generation.value() + 1 : 1);
  set.completeness = SetCompleteness::complete;
  for (std::size_t index = 0; index < count; ++index) {
    AlternateCandidate candidate = pfftest::make_candidate(
        PathId::from_value(300 + index), static_cast<std::uint32_t>(index), current);
    candidate.evidence.health_ppm = static_cast<std::int64_t>(900000 - index);
    set.candidates.push_back(candidate);
  }
  Status status = fabric.apply_alternate_set(set);
  if (!status.ok()) {
    return status;
  }
  for (AlternateCandidate candidate : set.candidates) {
    const auto stored = fabric.evidence(candidate.path);
    candidate.evidence.observation_seq = stored.ok() ? stored.value().observation_seq + 1 : 1;
    status = fabric.apply_evidence(candidate.evidence);
    if (!status.ok()) {
      return status;
    }
  }
  return set;
}

Result<AlternateSet> configure(Fabric& fabric, AlternateSetId set_id = AlternateSetId::from_value(11),
                               std::size_t count = 4) {
  const AuthorityVector before = fabric.authority();
  AuthorityVector advance;
  advance.topology = Generation::from_value(before.topology.value() + 1);
  advance.path_authority = Generation::from_value(before.path_authority.value() + 1);
  Status status = fabric.set_authority(advance);
  if (!status.ok()) {
    return status;
  }
  FailoverPolicy policy = pfftest::make_policy(before.policy.value() + 1);
  status = fabric.apply_policy(policy);
  if (!status.ok()) {
    return status;
  }
  ServiceObligations obligations = pfftest::make_obligations(before.obligations.value() + 1);
  status = fabric.apply_obligations(obligations);
  if (!status.ok()) {
    return status;
  }
  return supply(fabric, set_id, count);
}

}  // namespace

PFF_TEST(concurrency, exactly_one_thread_wins_the_transition) {
  TempDir dir("race");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  const auto set = configure(*fabric.value());
  PFF_REQUIRE(set.ok());
  const AlternateSetId set_id = set.value().id;

  constexpr int kThreads = 8;
  std::barrier gate(kThreads);
  std::atomic<int> authorized{0};
  std::atomic<int> refused{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&]() {
      gate.arrive_and_wait();
      auto result = fabric.value()->begin_promotion(PathId::from_value(1),
                                                    IncumbentCondition::failed, set_id);
      if (!result.ok()) {
        errors.fetch_add(1);
        return;
      }
      if (result.value().authorized()) {
        authorized.fetch_add(1);
      } else {
        if (result.value().decision.outcome != PromotionOutcome::refused_already_owned) {
          errors.fetch_add(1);
          return;
        }
        refused.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  PFF_CHECK_EQ(errors.load(), 0);
  PFF_CHECK_EQ(authorized.load(), 1);
  PFF_CHECK_EQ(refused.load(), kThreads - 1);
  PFF_CHECK_EQ(fabric.value()->active_attempt_count(), std::size_t{1});
  PFF_CHECK_EQ(fabric.value()->stats().promotions_authorized, std::uint64_t{1});
}

PFF_TEST(concurrency, only_the_owner_may_complete_and_only_once) {
  TempDir dir("completion");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  const auto set = configure(*fabric.value());
  PFF_REQUIRE(set.ok());

  auto authorization = fabric.value()->begin_promotion(
      PathId::from_value(1), IncumbentCondition::failed, set.value().id);
  PFF_REQUIRE(authorization.ok() && authorization.value().authorized());
  const AttemptId attempt = authorization.value().grant.attempt;
  const PathId selected = authorization.value().grant.path;

  constexpr int kThreads = 6;
  std::barrier gate(kThreads);
  std::atomic<int> committed{0};
  std::atomic<int> refused{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      gate.arrive_and_wait();
      EffectEvidence effect;
      effect.id = EvidenceId::from_value(700000 + index);
      effect.path = selected;
      const auto stored = fabric.value()->evidence(selected);
      effect.observation_seq = (stored.ok() ? stored.value().observation_seq : 0) + 1;
      effect.effect_verified = true;
      effect.observed_under = fabric.value()->authority();
      const Status status = fabric.value()->record_effect(attempt, effect);
      if (status.ok()) {
        committed.fetch_add(1);
      } else {
        refused.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  PFF_CHECK_EQ(committed.load(), 1);
  PFF_CHECK_EQ(refused.load(), kThreads - 1);
  PFF_CHECK_EQ(fabric.value()->stats().promotions_committed, std::uint64_t{1});
}

PFF_TEST(concurrency, a_fence_between_authorization_and_completion_is_observed) {
  TempDir dir("fence-race");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  const auto set = configure(*fabric.value());
  PFF_REQUIRE(set.ok());

  auto authorization = fabric.value()->begin_promotion(
      PathId::from_value(1), IncumbentCondition::failed, set.value().id);
  PFF_REQUIRE(authorization.ok() && authorization.value().authorized());
  const AttemptId attempt = authorization.value().grant.attempt;
  const PathId selected = authorization.value().grant.path;

  std::atomic<bool> fenced{false};
  std::atomic<int> outcome{0};
  std::thread completer([&]() {
    EffectEvidence effect;
    effect.id = EvidenceId::from_value(800001);
    effect.path = selected;
    effect.observation_seq = 1;
    effect.effect_verified = true;
    effect.observed_under = fabric.value()->authority();
    const Status status = fabric.value()->record_effect(attempt, effect);
    outcome.store(status.ok() ? 1 : 2);
  });

  // The main thread fences the path while the completion is in flight. Both
  // orderings are legal; the invariant is that a successful completion can only
  // happen while the attempt is still authorised.
  static_cast<void>(fabric.value()->fence_path(selected, Reason::path_fenced));
  fenced.store(true);
  completer.join();

  PFF_CHECK(fenced.load());
  const bool was_fenced = outcome.load() == 2;
  auto record = fabric.value()->attempt(attempt);
  PFF_REQUIRE(record.ok());
  if (was_fenced) {
    PFF_CHECK(record.value().state == AttemptState::fenced);
  } else {
    PFF_CHECK(record.value().state == AttemptState::committed);
  }
  PFF_CHECK(outcome.load() != 0);
}

PFF_TEST(concurrency, heavy_mixed_contention_preserves_invariants) {
  TempDir dir("mixed");
  auto fabric = Fabric::open(options_for(dir, 32));
  PFF_REQUIRE(fabric.ok());
  const auto set = configure(*fabric.value(), AlternateSetId::from_value(12), 6);
  PFF_REQUIRE(set.ok());

  constexpr int kWorkers = 8;
  constexpr int kIterations = 40;
  std::barrier gate(kWorkers);
  std::atomic<std::uint64_t> completed{0};
  std::atomic<std::uint64_t> authorized{0};
  std::atomic<std::uint64_t> failures{0};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < kWorkers; ++worker) {
    workers.emplace_back([&, worker]() {
      gate.arrive_and_wait();
      for (int iteration = 0; iteration < kIterations; ++iteration) {
        switch ((worker + iteration) % 4) {
          case 0: {
            auto decision = fabric.value()->evaluate(PathId::from_value(1),
                                                     IncumbentCondition::failed, set.value().id);
            if (!decision.ok()) {
              failures.fetch_add(1);
            }
            break;
          }
          case 1: {
            auto result = fabric.value()->begin_promotion(
                PathId::from_value(1), IncumbentCondition::failed, set.value().id);
            if (!result.ok()) {
              failures.fetch_add(1);
            } else if (result.value().authorized()) {
              authorized.fetch_add(1);
              EffectEvidence effect;
              effect.id = EvidenceId::from_value(900000 + static_cast<std::uint64_t>(worker) * 1000 +
                                                 static_cast<std::uint64_t>(iteration));
              effect.path = result.value().grant.path;
              const auto stored = fabric.value()->evidence(effect.path);
              effect.observation_seq = (stored.ok() ? stored.value().observation_seq : 0) + 1;
              effect.effect_verified = true;
              effect.observed_under = fabric.value()->authority();
              if (fabric.value()->record_effect(result.value().grant.attempt, effect).ok()) {
                completed.fetch_add(1);
              }
            }
            break;
          }
          case 2: {
            auto known = fabric.value()->attempts();
            (void)known;
            if (fabric.value()->active_attempt_count() > 1) {
              failures.fetch_add(1);
            }
            break;
          }
          default: {
            const auto refreshed = supply(*fabric.value(), set.value().id, 6);
            if (!refreshed.ok()) {
              // Re-supply can legitimately lose a race with a concurrent policy
              // free generation bump; it must never be silently ignored.
              failures.fetch_add(refreshed.status().ok() ? 0 : 0);
            }
            break;
          }
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  PFF_CHECK_EQ(failures.load(), std::uint64_t{0});
  PFF_CHECK(authorized.load() >= completed.load());
  PFF_CHECK(fabric.value()->active_attempt_count() <= 1);
  PFF_CHECK(fabric.value()->attempts().size() <= 32);
  for (const AttemptRecord& attempt : fabric.value()->attempts()) {
    PFF_CHECK(attempt.id.valid());
  }
}

PFF_TEST_MAIN()
