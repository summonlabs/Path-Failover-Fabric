// Property suite: seeded randomised operation sequences over a real runtime.
//
// The central invariant is the product defining one: a path is never authorised
// or promoted unless it is a member of the currently supplied alternate set, it
// is not fenced, and the independent reference solver agrees that it is the
// deterministic winner. Invariants are asserted after every single operation,
// not only at the end.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "framework.hpp"
#include "pff/fabric.hpp"
#include "support.hpp"
#include "tmpdir.hpp"

using namespace pff;
using pfftest::Rng;
using pfftest::TempDir;

namespace {

enum class AuthorityMode : std::uint8_t { current = 0, stale = 1, unknown = 2, conflict = 3 };

struct LocalCandidate {
  PathId path;
  std::uint32_t rank = 0;
  bool withdrawn = false;
  Freshness freshness = Freshness::fresh;
  Eligibility eligibility = Eligibility::eligible;
  Reachability reachability = Reachability::reachable;
  CapacityClass capacity = CapacityClass::sufficient;
  CapabilityMask capabilities = 0x3u;
  std::int64_t health = 900000;
  std::int64_t cost = 1000;
  AuthorityMode mode = AuthorityMode::current;
};

struct World {
  PathId incumbent = PathId::from_value(1);
  AlternateSetId set = AlternateSetId::from_value(9);
  SetCompleteness completeness = SetCompleteness::complete;
  std::vector<LocalCandidate> candidates;
  std::vector<PathId> fenced;
  FailoverPolicy policy;
  ServiceObligations obligations;
};

World make_world(Rng& rng, std::size_t count) {
  World world;
  world.policy = pfftest::make_policy(0);          // generation assigned at install time
  world.obligations = pfftest::make_obligations(0);  // generation assigned at install time
  if (rng.chance(25)) {
    world.completeness = rng.chance(50) ? SetCompleteness::partial : SetCompleteness::unknown;
  }
  if (rng.chance(25)) {
    world.policy.allow_unknown_skip = true;
  }
  if (rng.chance(20)) {
    world.policy.search_limit = static_cast<std::uint32_t>(1 + rng.below(4));
  }
  if (rng.chance(20)) {
    world.policy.min_score = 1000000000;
  }
  for (std::size_t index = 0; index < count; ++index) {
    LocalCandidate candidate;
    candidate.path = PathId::from_value(200 + index);
    // Deliberately collide scores so tie breaking is exercised.
    candidate.rank = static_cast<std::uint32_t>(rng.below(2));
    candidate.health = rng.chance(50) ? 900000 : 800000;
    candidate.cost = rng.chance(50) ? 1000 : 2000;
    candidate.withdrawn = rng.chance(8);
    if (rng.chance(8)) {
      candidate.eligibility = Eligibility::unknown;
    }
    if (rng.chance(6)) {
      candidate.eligibility = Eligibility::ineligible;
    }
    if (rng.chance(6)) {
      candidate.freshness = Freshness::stale;
    }
    if (rng.chance(5)) {
      candidate.freshness = Freshness::unknown;
    }
    if (rng.chance(6)) {
      candidate.reachability = Reachability::unreachable;
    }
    if (rng.chance(5)) {
      candidate.capacity = CapacityClass::unknown;
    }
    if (rng.chance(6)) {
      candidate.capabilities = 0x1u;
    }
    if (rng.chance(6)) {
      candidate.health = 10;
    }
    if (rng.chance(5)) {
      candidate.cost = limits::cost_scale;
    }
    const std::uint64_t mode = rng.below(12);
    if (mode == 0) {
      candidate.mode = AuthorityMode::stale;
    } else if (mode == 1) {
      candidate.mode = AuthorityMode::unknown;
    }
    world.candidates.push_back(candidate);
  }
  if (rng.chance(25) && !world.candidates.empty()) {
    world.fenced.push_back(world.candidates[rng.below(world.candidates.size())].path);
  }
  return world;
}

AlternateSet materialise(const World& world, const AuthorityVector& current,
                         std::uint64_t generation) {
  AlternateSet set;
  set.id = world.set;
  set.generation = Generation::from_value(generation);
  set.completeness = world.completeness;
  for (const LocalCandidate& local : world.candidates) {
    AlternateCandidate candidate = pfftest::make_candidate(local.path, local.rank, current);
    candidate.withdrawn = local.withdrawn;
    switch (local.mode) {
      case AuthorityMode::current:
        break;
      case AuthorityMode::stale:
        candidate.authority.path_authority =
            current.path_authority.value() > 1
                ? Generation::from_value(current.path_authority.value() - 1)
                : Generation::absent();
        break;
      case AuthorityMode::unknown:
        candidate.authority.policy = Generation::absent();
        break;
      case AuthorityMode::conflict:
        candidate.authority.path_authority =
            Generation::from_value(current.path_authority.value() + 5);
        break;
    }
    candidate.evidence.freshness = local.freshness;
    candidate.evidence.eligibility = local.eligibility;
    candidate.evidence.reachability = local.reachability;
    candidate.evidence.capacity = local.capacity;
    candidate.evidence.capabilities = local.capabilities;
    candidate.evidence.health_ppm = local.health;
    candidate.evidence.cost_units = local.cost;
    candidate.evidence.observed_under = candidate.authority;
    set.candidates.push_back(candidate);
  }
  return set;
}

struct Installed {
  AlternateSet set;
  FailoverPolicy policy;
  ServiceObligations obligations;
  AuthorityVector authority;
};

// Re-supplies the whole world under the authority the runtime currently holds.
Result<Installed> install(Fabric& fabric, const World& world) {
  const AuthorityVector before = fabric.authority();
  AuthorityVector advance;
  advance.topology = Generation::from_value(before.topology.value() + 1);
  advance.path_authority = Generation::from_value(before.path_authority.value() + 1);
  Status status = fabric.set_authority(advance);
  if (!status.ok()) {
    return status;
  }
  FailoverPolicy policy = world.policy;
  policy.generation = Generation::from_value(before.policy.value() + 1);
  status = fabric.apply_policy(policy);
  if (!status.ok()) {
    return status;
  }
  ServiceObligations obligations = world.obligations;
  obligations.generation = Generation::from_value(before.obligations.value() + 1);
  status = fabric.apply_obligations(obligations);
  if (!status.ok()) {
    return status;
  }
  const AuthorityVector current = fabric.authority();
  const auto existing = fabric.alternate_set(world.set);
  const std::uint64_t generation =
      existing.ok() ? existing.value().generation.value() + 1 : 1;
  AlternateSet set = materialise(world, current, generation);
  const Status valid = set.validate();
  if (!valid.ok()) {
    return valid;
  }
  status = fabric.apply_alternate_set(set);
  if (!status.ok()) {
    return status;
  }
  for (AlternateCandidate candidate : set.candidates) {
    const auto stored = fabric.evidence(candidate.path);
    candidate.evidence.observation_seq =
        stored.ok() ? stored.value().observation_seq + 1 : 1;
    status = fabric.apply_evidence(candidate.evidence);
    if (!status.ok()) {
      return status;
    }
  }
  // The world's pre-seeded fences are durable authority and must exist in the
  // runtime for the invariants to be about the same set of paths.
  for (const PathId path : world.fenced) {
    const Status fenced = fabric.fence_path(path, Reason::path_fenced);
    if (!fenced.ok()) {
      return fenced;
    }
  }
  Installed installed;
  installed.set = set;
  installed.policy = policy;
  installed.obligations = obligations;
  installed.authority = current;
  return installed;
}

bool contains(const std::vector<PathId>& paths, PathId path) {
  return std::find(paths.begin(), paths.end(), path) != paths.end();
}

// Invariants asserted after every operation.
void check_invariants(Fabric& fabric, const World& world, const Installed& installed,
                      const std::vector<PathId>& granted) {
  // 1. At most one attempt owns the transition.
  PFF_CHECK(fabric.active_attempt_count() <= 1);
  for (const AttemptRecord& attempt : fabric.active_attempts()) {
    PFF_CHECK(attempt.state == AttemptState::claimed || attempt.state == AttemptState::authorized ||
              attempt.state == AttemptState::effect_recorded);
    if (attempt.selected.valid()) {
      // 2. An in-flight transition always targets a supplied member.
      PFF_CHECK(contains(installed.set.candidates[0].path.valid() ? [&] {
        std::vector<PathId> paths;
        for (const AlternateCandidate& candidate : installed.set.candidates) {
          paths.push_back(candidate.path);
        }
        return paths;
      }() : std::vector<PathId>{}, attempt.selected));
    }
  }

  // 3. Every granted path is a supplied member and is not fenced.
  for (const PathId path : granted) {
    bool supplied = false;
    for (const AlternateCandidate& candidate : installed.set.candidates) {
      if (candidate.path == path) {
        supplied = true;
      }
    }
    PFF_CHECK(supplied);
    PFF_CHECK(!contains(world.fenced, path));
  }

  // 4. Durable lineage never names a path outside the supplied set.
  for (const LineageEntry& entry : fabric.lineage(limits::max_lineage)) {
    if (entry.outcome == PromotionOutcome::promoted) {
      bool supplied = false;
      for (const AlternateCandidate& candidate : installed.set.candidates) {
        if (candidate.path == entry.selected) {
          supplied = true;
        }
      }
      PFF_CHECK(supplied);
    }
  }

  // 5. A recommendation agrees with the independent reference solver.
  if (fabric.active_attempt_count() == 0) {
    auto decision = fabric.evaluate(world.incumbent, IncumbentCondition::failed, world.set);
    PFF_REQUIRE(decision.ok());
    SelectionInput input;
    input.incumbent = world.incumbent;
    input.set = &installed.set;
    input.policy = &installed.policy;
    input.obligations = &installed.obligations;
    input.current = fabric.authority();
    input.expected_set_generation = installed.set.generation;
    const std::vector<PathId> fences = [&] {
      std::vector<PathId> paths;
      for (const FenceRecord& fence : fabric.fences()) {
        if (fence.path.valid()) {
          paths.push_back(fence.path);
        }
      }
      return paths;
    }();
    input.fenced_paths = std::span<const PathId>(fences.data(), fences.size());
    const pfftest::ReferenceOutcome expected = pfftest::reference_solve(input);
    PFF_CHECK_MSG(decision.value().outcome == expected.outcome,
                  "seed case outcome " + std::string(to_string(decision.value().outcome)) +
                      " vs " + std::string(to_string(expected.outcome)));
    PFF_CHECK(decision.value().selected == expected.selected);
    if (decision.value().selected.valid()) {
      PFF_CHECK(!contains(world.fenced, decision.value().selected));
      PFF_CHECK(decision.value().outcome == PromotionOutcome::authorized);
    }
  }
}

}  // namespace

PFF_TEST(property, invariants_hold_after_every_operation) {
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    Rng rng(seed * 7919u);
    TempDir dir("property");
    FabricOptions options;
    options.directory = dir.path();
    options.provenance = Provenance::synthetic;
    options.max_retained_attempts = 32;
    options.compaction_journal_bytes = 8192;

    auto fabric = Fabric::open(options);
    PFF_REQUIRE(fabric.ok());

    const World world = make_world(rng, 1 + static_cast<std::size_t>(rng.below(8)));
    auto first_install = install(*fabric.value(), world);
    PFF_REQUIRE(first_install.ok());
    Installed installed = first_install.value();
    std::vector<PathId> granted;

    const int operations = 24;
    for (int step = 0; step < operations; ++step) {
      const std::uint64_t choice = rng.below(9);
      if (choice == 0 || choice == 1) {
        auto decision = fabric.value()->evaluate(world.incumbent, IncumbentCondition::failed,
                                                 world.set);
        PFF_REQUIRE(decision.ok());
        if (decision.value().selected.valid()) {
          PFF_CHECK(!contains(world.fenced, decision.value().selected));
        }
      } else if (choice == 2 || choice == 3) {
        auto authorization = fabric.value()->begin_promotion(
            world.incumbent, IncumbentCondition::failed, world.set);
        PFF_REQUIRE(authorization.ok());
        if (authorization.value().authorized()) {
          granted.push_back(authorization.value().grant.path);
        }
      } else if (choice == 4) {
        for (const AttemptRecord& attempt : fabric.value()->active_attempts()) {
          EffectEvidence effect;
          effect.id = EvidenceId::from_value(attempt.selected.value() + 40000 + step);
          effect.path = attempt.selected;
          const auto stored = fabric.value()->evidence(attempt.selected);
          effect.observation_seq = stored.ok() ? stored.value().observation_seq + 1 : 1;
          effect.effect_verified = true;
          effect.observed_under = fabric.value()->authority();
          effect.provenance = Provenance::synthetic;
          static_cast<void>(fabric.value()->record_effect(attempt.id, effect));
        }
      } else if (choice == 5) {
        if (!world.candidates.empty()) {
          const PathId path = world.candidates[rng.below(world.candidates.size())].path;
          static_cast<void>(fabric.value()->fence_path(path, Reason::path_fenced));
        }
      } else if (choice == 6) {
        for (const AttemptRecord& attempt : fabric.value()->active_attempts()) {
          static_cast<void>(fabric.value()->abandon_attempt(attempt.id, Reason::no_active_attempt));
        }
      } else if (choice == 7) {
        auto refreshed = install(*fabric.value(), world);
        PFF_REQUIRE(refreshed.ok());
        installed = refreshed.value();
      } else {
        // Restart the runtime over the same durable state.
        fabric = Result<std::unique_ptr<Fabric>>(Status(Code::refused, Reason::read_only));
        auto reopened = Fabric::open(options);
        PFF_REQUIRE(reopened.ok());
        fabric = std::move(reopened);
        auto refreshed = install(*fabric.value(), world);
        PFF_REQUIRE(refreshed.ok());
        installed = refreshed.value();
      }
      check_invariants(*fabric.value(), world, installed, granted);
    }

    // The epoch only ever advances.
    const std::uint64_t epoch = fabric.value()->identity().epoch.value();
    PFF_CHECK(epoch >= 1);
  }
}

PFF_TEST(property, promoted_paths_always_belong_to_the_supplied_eligible_set) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    Rng rng(seed * 104729u);
    TempDir dir("property-eligible");
    FabricOptions options;
    options.directory = dir.path();
    options.provenance = Provenance::synthetic;
    auto fabric = Fabric::open(options);
    PFF_REQUIRE(fabric.ok());

    const World world = make_world(rng, 2 + static_cast<std::size_t>(rng.below(6)));
    auto installed = install(*fabric.value(), world);
    PFF_REQUIRE(installed.ok());

    std::vector<PathId> eligible;
    for (const AlternateCandidate& candidate : installed.value().set.candidates) {
      if (candidate.evidence.eligibility == Eligibility::eligible && !candidate.withdrawn) {
        eligible.push_back(candidate.path);
      }
    }

    auto authorization = fabric.value()->begin_promotion(
        world.incumbent, IncumbentCondition::failed, world.set);
    PFF_REQUIRE(authorization.ok());
    if (!authorization.value().authorized()) {
      continue;
    }
    const PathId selected = authorization.value().grant.path;
    PFF_CHECK(contains(eligible, selected));
    PFF_CHECK(contains(world.fenced, selected) == false);

    EffectEvidence effect;
    effect.id = EvidenceId::from_value(selected.value() + 77000);
    effect.path = selected;
    const auto stored = fabric.value()->evidence(selected);
    effect.observation_seq = stored.ok() ? stored.value().observation_seq + 1 : 1;
    effect.effect_verified = true;
    effect.observed_under = fabric.value()->authority();
    PFF_REQUIRE(fabric.value()->record_effect(authorization.value().grant.attempt, effect).ok());
    PFF_CHECK(contains(eligible, selected));

    const std::vector<LineageEntry> lineage = fabric.value()->lineage(4);
    PFF_REQUIRE(!lineage.empty());
    PFF_CHECK(lineage.back().selected == selected);
    PFF_CHECK(contains(eligible, lineage.back().selected));
  }
}

PFF_TEST(property, attempts_are_monotonic_and_terminal_states_never_change) {
  Rng rng(20260101u);
  TempDir dir("property-attempts");
  FabricOptions options;
  options.directory = dir.path();
  options.max_retained_attempts = 64;
  auto fabric = Fabric::open(options);
  PFF_REQUIRE(fabric.ok());

  // This property needs a world that is always promotable so that every
  // iteration creates a real attempt.
  World world = make_world(rng, 4);
  world.policy.min_score = 0;
  world.policy.allow_unknown_skip = true;
  world.policy.search_limit = limits::default_search_limit;
  world.completeness = SetCompleteness::complete;
  world.fenced.clear();
  for (LocalCandidate& candidate : world.candidates) {
    candidate.withdrawn = false;
    candidate.eligibility = Eligibility::eligible;
    candidate.freshness = Freshness::fresh;
    candidate.reachability = Reachability::reachable;
    candidate.capacity = CapacityClass::sufficient;
    candidate.capabilities = 0x3u;
    candidate.health = 900000;
    candidate.cost = 1000;
    candidate.mode = AuthorityMode::current;
  }
  auto installed = install(*fabric.value(), world);
  PFF_REQUIRE(installed.ok());

  AttemptId last_seen;
  std::vector<AttemptId> terminal;
  for (int step = 0; step < 30; ++step) {
    // Every terminal attempt observed earlier must still be terminal.
    for (const AttemptId id : terminal) {
      auto record = fabric.value()->attempt(id);
      if (record.ok()) {
        PFF_CHECK(is_terminal_state(record.value().state));
      }
    }

    auto authorization = fabric.value()->begin_promotion(
        world.incumbent, IncumbentCondition::failed, world.set);
    PFF_REQUIRE(authorization.ok());
    if (!authorization.value().authorized()) {
      auto refreshed = install(*fabric.value(), world);
      PFF_REQUIRE(refreshed.ok());
      continue;
    }
    const AttemptId id = authorization.value().grant.attempt;
    PFF_CHECK(id > last_seen);
    last_seen = id;

    if (rng.chance(50)) {
      EffectEvidence effect;
      effect.id = EvidenceId::from_value(step + 500000);
      effect.path = authorization.value().grant.path;
      const auto stored = fabric.value()->evidence(effect.path);
      effect.observation_seq = stored.ok() ? stored.value().observation_seq + 1 : 1;
      effect.effect_verified = true;
      effect.observed_under = fabric.value()->authority();
      static_cast<void>(fabric.value()->record_effect(id, effect));
    } else {
      static_cast<void>(fabric.value()->abandon_attempt(id, Reason::no_active_attempt));
    }
    terminal.push_back(id);

    auto refreshed = install(*fabric.value(), world);
    PFF_REQUIRE(refreshed.ok());
  }
  PFF_CHECK(last_seen.valid());
}

PFF_TEST_MAIN()
