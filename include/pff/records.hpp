// Path Failover Fabric - durable record shapes.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <vector>

#include "pff/authority.hpp"
#include "pff/decision.hpp"
#include "pff/ids.hpp"
#include "pff/model.hpp"
#include "pff/status.hpp"

namespace pff {

enum class AttemptState : std::uint8_t {
  claimed = 0,          // transition ownership taken, nothing authorised yet
  authorized = 1,       // positive authority exists; effect not yet verified
  effect_recorded = 2,  // verified effect observed, durable commit pending
  committed = 3,        // transition complete and durable
  refused = 4,          // decision produced no positive authority
  fenced = 5,           // authority-bearing dependency changed; grant revoked
  superseded = 6,       // a newer attempt took transition ownership
  abandoned = 7,        // owner gave the transition back without committing
  rolled_back = 8,      // pre-commit undo completed with fresh incumbent validation
  interrupted = 9,      // process died while this attempt was active
};

const char* to_string(AttemptState s) noexcept;
[[nodiscard]] bool is_terminal_state(AttemptState s) noexcept;
[[nodiscard]] bool is_active_state(AttemptState s) noexcept;
[[nodiscard]] bool is_grant_state(AttemptState s) noexcept;

struct AttemptRecord {
  AttemptId id;
  Epoch epoch;
  BootId boot;
  PathId incumbent;
  PathId selected;
  AlternateSetId set;
  Generation set_generation;
  AuthorityVector basis;
  AttemptState state = AttemptState::claimed;
  Reason terminal_reason = Reason::none;
  std::uint64_t begin_sequence = 0;
  std::uint64_t terminal_sequence = 0;
  std::uint32_t revalidations = 0;
};

struct FenceRecord {
  FenceId id;
  Epoch epoch;
  BootId boot;
  AttemptId attempt;   // absent for path-scoped and global fences
  PathId path;         // absent for attempt-scoped fences
  Reason reason = Reason::none;
  AuthorityVector at;
  std::uint64_t sequence = 0;
};

// One completed, durable outcome. Lineage is history: it preserves what happened
// without restoring any freshness, lease or authority on restart.
struct LineageEntry {
  DecisionId decision;
  AttemptId attempt;
  Epoch epoch;
  BootId boot;
  PromotionOutcome outcome = PromotionOutcome::unsupported;
  PathId incumbent;
  PathId selected;
  AlternateSetId set;
  AuthorityVector basis;
  Reason reason = Reason::none;
  std::uint64_t sequence = 0;
};

// Everything the runtime is allowed to restore after a restart. Dynamic
// liveness, evidence freshness, leases and in-flight authority are deliberately
// absent: they must be re-established by the current incarnation.
struct DurableState {
  AuthorityVector authority;
  bool has_policy = false;
  FailoverPolicy policy;
  bool has_obligations = false;
  ServiceObligations obligations;
  std::vector<AlternateSet> sets;
  std::vector<FenceRecord> fences;
  std::vector<AttemptRecord> attempts;
  std::vector<LineageEntry> lineage;

  std::uint64_t attempt_sequence = 0;
  std::uint64_t fence_sequence = 0;
  std::uint64_t decision_sequence = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t restart_count = 0;

  bool lineage_truncated = false;
  std::uint64_t fence_overflow = 0;
  std::uint64_t attempts_dropped = 0;
};

// Process/incarnation boundary. A restart always produces a new incarnation and
// boot identity and advances the coordinator epoch, so authority issued before
// the restart can never be mistaken for authority after it.
struct FabricIdentity {
  IncarnationId incarnation;
  BootId boot;
  Epoch epoch;
  std::uint64_t process_id = 0;
  std::uint64_t start_unix_micros = 0;
  std::uint64_t restart_count = 0;
};

struct FabricStats {
  std::uint64_t promotions_authorized = 0;
  std::uint64_t promotions_committed = 0;
  std::uint64_t promotions_refused = 0;
  std::uint64_t promotions_indeterminate = 0;
  std::uint64_t fences_recorded = 0;
  std::uint64_t stale_completions_rejected = 0;
  std::uint64_t rollbacks = 0;
  std::uint64_t reversions = 0;
  std::uint64_t revalidations = 0;
  std::uint64_t journal_records = 0;
  std::uint64_t snapshots_written = 0;
  std::uint64_t restarts = 0;
  std::uint64_t evidence_accepted = 0;
  std::uint64_t evidence_rejected = 0;
  std::uint64_t attempts_dropped = 0;
};

struct RecoveryReport {
  bool snapshot_loaded = false;
  std::uint64_t snapshot_sequence = 0;
  std::size_t records_replayed = 0;
  std::size_t records_skipped = 0;
  bool torn_tail_present = false;
  bool torn_tail_recovered = false;
  std::uint64_t truncated_bytes = 0;
  bool inspection_only = false;
  bool journal_absent = false;
  std::uint64_t journal_bytes = 0;
};

}  // namespace pff
