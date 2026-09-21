// Path Failover Fabric - quickstart example.
//
// Demonstrates the product defining question end to end against the installed
// library: given a failed incumbent, an authoritative ordered alternate set,
// service obligations, policy and exact generations, which alternate may be
// promoted now, why, and what happens when the answer is not provable.
//
// The whole scenario is SYNTHETIC: no physical switch, NIC or RDMA fabric is
// involved. Every input is labelled with its provenance.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <filesystem>
#include <string>

#include "pff/fabric.hpp"

namespace {

using namespace pff;

void print_decision(const PromotionDecision& decision) {
  std::printf("  outcome       : %s\n", to_string(decision.outcome));
  std::printf("  authority kind: %s\n", to_string(decision.authority_kind));
  std::printf("  reason        : %s\n", to_string(decision.reason));
  std::printf("  selected      : %s\n",
              decision.selected.valid() ? id_hex(decision.selected).c_str() : "(none)");
  std::printf("  examined      : %zu of %zu supplied\n", decision.examined, decision.supplied);
  std::printf("  evidence      : %s\n", to_string(decision.provenance));
  for (const CandidateAssessment& assessment : decision.ranked) {
    std::printf("    path %s rank=%u score=%lld -> %s (%s, authority %s)\n",
                id_hex(assessment.path).c_str(), assessment.upstream_rank,
                static_cast<long long>(assessment.terms.total), to_string(assessment.verdict),
                to_string(assessment.reason), to_string(assessment.authority));
  }
}

AlternateCandidate make_candidate(PathId path, std::uint32_t rank, const AuthorityVector& authority,
                                  std::int64_t health, Eligibility eligibility) {
  AlternateCandidate candidate;
  candidate.path = path;
  candidate.upstream_rank = rank;
  candidate.authority = authority;
  candidate.evidence.id = EvidenceId::from_value(path.value());
  candidate.evidence.path = path;
  candidate.evidence.freshness = Freshness::fresh;
  candidate.evidence.eligibility = eligibility;
  candidate.evidence.reachability = Reachability::reachable;
  candidate.evidence.capacity = CapacityClass::sufficient;
  candidate.evidence.capabilities = 0x3u;
  candidate.evidence.health_ppm = health;
  candidate.evidence.cost_units = 100;
  candidate.evidence.observation_seq = 1;
  candidate.evidence.observed_under = authority;
  candidate.evidence.provenance = Provenance::synthetic;
  return candidate;
}

}  // namespace

int main() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "pff-quickstart-state";
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);

  FabricOptions options;
  options.directory = directory;
  options.provenance = Provenance::synthetic;

  auto fabric_result = Fabric::open(options);
  if (!fabric_result.ok()) {
    std::printf("cannot open runtime: %s\n", fabric_result.status().describe().c_str());
    return 1;
  }
  Fabric& fabric = *fabric_result.value();
  const FabricIdentity identity = fabric.identity();
  std::printf("runtime epoch=%llu boot=%s\n",
              static_cast<unsigned long long>(identity.epoch.value()),
              id_hex(identity.boot).c_str());

  // The adjacent systems supply the generations this runtime must bind to.
  AuthorityVector advance;
  advance.topology = Generation::from_value(1);
  advance.path_authority = Generation::from_value(1);
  if (!fabric.set_authority(advance).ok()) {
    return 1;
  }
  FailoverPolicy policy;
  policy.generation = Generation::from_value(1);
  policy.w_health = 4;
  policy.w_cost = 1;
  policy.w_rank = 1;
  if (!fabric.apply_policy(policy).ok()) {
    return 1;
  }
  ServiceObligations obligations;
  obligations.generation = Generation::from_value(1);
  obligations.required_capabilities = 0x3u;
  obligations.min_health_ppm = 500000;
  obligations.max_cost_units = 5000000;
  if (!fabric.apply_obligations(obligations).ok()) {
    return 1;
  }

  // The upstream path authority declares an ordered alternate set. The fabric
  // never invents candidates; it decides which supplied member may be promoted.
  const AuthorityVector authority = fabric.authority();
  const PathId incumbent = PathId::from_value(1);
  const AlternateSetId set_id = AlternateSetId::from_value(7);

  AlternateSet set;
  set.id = set_id;
  set.generation = Generation::from_value(1);
  set.completeness = SetCompleteness::complete;
  // Highest upstream rank first, but the fabric recomputes a canonical order.
  set.candidates.push_back(make_candidate(PathId::from_value(400), 3, authority, 990000,
                                          Eligibility::eligible));
  set.candidates.push_back(make_candidate(PathId::from_value(200), 0, authority, 800000,
                                          Eligibility::eligible));
  set.candidates.push_back(make_candidate(PathId::from_value(300), 1, authority, 990000,
                                          Eligibility::eligible));
  if (!fabric.apply_alternate_set(set).ok()) {
    return 1;
  }
  for (const AlternateCandidate& candidate : set.candidates) {
    static_cast<void>(fabric.apply_evidence(candidate.evidence));
  }

  std::printf("\n1. recommendation only (authorises nothing):\n");
  auto evaluation = fabric.evaluate(incumbent, IncumbentCondition::failed, set_id);
  if (!evaluation.ok()) {
    return 1;
  }
  print_decision(evaluation.value());

  std::printf("\n2. positive authority for exactly one transition:\n");
  auto authorization = fabric.begin_promotion(incumbent, IncumbentCondition::failed, set_id);
  if (!authorization.ok()) {
    std::printf("  error: %s\n", authorization.status().describe().c_str());
    return 1;
  }
  if (!authorization.value().authorized()) {
    std::printf("  no positive authority: %s\n",
                to_string(authorization.value().decision.outcome));
    return 1;
  }
  const Grant grant = authorization.value().grant;
  std::printf("  grant %s -> path %s (attempt %llu)\n", id_hex(grant.id).c_str(),
              id_hex(grant.path).c_str(),
              static_cast<unsigned long long>(grant.attempt.value()));

  std::printf("\n3. only a verified effect completes the transition:\n");
  EffectEvidence effect;
  effect.id = EvidenceId::from_value(9001);
  effect.path = grant.path;
  effect.observation_seq = 2;
  effect.effect_verified = true;
  effect.observed_under = fabric.authority();
  const Status committed = fabric.record_effect(grant.attempt, effect);
  std::printf("  record_effect: %s\n", committed.describe().c_str());

  std::printf("\n4. durable lineage:\n");
  for (const LineageEntry& entry : fabric.lineage(8)) {
    std::printf("  seq=%llu outcome=%s attempt=%llu selected=%s\n",
                static_cast<unsigned long long>(entry.sequence), to_string(entry.outcome),
                static_cast<unsigned long long>(entry.attempt.value()),
                id_hex(entry.selected).c_str());
  }

  std::printf("\n5. an alternate whose eligibility cannot be established:\n");
  const AuthorityVector current = fabric.authority();
  AlternateSet unknown_set;
  unknown_set.id = AlternateSetId::from_value(8);
  unknown_set.generation = Generation::from_value(1);
  unknown_set.completeness = SetCompleteness::complete;
  unknown_set.candidates.push_back(make_candidate(PathId::from_value(500), 0, current, 999000,
                                                  Eligibility::unknown));
  unknown_set.candidates.push_back(make_candidate(PathId::from_value(600), 1, current, 900000,
                                                  Eligibility::eligible));
  static_cast<void>(fabric.apply_alternate_set(unknown_set));
  auto unknown = fabric.evaluate(PathId::from_value(1), IncumbentCondition::failed,
                                 AlternateSetId::from_value(8));
  if (unknown.ok()) {
    print_decision(unknown.value());
  }

  std::printf("\n6. the same set with an explicitly configured skip policy:\n");
  FailoverPolicy skip_policy = policy;
  skip_policy.generation = Generation::from_value(2);
  skip_policy.allow_unknown_skip = true;
  static_cast<void>(fabric.apply_policy(skip_policy));
  // A policy change is authority bearing: the previous alternate set was
  // declared under the previous policy generation, so the upstream path
  // authority re-supplies it under the current generations.
  AlternateSet resupplied = unknown_set;
  resupplied.generation = Generation::from_value(2);
  for (AlternateCandidate& candidate : resupplied.candidates) {
    candidate.authority = fabric.authority();
    candidate.evidence.observed_under = fabric.authority();
  }
  static_cast<void>(fabric.apply_alternate_set(resupplied));
  for (const AlternateCandidate& candidate : resupplied.candidates) {
    static_cast<void>(fabric.apply_evidence(candidate.evidence));
  }
  auto skipped = fabric.evaluate(PathId::from_value(1), IncumbentCondition::failed,
                                 AlternateSetId::from_value(8));
  if (skipped.ok()) {
    print_decision(skipped.value());
  }

  static_cast<void>(fabric.fence_path(PathId::from_value(600), Reason::path_fenced));
  std::printf("\n7. after fencing the remaining eligible member:\n");
  auto fenced = fabric.evaluate(PathId::from_value(1), IncumbentCondition::failed,
                                AlternateSetId::from_value(8));
  if (fenced.ok()) {
    print_decision(fenced.value());
  }

  std::filesystem::remove_all(directory, ec);
  return 0;
}
