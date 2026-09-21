// Downstream consumer of the installed Path Failover Fabric package.
//
// It only uses the installed public headers and the installed library, so a
// successful build and run proves that the exported package is usable by an
// independent project. The scenario is SYNTHETIC.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <filesystem>
#include <string>

#include <pff/fabric.hpp>
#include <pff/version.hpp>

int main() {
  using namespace pff;

  static_assert(version_major == 1, "consumer built against an unexpected major version");
  std::printf("consumer: linked against Path Failover Fabric %s (%u)\n", version_string,
              static_cast<unsigned>(version_number));

  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "pff-consumer-state";
  std::error_code ec;
  std::filesystem::remove_all(directory, ec);

  FabricOptions options;
  options.directory = directory;
  auto fabric_result = Fabric::open(options);
  if (!fabric_result.ok()) {
    std::printf("consumer: cannot open runtime: %s\n",
                fabric_result.status().describe().c_str());
    return 1;
  }
  Fabric& fabric = *fabric_result.value();

  AuthorityVector advance;
  advance.topology = Generation::from_value(1);
  advance.path_authority = Generation::from_value(1);
  if (!fabric.set_authority(advance).ok()) {
    return 1;
  }
  FailoverPolicy policy;
  policy.generation = Generation::from_value(1);
  if (!fabric.apply_policy(policy).ok()) {
    return 1;
  }
  ServiceObligations obligations;
  obligations.generation = Generation::from_value(1);
  obligations.required_capabilities = 0x1u;
  if (!fabric.apply_obligations(obligations).ok()) {
    return 1;
  }

  const AuthorityVector authority = fabric.authority();
  AlternateSet set;
  set.id = AlternateSetId::from_value(2);
  set.generation = Generation::from_value(1);
  set.completeness = SetCompleteness::complete;
  AlternateCandidate candidate;
  candidate.path = PathId::from_value(77);
  candidate.upstream_rank = 0;
  candidate.authority = authority;
  candidate.evidence.id = EvidenceId::from_value(1);
  candidate.evidence.path = candidate.path;
  candidate.evidence.freshness = Freshness::fresh;
  candidate.evidence.eligibility = Eligibility::eligible;
  candidate.evidence.reachability = Reachability::reachable;
  candidate.evidence.capacity = CapacityClass::sufficient;
  candidate.evidence.capabilities = 0x1u;
  candidate.evidence.health_ppm = 999000;
  candidate.evidence.cost_units = 10;
  candidate.evidence.observed_under = authority;
  candidate.evidence.provenance = Provenance::synthetic;
  set.candidates.push_back(candidate);
  if (!fabric.apply_alternate_set(set).ok()) {
    return 1;
  }
  if (!fabric.apply_evidence(candidate.evidence).ok()) {
    return 1;
  }

  auto authorization =
      fabric.begin_promotion(PathId::from_value(1), IncumbentCondition::failed, set.id);
  if (!authorization.ok() || !authorization.value().authorized()) {
    std::printf("consumer: promotion was not authorised\n");
    return 1;
  }
  EffectEvidence effect;
  effect.id = EvidenceId::from_value(2);
  effect.path = authorization.value().grant.path;
  effect.observation_seq = 2;
  effect.effect_verified = true;
  effect.observed_under = fabric.authority();
  if (!fabric.record_effect(authorization.value().grant.attempt, effect).ok()) {
    std::printf("consumer: verified effect was refused\n");
    return 1;
  }

  std::printf("consumer: promoted path %s via attempt %llu\n",
              id_hex(authorization.value().grant.path).c_str(),
              static_cast<unsigned long long>(authorization.value().grant.attempt.value()));
  for (const LineageEntry& entry : fabric.lineage(4)) {
    std::printf("consumer: lineage %s selected=%s\n", to_string(entry.outcome),
                id_hex(entry.selected).c_str());
  }

  std::filesystem::remove_all(directory, ec);
  return 0;
}
