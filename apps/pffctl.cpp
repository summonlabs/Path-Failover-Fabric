// pffctl - Path Failover Fabric command line client and offline inspector.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "args.hpp"
#include "pff/fabric.hpp"
#include "pff/protocol.hpp"
#include "pff/server.hpp"
#include "pff/store.hpp"

namespace {

using namespace pff;

void print_decision(const PromotionDecision& decision) {
  std::printf("outcome=%s\n", to_string(decision.outcome));
  std::printf("authority_kind=%s\n", to_string(decision.authority_kind));
  std::printf("reason=%s\n", to_string(decision.reason));
  std::printf("incumbent=%s\n", id_hex(decision.incumbent).c_str());
  std::printf("selected=%s\n",
              decision.selected.valid() ? id_hex(decision.selected).c_str() : "none");
  std::printf("examined=%zu/%zu search_limited=%d set_complete=%d\n", decision.examined,
              decision.supplied, decision.search_limited ? 1 : 0, decision.set_complete ? 1 : 0);
  std::printf("digest=%016llx\n",
              static_cast<unsigned long long>(decision_digest(decision)));
  for (const CandidateAssessment& assessment : decision.ranked) {
    std::printf("candidate path=%s rank=%u score=%lld verdict=%s reason=%s authority=%s\n",
                id_hex(assessment.path).c_str(), assessment.upstream_rank,
                static_cast<long long>(assessment.terms.total),
                to_string(assessment.verdict), to_string(assessment.reason),
                to_string(assessment.authority));
  }
}

int inspect_offline(const std::string& directory) {
  StoreOptions options;
  options.directory = directory;
  options.inspection_only = true;
  options.create_directory = false;
  auto store = Store::open(options);
  if (!store.ok()) {
    std::fprintf(stderr, "inspect: %s\n", store.status().describe().c_str());
    return 3;
  }
  auto state = store.value()->load();
  if (!state.ok()) {
    std::fprintf(stderr, "inspect: %s\n", state.status().describe().c_str());
    return 4;
  }
  const DurableState& loaded = state.value();
  const RecoveryReport& recovery = store.value()->recovery();
  std::printf("epoch=%llu boot=%s topology=%llu path_authority=%llu policy=%llu obligations=%llu\n",
              static_cast<unsigned long long>(loaded.authority.epoch.value()),
              id_hex(loaded.authority.boot).c_str(),
              static_cast<unsigned long long>(loaded.authority.topology.value()),
              static_cast<unsigned long long>(loaded.authority.path_authority.value()),
              static_cast<unsigned long long>(loaded.authority.policy.value()),
              static_cast<unsigned long long>(loaded.authority.obligations.value()));
  std::printf("restarts=%llu last_sequence=%llu policy=%d obligations=%d sets=%zu fences=%zu\n",
              static_cast<unsigned long long>(loaded.restart_count),
              static_cast<unsigned long long>(loaded.last_sequence), loaded.has_policy ? 1 : 0,
              loaded.has_obligations ? 1 : 0, loaded.sets.size(), loaded.fences.size());
  std::printf("snapshot_loaded=%d snapshot_sequence=%llu replayed=%zu skipped=%zu torn=%d\n",
              recovery.snapshot_loaded ? 1 : 0,
              static_cast<unsigned long long>(recovery.snapshot_sequence),
              recovery.records_replayed, recovery.records_skipped,
              recovery.torn_tail_present ? 1 : 0);
  for (const AttemptRecord& attempt : loaded.attempts) {
    std::printf("attempt id=%llu state=%s reason=%s selected=%s epoch=%llu\n",
                static_cast<unsigned long long>(attempt.id.value()), to_string(attempt.state),
                to_string(attempt.terminal_reason), id_hex(attempt.selected).c_str(),
                static_cast<unsigned long long>(attempt.epoch.value()));
  }
  for (const LineageEntry& entry : loaded.lineage) {
    std::printf("lineage seq=%llu outcome=%s attempt=%llu selected=%s reason=%s\n",
                static_cast<unsigned long long>(entry.sequence), to_string(entry.outcome),
                static_cast<unsigned long long>(entry.attempt.value()),
                id_hex(entry.selected).c_str(), to_string(entry.reason));
  }
  return 0;
}

}  // namespace

int run_main(int argc, char** argv) {
  using namespace pff::tools;

  const auto cmd = command(argc, argv);
  if (!cmd.has_value()) {
    std::fprintf(stderr,
                 "usage: pffctl [--host H] [--port N] <command> [args]\n"
                 "  status\n"
                 "  lineage [--limit N]\n"
                 "  evaluate --incumbent I --set S\n"
                 "  promote  --incumbent I --set S\n"
                 "  fence    --path P\n"
                 "  shutdown\n"
                 "  inspect  --dir D   (offline, read-only)\n");
    return 2;
  }

  if (*cmd == "inspect") {
    const auto directory = arg_value(argc, argv, "--dir");
    if (!directory.has_value()) {
      std::fprintf(stderr, "inspect requires --dir\n");
      return 2;
    }
    return inspect_offline(*directory);
  }

  ClientOptions options;
  options.host = arg_or(argc, argv, "--host", "127.0.0.1");
  options.port = static_cast<std::uint16_t>(arg_u64(argc, argv, "--port", 0));
  options.name = "pffctl";
  options.nonce = arg_u64(argc, argv, "--nonce", 0);

  auto client = Client::connect(options);
  if (!client.ok()) {
    std::fprintf(stderr, "pffctl: %s\n", client.status().describe().c_str());
    return 3;
  }

  const HelloResponse& hello = client.value()->hello();
  std::printf("session=%s epoch=%llu boot=%s incarnation=%s\n",
              id_hex(hello.session).c_str(),
              static_cast<unsigned long long>(hello.epoch.value()), id_hex(hello.boot).c_str(),
              id_hex(hello.incarnation).c_str());

  if (*cmd == "status") {
    auto status = client.value()->status();
    if (!status.ok()) {
      std::fprintf(stderr, "status: %s\n", status.status().describe().c_str());
      return 4;
    }
    std::printf("policy_installed=%d obligations_installed=%d sets=%llu active_attempts=%llu\n",
                status.value().policy_installed ? 1 : 0,
                status.value().obligations_installed ? 1 : 0,
                static_cast<unsigned long long>(status.value().alternate_sets),
                static_cast<unsigned long long>(status.value().active_attempts));
    std::printf("authorized=%llu committed=%llu refused=%llu indeterminate=%llu fences=%llu\n",
                static_cast<unsigned long long>(status.value().stats.promotions_authorized),
                static_cast<unsigned long long>(status.value().stats.promotions_committed),
                static_cast<unsigned long long>(status.value().stats.promotions_refused),
                static_cast<unsigned long long>(status.value().stats.promotions_indeterminate),
                static_cast<unsigned long long>(status.value().stats.fences_recorded));
    return 0;
  }
  if (*cmd == "lineage") {
    auto lineage = client.value()->lineage(static_cast<std::size_t>(arg_u64(argc, argv, "--limit", 32)));
    if (!lineage.ok()) {
      std::fprintf(stderr, "lineage: %s\n", lineage.status().describe().c_str());
      return 4;
    }
    for (const LineageEntry& entry : lineage.value().entries) {
      std::printf("seq=%llu outcome=%s attempt=%llu incumbent=%s selected=%s reason=%s\n",
                  static_cast<unsigned long long>(entry.sequence), to_string(entry.outcome),
                  static_cast<unsigned long long>(entry.attempt.value()),
                  id_hex(entry.incumbent).c_str(), id_hex(entry.selected).c_str(),
                  to_string(entry.reason));
    }
    std::printf("entries=%zu truncated=%d\n", lineage.value().entries.size(),
                lineage.value().truncated ? 1 : 0);
    return 0;
  }
  if (*cmd == "shutdown") {
    const Status status = client.value()->shutdown();
    if (!status.ok()) {
      std::fprintf(stderr, "shutdown: %s\n", status.describe().c_str());
      return 4;
    }
    std::printf("stopping\n");
    return 0;
  }

  PromotionRequest request;
  request.incumbent = PathId::from_value(arg_u64(argc, argv, "--incumbent", 0));
  request.set = AlternateSetId::from_value(arg_u64(argc, argv, "--set", 0));
  request.condition = IncumbentCondition::failed;
  const AuthorityVector authority = client.value()->hello().authority;
  request.policy_generation = authority.policy;
  request.obligations_generation = authority.obligations;
  request.topology_generation = authority.topology;
  request.path_authority_generation = authority.path_authority;
  request.epoch = authority.epoch;
  request.boot = authority.boot;

  if (*cmd == "evaluate" || *cmd == "promote") {
    if (*cmd == "evaluate") {
      auto decision = client.value()->evaluate(request);
      if (!decision.ok()) {
        std::fprintf(stderr, "evaluate: %s\n", decision.status().describe().c_str());
        return 4;
      }
      print_decision(decision.value());
      return 0;
    }
    auto response = client.value()->promote(request);
    if (!response.ok()) {
      std::fprintf(stderr, "promote: %s\n", response.status().describe().c_str());
      return 4;
    }
    print_decision(response.value().decision);
    std::printf("grant=%s attempt=%llu\n", id_hex(response.value().grant).c_str(),
                static_cast<unsigned long long>(response.value().attempt.value()));
    return 0;
  }
  if (*cmd == "fence") {
    const PathId path = PathId::from_value(arg_u64(argc, argv, "--path", 0));
    const Status status = client.value()->fence_path(path, Reason::path_fenced);
    if (!status.ok()) {
      std::fprintf(stderr, "fence: %s\n", status.describe().c_str());
      return 4;
    }
    std::printf("fenced=%s\n", id_hex(path).c_str());
    return 0;
  }

  std::fprintf(stderr, "pffctl: unknown command %s\n", cmd->c_str());
  return 2;
}
int main(int argc, char** argv) {
  try {
    return run_main(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "pffctl: fatal: %s\n", error.what());
    return 70;
  } catch (...) {
    std::fprintf(stderr, "pffctl: fatal: unknown error\n");
    return 70;
  }
}

