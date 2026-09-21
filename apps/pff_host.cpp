// pff_host - scenario worker used by the multiprocess and crash/restart proofs.
//
// Every invocation is a real, independent operating system process that opens
// the durable state directory, performs a bounded scenario and exits (or is
// hard killed at a requested boundary). The worker never simulates a crash: the
// crash stages terminate the process with _Exit so no destructor, flush or
// buffer is given a chance to run.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "args.hpp"
#include "pff/fabric.hpp"

namespace {

using namespace pff;
using namespace pff::tools;

void crash_now() {
  std::fflush(nullptr);
  std::_Exit(9);
}

struct Prepared {
  AlternateSetId set;
  Generation set_generation;
  AuthorityVector authority;
  std::vector<PathId> paths;
  PathId incumbent;
};

// Simulates the adjacent upstream systems: it re-supplies policy, obligations,
// the topology and path authority generations, the alternate set and fresh
// observations for every member, all bound to the authority vector the runtime
// currently holds. This is what a real path authority does after a restart.
Result<Prepared> prepare(Fabric& fabric, AlternateSetId set_id, std::size_t path_count,
                         PathId incumbent) {
  const AuthorityVector before = fabric.authority();

  AuthorityVector advance;
  advance.topology = Generation::from_value(before.topology.value() + 1);
  advance.path_authority = Generation::from_value(before.path_authority.value() + 1);
  Status status = fabric.set_authority(advance);
  if (!status.ok()) {
    return status;
  }

  FailoverPolicy policy;
  policy.generation = Generation::from_value(before.policy.value() + 1);
  policy.w_health = 4;
  policy.w_cost = 1;
  policy.w_rank = 1;
  policy.w_capability = 1;
  policy.min_score = 0;
  policy.search_limit = limits::default_search_limit;
  policy.max_retained_attempts = 256;
  policy.max_revalidations = 4;
  policy.allow_unknown_skip = false;
  policy.require_verified_effect = true;
  status = fabric.apply_policy(policy);
  if (!status.ok()) {
    return status;
  }

  ServiceObligations obligations;
  obligations.generation = Generation::from_value(before.obligations.value() + 1);
  obligations.required_capabilities = 0x3u;
  obligations.min_health_ppm = 500000;
  obligations.max_cost_units = 5000000;
  obligations.require_reachable = true;
  obligations.require_capacity = true;
  obligations.require_fresh_evidence = true;
  status = fabric.apply_obligations(obligations);
  if (!status.ok()) {
    return status;
  }

  Prepared prepared;
  prepared.set = set_id;
  prepared.incumbent = incumbent;
  prepared.authority = fabric.authority();

  AlternateSet set;
  set.id = prepared.set;
  set.completeness = SetCompleteness::complete;
  const auto existing = fabric.alternate_set(prepared.set);
  set.generation = Generation::from_value(existing.ok() ? existing.value().generation.value() + 1 : 1);
  prepared.set_generation = set.generation;

  for (std::size_t index = 0; index < path_count; ++index) {
    const PathId path = PathId::from_value(1000 + static_cast<std::uint64_t>(index));
    if (path == prepared.incumbent) {
      continue;
    }
    AlternateCandidate candidate;
    candidate.path = path;
    candidate.upstream_rank = static_cast<std::uint32_t>(index);
    candidate.withdrawn = false;
    candidate.authority = prepared.authority;
    candidate.evidence.id = EvidenceId::from_value(path.value());
    candidate.evidence.path = path;
    candidate.evidence.freshness = Freshness::fresh;
    candidate.evidence.eligibility = Eligibility::eligible;
    candidate.evidence.reachability = Reachability::reachable;
    candidate.evidence.capacity = CapacityClass::sufficient;
    candidate.evidence.capabilities = 0x3u;
    candidate.evidence.health_ppm = static_cast<std::int64_t>(900000 - index);
    candidate.evidence.cost_units = static_cast<std::int64_t>(100 + index);
    candidate.evidence.observation_seq = 1;
    candidate.evidence.observed_under = prepared.authority;
    candidate.evidence.provenance = Provenance::synthetic;
    set.candidates.push_back(candidate);
    prepared.paths.push_back(path);
  }

  status = fabric.apply_alternate_set(set);
  if (!status.ok()) {
    return status;
  }
  for (const AlternateCandidate& candidate : set.candidates) {
    status = fabric.apply_evidence(candidate.evidence);
    if (!status.ok()) {
      return status;
    }
  }
  return prepared;
}

void print_summary(Fabric& fabric) {
  const FabricIdentity identity = fabric.identity();
  const FabricStats stats = fabric.stats();
  std::printf("EPOCH %llu\n", static_cast<unsigned long long>(identity.epoch.value()));
  std::printf("BOOT %s\n", id_hex(identity.boot).c_str());
  std::printf("INCARNATION %s\n", id_hex(identity.incarnation).c_str());
  std::printf("RESTARTS %llu\n", static_cast<unsigned long long>(identity.restart_count));
  std::printf("POLICY_INSTALLED %d\n", fabric.policy_installed() ? 1 : 0);
  std::printf("OBLIGATIONS_INSTALLED %d\n", fabric.obligations_installed() ? 1 : 0);
  std::printf("ACTIVE_ATTEMPTS %zu\n", fabric.active_attempt_count());
  std::printf("AUTHORIZED %llu\n", static_cast<unsigned long long>(stats.promotions_authorized));
  std::printf("COMMITTED %llu\n", static_cast<unsigned long long>(stats.promotions_committed));
  std::printf("STALE_COMPLETIONS_REJECTED %llu\n",
              static_cast<unsigned long long>(stats.stale_completions_rejected));
  const RecoveryReport& recovery = fabric.recovery();
  std::printf("RECOVERY snapshot=%d torn=%d replayed=%zu truncated_bytes=%llu\n",
              recovery.snapshot_loaded ? 1 : 0, recovery.torn_tail_present ? 1 : 0,
              recovery.records_replayed,
              static_cast<unsigned long long>(recovery.truncated_bytes));
  for (const AttemptRecord& attempt : fabric.attempts()) {
    std::printf("ATTEMPT %llu %s %s selected=%s epoch=%llu\n",
                static_cast<unsigned long long>(attempt.id.value()), to_string(attempt.state),
                to_string(attempt.terminal_reason), id_hex(attempt.selected).c_str(),
                static_cast<unsigned long long>(attempt.epoch.value()));
  }
  for (const LineageEntry& entry : fabric.lineage(limits::max_lineage)) {
    std::printf("LINEAGE %s attempt=%llu selected=%s reason=%s\n", to_string(entry.outcome),
                static_cast<unsigned long long>(entry.attempt.value()),
                id_hex(entry.selected).c_str(), to_string(entry.reason));
  }
  for (const FenceRecord& fence : fabric.fences()) {
    std::printf("FENCE attempt=%llu path=%s reason=%s\n",
                static_cast<unsigned long long>(fence.attempt.value()),
                id_hex(fence.path).c_str(), to_string(fence.reason));
  }
}

}  // namespace

int run_main(int argc, char** argv) {
  using namespace pff;
  using namespace pff::tools;

  const auto cmd = command(argc, argv);
  const auto directory = arg_value(argc, argv, "--dir");
  if (!cmd.has_value() || !directory.has_value()) {
    std::fprintf(stderr,
                 "usage: pff_host --dir D <command> [args]\n"
                 "  prepare --set S [--paths N] [--incumbent I]\n"
                 "  promote --set S [--paths N] [--incumbent I] [--crash stage]\n"
                 "  verify [--probe-set S] [--probe-incumbent I]\n"
                 "  late-effect --attempt A --path P\n"
                 "  hold\n"
                 "  try-open\n");
    return 2;
  }

  FabricOptions options;
  options.directory = *directory;
  const std::string provenance = arg_or(argc, argv, "--provenance", "synthetic");
  options.provenance = provenance == "real" ? Provenance::real : Provenance::synthetic;

  auto fabric = Fabric::open(options);
  if (!fabric.ok()) {
    if (*cmd == "try-open") {
      std::printf("OPEN denied %s %s\n", to_string(fabric.status().code),
                  to_string(fabric.status().reason));
      std::fflush(stdout);
      return 0;
    }
    std::fprintf(stderr, "pff_host: cannot open: %s\n", fabric.status().describe().c_str());
    return 3;
  }
  if (*cmd == "try-open") {
    std::printf("OPEN ok\n");
    std::fflush(stdout);
    return 0;
  }

  Fabric& runtime = *fabric.value();
  const AlternateSetId set_id = AlternateSetId::from_value(arg_u64(argc, argv, "--set", 1));
  const auto paths = static_cast<std::size_t>(arg_u64(argc, argv, "--paths", 4));
  const PathId incumbent = PathId::from_value(arg_u64(argc, argv, "--incumbent", 1));

  if (*cmd == "hold") {
    std::printf("HELD epoch=%llu boot=%s\n",
                static_cast<unsigned long long>(runtime.identity().epoch.value()),
                id_hex(runtime.identity().boot).c_str());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::hours(1));
    return 0;
  }

  if (*cmd == "prepare") {
    auto prepared = prepare(runtime, set_id, paths, incumbent);
    if (!prepared.ok()) {
      std::fprintf(stderr, "prepare: %s\n", prepared.status().describe().c_str());
      return 4;
    }
    std::printf("PREPARED set=%llu generation=%llu paths=%zu\n",
                static_cast<unsigned long long>(prepared.value().set.value()),
                static_cast<unsigned long long>(prepared.value().set_generation.value()),
                prepared.value().paths.size());
    std::fflush(stdout);
    return 0;
  }

  if (*cmd == "verify") {
    print_summary(runtime);
    const std::uint64_t probe_set = arg_u64(argc, argv, "--probe-set", 0);
    if (probe_set != 0) {
      auto probe = runtime.begin_promotion(
          PathId::from_value(arg_u64(argc, argv, "--probe-incumbent", 1)),
          IncumbentCondition::failed, AlternateSetId::from_value(probe_set));
      if (!probe.ok()) {
        std::printf("PROBE error %s %s\n", to_string(probe.status().code),
                    to_string(probe.status().reason));
      } else if (probe.value().authorized()) {
        std::printf("PROBE AUTHORIZED selected=%s attempt=%llu\n",
                    id_hex(probe.value().grant.path).c_str(),
                    static_cast<unsigned long long>(probe.value().grant.attempt.value()));
      } else {
        std::printf("PROBE refused %s %s\n",
                    to_string(probe.value().decision.outcome),
                    to_string(probe.value().decision.reason));
      }
    }
    std::fflush(stdout);
    return 0;
  }

  if (*cmd == "late-effect") {
    const PathId path = PathId::from_value(arg_u64(argc, argv, "--path", 1000));
    EffectEvidence effect;
    effect.id = EvidenceId::from_value(path.value() + 500000);
    effect.path = path;
    effect.observation_seq = 77;
    effect.effect_verified = true;
    effect.observed_under = runtime.authority();
    effect.provenance = Provenance::synthetic;
    const Status status = runtime.record_effect(AttemptId::from_value(arg_u64(argc, argv, "--attempt", 1)),
                                                effect);
    std::printf("LATE-EFFECT %s %s\n", to_string(status.code), to_string(status.reason));
    std::fflush(stdout);
    return 0;
  }

  if (*cmd == "promote") {
    auto prepared = prepare(runtime, set_id, paths, incumbent);
    if (!prepared.ok()) {
      std::fprintf(stderr, "prepare: %s\n", prepared.status().describe().c_str());
      return 4;
    }
    const std::string crash_stage = arg_or(argc, argv, "--crash", "none");
    auto authorization =
        runtime.begin_promotion(incumbent, IncumbentCondition::failed, set_id);
    if (!authorization.ok()) {
      std::printf("PROMOTE error %s %s\n", to_string(authorization.status().code),
                  to_string(authorization.status().reason));
      std::fflush(stdout);
      return 6;
    }
    if (!authorization.value().authorized()) {
      std::printf("PROMOTE refused %s %s\n",
                  to_string(authorization.value().decision.outcome),
                  to_string(authorization.value().decision.reason));
      std::fflush(stdout);
      return 0;
    }
    const AttemptId attempt = authorization.value().grant.attempt;
    const PathId selected = authorization.value().grant.path;
    std::printf("GRANTED attempt=%llu selected=%s\n",
                static_cast<unsigned long long>(attempt.value()), id_hex(selected).c_str());
    std::fflush(stdout);
    if (crash_stage == "after-grant") {
      crash_now();
    }

    EffectEvidence effect;
    effect.id = EvidenceId::from_value(selected.value() + 900000);
    effect.path = selected;
    effect.observation_seq = 2;
    effect.effect_verified = true;
    effect.observed_under = runtime.authority();
    effect.provenance = Provenance::synthetic;
    const Status status = runtime.record_effect(attempt, effect);
    if (!status.ok()) {
      std::printf("EFFECT failed %s %s\n", to_string(status.code), to_string(status.reason));
      std::fflush(stdout);
      return 5;
    }
    if (crash_stage == "after-effect" || crash_stage == "before-ack") {
      crash_now();
    }
    std::printf("PROMOTED attempt=%llu selected=%s\n",
                static_cast<unsigned long long>(attempt.value()), id_hex(selected).c_str());
    std::fflush(stdout);
    return 0;
  }

  std::fprintf(stderr, "pff_host: unknown command %s\n", cmd->c_str());
  return 2;
}
int main(int argc, char** argv) {
  try {
    return run_main(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "pff_host: fatal: %s\n", error.what());
    return 70;
  } catch (...) {
    std::fprintf(stderr, "pff_host: fatal: unknown error\n");
    return 70;
  }
}

