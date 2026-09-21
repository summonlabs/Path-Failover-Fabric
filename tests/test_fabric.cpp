// Fabric suite: coordinator lifecycle, transition ownership, fencing, rollback,
// reversion, restart semantics and resource bounds.
//
// Restart in this suite is a close/reopen of the runtime inside one process. The
// real process termination proof lives in the multiprocess suite.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <string>
#include <vector>

#include "framework.hpp"
#include "pff/fabric.hpp"
#include "support.hpp"
#include "tmpdir.hpp"

using namespace pff;
using pfftest::TempDir;

namespace {

struct Baseline {
  AlternateSetId set = AlternateSetId::from_value(7);
  PathId incumbent = PathId::from_value(1);
  AuthorityVector authority;
  std::vector<PathId> paths;
};

FabricOptions options_for(const TempDir& dir, std::uint64_t attempts = 64) {
  FabricOptions options;
  options.directory = dir.path();
  options.provenance = Provenance::synthetic;
  options.max_retained_attempts = static_cast<std::uint32_t>(attempts);
  options.enable_compaction = true;
  options.compaction_journal_bytes = limits::max_journal_bytes;
  return options;
}

// Simulates the adjacent upstream systems re-supplying policy, obligations, the
// topology generations, the alternate set and fresh observations under the
// authority the runtime currently holds.
Result<Baseline> install_baseline(Fabric& fabric, std::size_t path_count = 3,
                                  std::uint64_t set_id = 7) {
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

  Baseline baseline;
  baseline.set = AlternateSetId::from_value(set_id);
  baseline.authority = fabric.authority();
  const auto existing = fabric.alternate_set(baseline.set);
  AlternateSet set;
  set.id = baseline.set;
  set.generation = Generation::from_value(existing.ok() ? existing.value().generation.value() + 1 : 1);
  set.completeness = SetCompleteness::complete;
  for (std::size_t index = 0; index < path_count; ++index) {
    const PathId path = PathId::from_value(100 + index);
    AlternateCandidate candidate =
        pfftest::make_candidate(path, static_cast<std::uint32_t>(index), baseline.authority);
    candidate.evidence.health_ppm = static_cast<std::int64_t>(900000 - index);
    candidate.evidence.cost_units = static_cast<std::int64_t>(1000 + index);
    set.candidates.push_back(candidate);
    baseline.paths.push_back(path);
  }
  status = fabric.apply_alternate_set(set);
  if (!status.ok()) {
    return status;
  }
  for (AlternateCandidate candidate : set.candidates) {
    // Observations are monotonic per path: a re-supply must be strictly newer
    // than whatever the runtime already holds for that path.
    const auto stored = fabric.evidence(candidate.path);
    candidate.evidence.observation_seq = stored.ok() ? stored.value().observation_seq + 1 : 1;
    status = fabric.apply_evidence(candidate.evidence);
    if (!status.ok()) {
      return status;
    }
  }
  return baseline;
}

// Evidence observed under generations that are no longer current.
PathEvidence stale_evidence(PathId path, AuthorityVector authority,
                            std::uint64_t observation_seq) {
  authority.path_authority = Generation::from_value(authority.path_authority.value() - 1);
  PathEvidence evidence = [&] {
    PathEvidence value;
    value.id = EvidenceId::from_value(path.value() + 7000);
    value.path = path;
    value.freshness = Freshness::fresh;
    value.eligibility = Eligibility::eligible;
    value.reachability = Reachability::reachable;
    value.capacity = CapacityClass::sufficient;
    value.capabilities = 0x3u;
    value.health_ppm = 900000;
    value.cost_units = 1000;
    value.observation_seq = observation_seq;
    value.observed_under = authority;
    value.provenance = Provenance::synthetic;
    return value;
  }();
  (void)observation_seq;
  return evidence;
}

PathEvidence fresh_evidence(PathId path, const AuthorityVector& authority,
                            std::uint64_t observation_seq) {
  PathEvidence evidence;
  evidence.id = EvidenceId::from_value(path.value() + 5000);
  evidence.path = path;
  evidence.freshness = Freshness::fresh;
  evidence.eligibility = Eligibility::eligible;
  evidence.reachability = Reachability::reachable;
  evidence.capacity = CapacityClass::sufficient;
  evidence.capabilities = 0x3u;
  evidence.health_ppm = 900000;
  evidence.cost_units = 1000;
  evidence.observation_seq = observation_seq;
  evidence.observed_under = authority;
  evidence.provenance = Provenance::synthetic;
  return evidence;
}

// A verified effect whose observation sequence is strictly newer than whatever
// the runtime already holds for that path.
EffectEvidence next_verified_effect(Fabric& fabric, PathId path) {
  EffectEvidence effect;
  effect.id = EvidenceId::from_value(path.value() + 9000);
  effect.path = path;
  const auto stored = fabric.evidence(path);
  effect.observation_seq = stored.ok() ? stored.value().observation_seq + 1 : 1;
  effect.effect_verified = true;
  effect.observed_under = fabric.authority();
  effect.provenance = Provenance::synthetic;
  return effect;
}

EffectEvidence verified_effect(PathId path, const AuthorityVector& authority,
                               std::uint64_t observation_seq = 99) {
  EffectEvidence effect;
  effect.id = EvidenceId::from_value(path.value() + 9000);
  effect.path = path;
  effect.observation_seq = observation_seq;
  effect.effect_verified = true;
  effect.observed_under = authority;
  effect.provenance = Provenance::synthetic;
  return effect;
}

}  // namespace

PFF_TEST(fabric, promotion_commits_only_with_a_verified_effect) {
  TempDir dir("lifecycle");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  PFF_CHECK_EQ(fabric.value()->identity().epoch.value(), std::uint64_t{1});

  // Nothing is promotable before the adjacent systems supply anything.
  auto early = fabric.value()->begin_promotion(PathId::from_value(1), IncumbentCondition::failed,
                                               AlternateSetId::from_value(7));
  PFF_REQUIRE(early.ok());
  PFF_CHECK(!early.value().authorized());
  PFF_CHECK(early.value().decision.outcome == PromotionOutcome::refused_invalid_input);

  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());

  // A recommendation authorises nothing and does not take ownership.
  auto recommendation = fabric.value()->evaluate(baseline.value().incumbent,
                                                 IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(recommendation.ok());
  PFF_CHECK(recommendation.value().outcome == PromotionOutcome::authorized);
  PFF_CHECK(recommendation.value().authority_kind == AuthorityKind::recommendation);
  PFF_CHECK_EQ(fabric.value()->stats().promotions_authorized, std::uint64_t{0});
  PFF_CHECK_EQ(fabric.value()->active_attempt_count(), std::size_t{0});

  auto authorization = fabric.value()->begin_promotion(
      baseline.value().incumbent, IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(authorization.ok());
  PFF_REQUIRE(authorization.value().authorized());
  PFF_CHECK(authorization.value().decision.authority_kind == AuthorityKind::grant);
  PFF_CHECK(authorization.value().grant.kind == AuthorityKind::grant);
  const AttemptId attempt = authorization.value().grant.attempt;
  const PathId selected = authorization.value().grant.path;
  PFF_CHECK(selected.valid());
  PFF_CHECK_EQ(fabric.value()->stats().promotions_authorized, std::uint64_t{1});

  auto record = fabric.value()->attempt(attempt);
  PFF_REQUIRE(record.ok());
  PFF_CHECK(record.value().state == AttemptState::authorized);
  PFF_CHECK(record.value().selected == selected);
  PFF_CHECK(record.value().epoch == fabric.value()->identity().epoch);

  // A wrong path is not an effect on the authorised transition.
  EffectEvidence wrong_path = verified_effect(baseline.value().paths[1], fabric.value()->authority());
  PFF_CHECK_EQ(fabric.value()->record_effect(attempt, wrong_path).reason, Reason::effect_path_mismatch);

  // An unverified claim is not a verified effect.
  EffectEvidence unverified = verified_effect(selected, fabric.value()->authority());
  unverified.effect_verified = false;
  PFF_CHECK_EQ(fabric.value()->record_effect(attempt, unverified).reason, Reason::effect_unverified);

  // An effect observed under other generations is not current authority.
  EffectEvidence stale = verified_effect(selected, baseline.value().authority);
  stale.observed_under.epoch = Epoch::from_value(99);
  PFF_CHECK(!fabric.value()->record_effect(attempt, stale).ok());

  const Status committed =
      fabric.value()->record_effect(attempt, verified_effect(selected, fabric.value()->authority()));
  PFF_CHECK(committed.ok());
  PFF_CHECK_EQ(fabric.value()->stats().promotions_committed, std::uint64_t{1});
  PFF_CHECK_EQ(fabric.value()->active_attempt_count(), std::size_t{0});

  auto committed_record = fabric.value()->attempt(attempt);
  PFF_REQUIRE(committed_record.ok());
  PFF_CHECK(committed_record.value().state == AttemptState::committed);

  // A second completion for the same attempt is refused.
  PFF_CHECK_EQ(fabric.value()->record_effect(attempt, verified_effect(selected, fabric.value()->authority(), 200)).reason,
               Reason::attempt_already_committed);

  const std::vector<LineageEntry> lineage = fabric.value()->lineage(16);
  PFF_REQUIRE(lineage.size() == 1);
  PFF_CHECK(lineage[0].outcome == PromotionOutcome::promoted);
  PFF_CHECK(lineage[0].selected == selected);
  PFF_CHECK(lineage[0].attempt == attempt);
}

PFF_TEST(fabric, only_one_attempt_owns_the_transition) {
  TempDir dir("ownership");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());

  auto first = fabric.value()->begin_promotion(baseline.value().incumbent,
                                               IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(first.ok());
  PFF_REQUIRE(first.value().authorized());

  auto second = fabric.value()->begin_promotion(baseline.value().incumbent,
                                                IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(second.ok());
  PFF_CHECK(!second.value().authorized());
  PFF_CHECK(second.value().decision.outcome == PromotionOutcome::refused_already_owned);
  PFF_CHECK_EQ(fabric.value()->stats().promotions_refused, std::uint64_t{1});
  PFF_CHECK_EQ(fabric.value()->attempts().size(), std::size_t{1});
}

PFF_TEST(fabric, late_completion_from_a_superseded_attempt_is_refused) {
  TempDir dir("superseded");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());

  auto first = fabric.value()->begin_promotion(baseline.value().incumbent,
                                               IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(first.ok() && first.value().authorized());
  const AttemptId first_attempt = first.value().grant.attempt;
  const PathId first_path = first.value().grant.path;

  // Authority bearing dependency changes while the first attempt is in flight.
  FailoverPolicy new_policy = pfftest::make_policy(fabric.value()->authority().policy.value() + 1);
  PFF_REQUIRE(fabric.value()->apply_policy(new_policy).ok());

  auto fenced = fabric.value()->attempt(first_attempt);
  PFF_REQUIRE(fenced.ok());
  PFF_CHECK(fenced.value().state == AttemptState::fenced);
  PFF_CHECK(fenced.value().terminal_reason == Reason::authority_fenced);
  PFF_CHECK_EQ(fabric.value()->active_attempt_count(), std::size_t{0});

  // The late completion of the fenced attempt is refused.
  const Status late =
      fabric.value()->record_effect(first_attempt, verified_effect(first_path, fabric.value()->authority()));
  PFF_CHECK(!late.ok());
  PFF_CHECK(late.code == Code::fenced || late.code == Code::superseded);
  PFF_CHECK_EQ(fabric.value()->stats().stale_completions_rejected, std::uint64_t{1});

  // A new attempt can now own the transition, but the previous set is stale.
  auto second = fabric.value()->begin_promotion(baseline.value().incumbent,
                                                IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(second.ok());
  PFF_CHECK(!second.value().authorized());
  PFF_CHECK(second.value().decision.outcome == PromotionOutcome::refused_stale_authority);
}

PFF_TEST(fabric, fencing_a_path_revokes_a_targeted_attempt) {
  TempDir dir("fence-path");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());

  auto authorization = fabric.value()->begin_promotion(
      baseline.value().incumbent, IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(authorization.ok() && authorization.value().authorized());
  const AttemptId attempt = authorization.value().grant.attempt;
  const PathId selected = authorization.value().grant.path;

  PFF_REQUIRE(fabric.value()->fence_path(selected, Reason::path_fenced).ok());
  auto record = fabric.value()->attempt(attempt);
  PFF_REQUIRE(record.ok());
  PFF_CHECK(record.value().state == AttemptState::fenced);
  PFF_CHECK(record.value().terminal_reason == Reason::path_fenced);
  PFF_CHECK(!fabric.value()->record_effect(attempt, verified_effect(selected, fabric.value()->authority())).ok());

  // Once fenced, the path can never be selected again.
  auto again = fabric.value()->evaluate(baseline.value().incumbent, IncumbentCondition::failed,
                                        baseline.value().set);
  PFF_REQUIRE(again.ok());
  PFF_CHECK(again.value().selected != selected);
}

PFF_TEST(fabric, restart_advances_authority_and_fences_everything_in_flight) {
  TempDir dir("restart");
  AttemptId pre_restart;
  PathId pre_restart_path;
  std::uint64_t pre_restart_epoch = 0;
  BootId pre_restart_boot;
  {
    auto fabric = Fabric::open(options_for(dir));
    PFF_REQUIRE(fabric.ok());
    pre_restart_epoch = fabric.value()->identity().epoch.value();
    pre_restart_boot = fabric.value()->identity().boot;
    auto baseline = install_baseline(*fabric.value());
    PFF_REQUIRE(baseline.ok());
    auto authorization = fabric.value()->begin_promotion(
        baseline.value().incumbent, IncumbentCondition::failed, baseline.value().set);
    PFF_REQUIRE(authorization.ok() && authorization.value().authorized());
    pre_restart = authorization.value().grant.attempt;
    pre_restart_path = authorization.value().grant.path;
    // The runtime is destroyed with the transition still in flight, which is what
    // an abrupt process exit leaves behind on disk.
  }

  auto reopened = Fabric::open(options_for(dir));
  PFF_REQUIRE(reopened.ok());
  const FabricIdentity identity = reopened.value()->identity();
  PFF_CHECK_EQ(identity.epoch.value(), pre_restart_epoch + 1);
  PFF_CHECK(identity.boot != pre_restart_boot);
  PFF_CHECK_EQ(identity.restart_count, std::uint64_t{2});
  PFF_CHECK_EQ(reopened.value()->active_attempt_count(), std::size_t{0});

  auto attempt = reopened.value()->attempt(pre_restart);
  PFF_REQUIRE(attempt.ok());
  PFF_CHECK(attempt.value().state == AttemptState::interrupted);
  PFF_CHECK(attempt.value().terminal_reason == Reason::restart_fenced);
  PFF_CHECK(attempt.value().epoch.value() == pre_restart_epoch);

  // The pre restart grant is gone: its completion can never be accepted.
  const Status late = reopened.value()->record_effect(
      pre_restart, verified_effect(pre_restart_path, reopened.value()->authority()));
  PFF_CHECK(!late.ok());
  PFF_CHECK_EQ(late.code, Code::superseded);

  // Durability is not liveness: the persisted candidate authority is stale and no
  // evidence survived, so nothing may be promoted until upstream re-supplies.
  auto stale = reopened.value()->begin_promotion(PathId::from_value(1), IncumbentCondition::failed,
                                                 AlternateSetId::from_value(7));
  PFF_REQUIRE(stale.ok());
  PFF_CHECK(!stale.value().authorized());
  PFF_CHECK(stale.value().decision.outcome == PromotionOutcome::refused_stale_authority);
  PFF_CHECK(!reopened.value()->evidence(PathId::from_value(100)).ok());

  // Re-supplying under the new authority makes promotion possible again.
  auto baseline = install_baseline(*reopened.value());
  PFF_REQUIRE(baseline.ok());
  auto fresh = reopened.value()->begin_promotion(PathId::from_value(1), IncumbentCondition::failed,
                                                 AlternateSetId::from_value(7));
  PFF_REQUIRE(fresh.ok());
  PFF_CHECK(fresh.value().authorized());
}

PFF_TEST(fabric, restart_preserves_definitions_lineage_and_fences) {
  TempDir dir("restart-durable");
  PathId committed_path;
  {
    auto fabric = Fabric::open(options_for(dir));
    PFF_REQUIRE(fabric.ok());
    auto baseline = install_baseline(*fabric.value());
    PFF_REQUIRE(baseline.ok());
    auto authorization = fabric.value()->begin_promotion(
        baseline.value().incumbent, IncumbentCondition::failed, baseline.value().set);
    PFF_REQUIRE(authorization.ok() && authorization.value().authorized());
    committed_path = authorization.value().grant.path;
    PFF_REQUIRE(fabric.value()
                    ->record_effect(authorization.value().grant.attempt,
                                    verified_effect(committed_path, fabric.value()->authority()))
                    .ok());
    PFF_REQUIRE(fabric.value()->fence_path(PathId::from_value(999), Reason::path_fenced).ok());
  }
  auto reopened = Fabric::open(options_for(dir));
  PFF_REQUIRE(reopened.ok());
  PFF_CHECK(reopened.value()->policy_installed());
  PFF_CHECK(reopened.value()->obligations_installed());
  PFF_CHECK_EQ(reopened.value()->alternate_set_count(), std::size_t{1});
  const std::vector<LineageEntry> lineage = reopened.value()->lineage(16);
  PFF_REQUIRE(!lineage.empty());
  PFF_CHECK(lineage.back().outcome == PromotionOutcome::promoted);
  PFF_CHECK(lineage.back().selected == committed_path);
  PFF_CHECK_EQ(reopened.value()->fences().size(), std::size_t{1});
  PFF_CHECK(reopened.value()->fences()[0].path == PathId::from_value(999));
  PFF_CHECK_EQ(reopened.value()->stats().restarts, std::uint64_t{2});
}

PFF_TEST(fabric, generation_reuse_with_different_content_is_a_conflict) {
  TempDir dir("generations");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());

  const FailoverPolicy installed = fabric.value()->policy().value();
  PFF_CHECK(fabric.value()->apply_policy(installed).ok());  // idempotent
  FailoverPolicy changed = installed;
  changed.w_health = 42;
  const Status conflict = fabric.value()->apply_policy(changed);
  PFF_CHECK(!conflict.ok());
  PFF_CHECK_EQ(conflict.reason, Reason::generation_conflict);

  FailoverPolicy newer = installed;
  newer.generation = Generation::from_value(installed.generation.value() + 1);
  newer.w_cost = 9;
  PFF_REQUIRE(fabric.value()->apply_policy(newer).ok());
  PFF_CHECK_EQ(fabric.value()->apply_policy(installed).reason, Reason::policy_generation_stale);

  const AlternateSet set = fabric.value()->alternate_set(baseline.value().set).value();
  PFF_CHECK(fabric.value()->apply_alternate_set(set).ok());
  AlternateSet mutated = set;
  mutated.candidates[0].upstream_rank += 1;
  PFF_CHECK_EQ(fabric.value()->apply_alternate_set(mutated).reason, Reason::generation_conflict);
  AlternateSet newer_set = set;
  newer_set.generation = Generation::from_value(set.generation.value() + 1);
  PFF_REQUIRE(fabric.value()->apply_alternate_set(newer_set).ok());
  PFF_CHECK_EQ(fabric.value()->apply_alternate_set(set).reason, Reason::set_generation_mismatch);

  // Replayed and regressed observations are refused.
  PathEvidence evidence = set.candidates[0].evidence;
  PFF_CHECK_EQ(fabric.value()->apply_evidence(evidence).reason, Reason::evidence_superseded);
  evidence.observation_seq = 0;
  PFF_CHECK_EQ(fabric.value()->apply_evidence(evidence).reason, Reason::evidence_superseded);
  evidence.observation_seq = 2;
  PFF_CHECK(fabric.value()->apply_evidence(evidence).ok());
}

PFF_TEST(fabric, rollback_requires_fresh_incumbent_validation) {
  TempDir dir("rollback");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());

  auto authorization = fabric.value()->begin_promotion(
      baseline.value().incumbent, IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(authorization.ok() && authorization.value().authorized());
  const AttemptId attempt = authorization.value().grant.attempt;

  // Stale incumbent evidence cannot justify a rollback.
  PathEvidence stale =
      stale_evidence(baseline.value().incumbent, baseline.value().authority, 5);
  PFF_CHECK_EQ(fabric.value()->rollback(attempt, stale).reason, Reason::rollback_incumbent_invalid);

  // Neither can evidence that is fresh but not proven eligible.
  PathEvidence ineligible = fresh_evidence(baseline.value().incumbent, fabric.value()->authority(), 5);
  ineligible.eligibility = Eligibility::ineligible;
  PFF_CHECK_EQ(fabric.value()->rollback(attempt, ineligible).reason,
               Reason::rollback_incumbent_invalid);

  // Evidence about a different path is not the incumbent.
  PathEvidence other = fresh_evidence(baseline.value().paths[2], fabric.value()->authority(), 6);
  PFF_CHECK_EQ(fabric.value()->rollback(attempt, other).reason, Reason::effect_path_mismatch);

  PathEvidence good = fresh_evidence(baseline.value().incumbent, fabric.value()->authority(), 7);
  PFF_CHECK(fabric.value()->rollback(attempt, good).ok());
  auto record = fabric.value()->attempt(attempt);
  PFF_REQUIRE(record.ok());
  PFF_CHECK(record.value().state == AttemptState::rolled_back);
  PFF_CHECK_EQ(fabric.value()->stats().rollbacks, std::uint64_t{1});
  PFF_CHECK_EQ(fabric.value()->active_attempt_count(), std::size_t{0});
  const std::vector<LineageEntry> lineage = fabric.value()->lineage(8);
  PFF_REQUIRE(!lineage.empty());
  PFF_CHECK(lineage.back().outcome == PromotionOutcome::rolled_back);

  // A committed promotion is reverted, never rolled back.
  auto second = fabric.value()->begin_promotion(baseline.value().incumbent,
                                                IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(second.ok() && second.value().authorized());
  PFF_REQUIRE(fabric.value()
                  ->record_effect(second.value().grant.attempt,
                                  verified_effect(second.value().grant.path,
                                                  fabric.value()->authority()))
                  .ok());
  PFF_CHECK_EQ(fabric.value()->rollback(second.value().grant.attempt, good).reason,
               Reason::rollback_not_committed);
}

PFF_TEST(fabric, reversion_is_an_explicit_transition_with_fresh_validation) {
  TempDir dir("revert");
  auto fabric = Fabric::open(options_for(dir));
  PFF_REQUIRE(fabric.ok());
  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());

  auto authorization = fabric.value()->begin_promotion(
      baseline.value().incumbent, IncumbentCondition::failed, baseline.value().set);
  PFF_REQUIRE(authorization.ok() && authorization.value().authorized());
  const PathId promoted_path = authorization.value().grant.path;
  PFF_REQUIRE(fabric.value()
                  ->record_effect(authorization.value().grant.attempt,
                                  verified_effect(promoted_path, fabric.value()->authority()))
                  .ok());

  // The displaced path cannot be restored without fresh validation of its own.
  PathEvidence stale =
      stale_evidence(baseline.value().incumbent, baseline.value().authority, 11);
  PFF_CHECK(!fabric.value()->revert(authorization.value().grant.attempt, stale).ok());

  PathEvidence good = fresh_evidence(baseline.value().incumbent, fabric.value()->authority(), 12);
  auto reverted = fabric.value()->revert(authorization.value().grant.attempt, good);
  PFF_REQUIRE(reverted.ok());
  PFF_CHECK(reverted.value().outcome == PromotionOutcome::reverted);
  PFF_CHECK(reverted.value().selected == baseline.value().incumbent);
  PFF_CHECK(reverted.value().incumbent == promoted_path);
  PFF_CHECK_EQ(fabric.value()->stats().reversions, std::uint64_t{1});
  const std::vector<LineageEntry> lineage = fabric.value()->lineage(8);
  PFF_REQUIRE(lineage.size() >= 2);
  PFF_CHECK(lineage.back().outcome == PromotionOutcome::reverted);

  // Reversion applies only to a committed promotion.
  PathEvidence another = fresh_evidence(baseline.value().paths[1], fabric.value()->authority(), 13);
  PFF_CHECK(!fabric.value()->revert(AttemptId::from_value(9999), another).ok());
}

PFF_TEST(fabric, resource_bounds_refuse_rather_than_grow) {
  TempDir dir("bounds");
  auto fabric = Fabric::open(options_for(dir, 2));
  PFF_REQUIRE(fabric.ok());
  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());

  for (int index = 0; index < 6; ++index) {
    auto authorization = fabric.value()->begin_promotion(
        baseline.value().incumbent, IncumbentCondition::failed, baseline.value().set);
    if (!authorization.ok() || !authorization.value().authorized()) {
      break;
    }
    static_cast<void>(fabric.value()->abandon_attempt(authorization.value().grant.attempt,
                                                      Reason::no_active_attempt));
  }
  PFF_CHECK(fabric.value()->attempts().size() <= 2);
  PFF_CHECK(fabric.value()->stats().attempts_dropped >= 1);

  // The fence table refuses rather than silently dropping authority.
  for (std::uint64_t index = 0; index < limits::max_fences; ++index) {
    PFF_REQUIRE(fabric.value()->fence_path(PathId::from_value(100000 + index), Reason::path_fenced).ok());
  }
  const Status full = fabric.value()->fence_path(PathId::from_value(999999), Reason::path_fenced);
  PFF_CHECK(!full.ok());
  PFF_CHECK_EQ(full.reason, Reason::table_full);
}

PFF_TEST(fabric, compaction_keeps_the_bounded_journal_recoverable) {
  TempDir dir("compaction");
  FabricOptions options = options_for(dir);
  options.compaction_journal_bytes = 4096;
  {
    auto fabric = Fabric::open(options);
    PFF_REQUIRE(fabric.ok());
    auto baseline = install_baseline(*fabric.value());
    PFF_REQUIRE(baseline.ok());
    for (int index = 0; index < 8; ++index) {
      auto authorization = fabric.value()->begin_promotion(
          baseline.value().incumbent, IncumbentCondition::failed, baseline.value().set);
      PFF_REQUIRE(authorization.ok());
      if (!authorization.value().authorized()) {
        auto refreshed = install_baseline(*fabric.value());
        PFF_REQUIRE(refreshed.ok());
        continue;
      }
      PFF_REQUIRE(fabric.value()
                      ->record_effect(authorization.value().grant.attempt,
                                      next_verified_effect(*fabric.value(),
                                                           authorization.value().grant.path))
                      .ok());
      auto refreshed = install_baseline(*fabric.value());
      PFF_REQUIRE(refreshed.ok());
    }
    PFF_CHECK(fabric.value()->stats().snapshots_written > 0 ||
              fabric.value()->recovery().snapshot_loaded);
  }
  auto reopened = Fabric::open(options);
  PFF_REQUIRE(reopened.ok());
  PFF_CHECK(reopened.value()->recovery().snapshot_loaded);
  PFF_CHECK(reopened.value()->policy_installed());
  PFF_CHECK(!reopened.value()->lineage(64).empty());
}

PFF_TEST(fabric, a_second_runtime_cannot_own_the_same_directory) {
  TempDir dir("exclusive");
  auto first = Fabric::open(options_for(dir));
  PFF_REQUIRE(first.ok());
  auto second = Fabric::open(options_for(dir));
  PFF_CHECK(!second.ok());
  PFF_CHECK_EQ(second.status().reason, Reason::directory_locked);
}

PFF_TEST(fabric, fabricated_evidence_is_labelled_with_its_provenance) {
  TempDir dir("provenance");
  FabricOptions options = options_for(dir);
  options.provenance = Provenance::synthetic;
  auto fabric = Fabric::open(options);
  PFF_REQUIRE(fabric.ok());
  auto baseline = install_baseline(*fabric.value());
  PFF_REQUIRE(baseline.ok());
  auto decision = fabric.value()->evaluate(baseline.value().incumbent, IncumbentCondition::failed,
                                           baseline.value().set);
  PFF_REQUIRE(decision.ok());
  PFF_CHECK(decision.value().provenance == Provenance::synthetic);
  PFF_CHECK(std::string(to_string(decision.value().provenance)) == "SYNTHETIC");
  PFF_CHECK(decision.value().basis == fabric.value()->authority());
}

PFF_TEST_MAIN()
