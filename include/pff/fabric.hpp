// Path Failover Fabric - the promotion coordinator.
//
// Fabric owns exactly one responsibility: deciding which already-supplied,
// already-legal alternate path may be promoted right now, and carrying that one
// transition through authorisation, verified effect, fencing, rollback and
// durable lineage.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "pff/decision.hpp"
#include "pff/model.hpp"
#include "pff/records.hpp"
#include "pff/selector.hpp"
#include "pff/store.hpp"
#include "pff/version.hpp"

namespace pff {

struct FabricOptions {
  std::filesystem::path directory;
  Provenance provenance = Provenance::synthetic;
  std::uint32_t max_retained_attempts = 256;
  bool enable_compaction = true;
  std::uint64_t compaction_journal_bytes = limits::max_journal_bytes;
};

// A grant is positive authority to apply one transition. It is not an
// acknowledgement and not a verified effect.
struct Grant {
  GrantId id;
  AttemptId attempt;
  PathId path;
  AuthorityVector basis;
  AuthorityKind kind = AuthorityKind::grant;
};

// The answer to a promotion request: the decision document plus, when the
// decision carries positive authority, the grant that authorises the transition.
// A refusal is a normal, fully described outcome rather than an error.
struct PromotionAuthorization {
  PromotionDecision decision;
  Grant grant;

  [[nodiscard]] bool authorized() const noexcept {
    return decision.outcome == PromotionOutcome::authorized && decision.selected.valid();
  }
};

class Fabric {
 public:
  static Result<std::unique_ptr<Fabric>> open(const FabricOptions& options);
  ~Fabric();
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;

  [[nodiscard]] FabricIdentity identity() const;
  [[nodiscard]] AuthorityVector authority() const;
  [[nodiscard]] FabricStats stats() const;
  [[nodiscard]] const RecoveryReport& recovery() const noexcept;
  [[nodiscard]] bool policy_installed() const;
  [[nodiscard]] bool obligations_installed() const;
  [[nodiscard]] std::size_t alternate_set_count() const;
  [[nodiscard]] std::size_t active_attempt_count() const;

  // ---- ingest (all inputs are untrusted and validated) --------------------
  Status apply_policy(const FailoverPolicy& policy);
  Status apply_obligations(const ServiceObligations& obligations);
  Status apply_alternate_set(const AlternateSet& set);
  Status apply_evidence(const PathEvidence& evidence);
  // Advances topology / path-authority generations and fences whatever the new
  // vector no longer covers.
  Status set_authority(const AuthorityVector& authority);
  Status fence_path(PathId path, Reason reason);
  Status fence_all(Reason reason);

  // ---- decisions ----------------------------------------------------------
  // A recommendation: it authorises nothing.
  Result<PromotionDecision> evaluate(PathId incumbent, IncumbentCondition condition,
                                     AlternateSetId set);
  // Positive authority: claims transition ownership and issues a grant.
  Result<PromotionAuthorization> begin_promotion(PathId incumbent, IncumbentCondition condition,
                                                 AlternateSetId set);
  // Verified effect. The only path to a committed promotion.
  Status record_effect(AttemptId attempt, const EffectEvidence& effect);
  Status abandon_attempt(AttemptId attempt, Reason reason);
  // Pre-commit undo: gives the transition back to the incumbent after fresh
  // validation of the incumbent.
  Status rollback(AttemptId attempt, const PathEvidence& incumbent_evidence);
  // Post-commit explicit reversion to a previously displaced path.
  Result<PromotionDecision> revert(AttemptId committed_attempt, const PathEvidence& target);

  // ---- queries ------------------------------------------------------------
  Result<AttemptRecord> attempt(AttemptId id) const;
  std::vector<AttemptRecord> attempts() const;
  std::vector<AttemptRecord> active_attempts() const;
  std::vector<LineageEntry> lineage(std::size_t limit) const;
  std::vector<FenceRecord> fences() const;
  Result<AlternateSet> alternate_set(AlternateSetId id) const;
  Result<FailoverPolicy> policy() const;
  Result<ServiceObligations> obligations() const;
  Result<PathEvidence> evidence(PathId path) const;

 private:
  Fabric() = default;

  Status append_locked(RecordType type, std::span<const std::byte> payload);
  Status compact_if_needed_locked();
  Status persist_attempt_locked(const AttemptRecord& record, bool is_begin);
  Status persist_lineage_locked(const LineageEntry& entry);
  Status persist_fence_locked(const FenceRecord& fence);
  Status retain_attempt_locked(const AttemptRecord& record);
  Status fence_attempts_locked(Reason reason, AttemptState state,
                               const AuthorityVector& at);
  Status record_fence_locked(AttemptId attempt, PathId path, Reason reason);
  Status validate_target_locked(const PathEvidence& evidence, Reason& reason) const;
  Result<PromotionDecision> select_locked(PathId incumbent, IncumbentCondition condition,
                                          AlternateSetId set, DecisionId decision_id,
                                          AttemptId attempt, AuthorityKind kind);
  std::vector<PathId> fenced_path_ids_locked() const;
  void sync_attempts_locked();
  DurableState snapshot_state_locked() const;
  AttemptRecord* find_attempt_locked(AttemptId id);

  mutable std::mutex mu_;

  std::unique_ptr<Store> store_;
  FabricIdentity identity_;
  FabricOptions options_;

  DurableState state_;
  std::map<AlternateSetId, AlternateSet> sets_;
  std::map<PathId, PathEvidence> evidence_;
  std::map<AttemptId, AttemptRecord> attempts_;
  std::map<PathId, Reason> path_fences_;

  AttemptId active_attempt_;
  std::map<AttemptId, Grant> grants_;
  FabricStats stats_;
  std::uint64_t evidence_sequence_ = 0;
};

}  // namespace pff
