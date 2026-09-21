// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/fabric.hpp"

#include <algorithm>
#include <new>
#include <utility>

#include "pff/checked.hpp"
#include "pff/wire.hpp"
#include "platform.hpp"

namespace pff {
namespace {

Reason authority_reason(AuthorityClass cls) noexcept {
  switch (cls) {
    case AuthorityClass::current:
      return Reason::authority_current;
    case AuthorityClass::stale:
      return Reason::authority_stale;
    case AuthorityClass::unknown:
      return Reason::authority_unknown;
    case AuthorityClass::conflict:
      return Reason::authority_conflict;
  }
  return Reason::authority_unknown;
}

PromotionOutcome refusal_outcome_for(const AuthorityComparison& comparison) noexcept {
  switch (comparison.overall) {
    case AuthorityClass::current:
      return PromotionOutcome::unsupported;
    case AuthorityClass::conflict:
      return PromotionOutcome::refused_conflict;
    default:
      return PromotionOutcome::refused_stale_authority;
  }
}

}  // namespace

Fabric::~Fabric() = default;

Result<std::unique_ptr<Fabric>> Fabric::open(const FabricOptions& options) {
  try {
    if (options.max_retained_attempts == 0 ||
        options.max_retained_attempts > limits::max_attempts) {
      return Status(Code::invalid, Reason::domain_invalid,
                    "max_retained_attempts out of range");
    }
    auto fabric = std::unique_ptr<Fabric>(new Fabric());
    fabric->options_ = options;

    StoreOptions store_options;
    store_options.directory = options.directory;
    auto store = Store::open(store_options);
    if (!store.ok()) {
      return store.status();
    }
    fabric->store_ = std::move(store).value();

    auto loaded = fabric->store_->load();
    if (!loaded.ok()) {
      return loaded.status();
    }
    fabric->state_ = std::move(loaded).value();

    for (const AlternateSet& set : fabric->state_.sets) {
      fabric->sets_.emplace(set.id, set);
    }
    for (const AttemptRecord& attempt : fabric->state_.attempts) {
      fabric->attempts_.emplace(attempt.id, attempt);
    }
    for (const FenceRecord& fence : fabric->state_.fences) {
      if (fence.path.valid()) {
        fabric->path_fences_.emplace(fence.path, fence.reason);
      }
    }
    // Evidence freshness, liveness, leases and in-flight authority are dynamic
    // and are deliberately not restored from durable state.

    const std::uint64_t previous_epoch = fabric->state_.authority.epoch.value();
    if (previous_epoch == UINT64_MAX) {
      return Status(Code::exhausted, Reason::table_full, "coordinator epoch exhausted");
    }
    const Epoch new_epoch = Epoch::from_value(previous_epoch + 1);
    const BootId new_boot = BootId::from_value(platform::random_u64());
    const IncarnationId new_incarnation = IncarnationId::from_value(platform::random_u64());

    fabric->state_.restart_count += 1;
    fabric->state_.authority.epoch = new_epoch;
    fabric->state_.authority.boot = new_boot;

    fabric->identity_.incarnation = new_incarnation;
    fabric->identity_.boot = new_boot;
    fabric->identity_.epoch = new_epoch;
    fabric->identity_.process_id = platform::process_id();
    fabric->identity_.start_unix_micros = platform::unix_micros();
    fabric->identity_.restart_count = fabric->state_.restart_count;
    fabric->stats_.restarts = fabric->state_.restart_count;

    const std::vector<std::byte> epoch_payload =
        encode_epoch_record(fabric->state_.authority, fabric->state_.restart_count,
                            fabric->state_.attempt_sequence, fabric->state_.fence_sequence,
                            fabric->state_.decision_sequence);
    if (epoch_payload.empty()) {
      return Status(Code::invalid, Reason::invalid_payload, "cannot encode restart record");
    }
    Status status = fabric->append_locked(RecordType::epoch_advance, epoch_payload);
    if (!status.ok()) {
      return status;
    }

    // Every attempt that was in flight when the previous incarnation died is
    // fenced and marked interrupted. It can never be resumed or completed.
    std::vector<AttemptId> interrupted;
    for (const auto& [id, attempt] : fabric->attempts_) {
      if (is_active_state(attempt.state)) {
        interrupted.push_back(id);
      }
    }
    for (const AttemptId id : interrupted) {
      AttemptRecord* record = fabric->find_attempt_locked(id);
      if (record == nullptr) {
        continue;
      }
      record->state = AttemptState::interrupted;
      record->terminal_reason = Reason::restart_fenced;
      record->terminal_sequence = fabric->state_.last_sequence;
      status = fabric->persist_attempt_locked(*record, false);
      if (!status.ok()) {
        return status;
      }
      status = fabric->record_fence_locked(id, PathId::absent(), Reason::restart_fenced);
      if (!status.ok()) {
        return status;
      }
      ++fabric->stats_.fences_recorded;
    }
    fabric->active_attempt_ = AttemptId::absent();
    fabric->grants_.clear();
    fabric->sync_attempts_locked();

    status = fabric->compact_if_needed_locked();
    if (!status.ok()) {
      return status;
    }
    return fabric;
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory during open");
  }
}

// ---- helpers (mutex held) --------------------------------------------------

Status Fabric::append_locked(RecordType type, std::span<const std::byte> payload) {
  Status status = store_->append(type, payload);
  if (!status.ok()) {
    return status;
  }
  state_.last_sequence = store_->last_sequence();
  ++stats_.journal_records;
  return status;
}

void Fabric::sync_attempts_locked() {
  state_.attempts.clear();
  state_.attempts.reserve(attempts_.size());
  for (const auto& [id, attempt] : attempts_) {
    state_.attempts.push_back(attempt);
  }
}

AttemptRecord* Fabric::find_attempt_locked(AttemptId id) {
  const auto it = attempts_.find(id);
  return it == attempts_.end() ? nullptr : &it->second;
}

Status Fabric::retain_attempt_locked(const AttemptRecord& record) {
  const auto existing = attempts_.find(record.id);
  if (existing != attempts_.end()) {
    existing->second = record;
    return ok_status();
  }
  if (attempts_.size() >= options_.max_retained_attempts) {
    const auto evictable = std::find_if(attempts_.begin(), attempts_.end(),
                                        [](const auto& entry) {
                                          return is_terminal_state(entry.second.state);
                                        });
    if (evictable == attempts_.end()) {
      return Status(Code::table_full, Reason::table_full, "attempt table full");
    }
    attempts_.erase(evictable);
    ++state_.attempts_dropped;
    ++stats_.attempts_dropped;
  }
  attempts_.emplace(record.id, record);
  return ok_status();
}

Status Fabric::persist_attempt_locked(const AttemptRecord& record, bool is_begin) {
  const std::vector<std::byte> payload = encode_attempt_record(record);
  if (payload.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode attempt record");
  }
  Status status =
      append_locked(is_begin ? RecordType::attempt_begin : RecordType::attempt_state, payload);
  if (!status.ok()) {
    return status;
  }
  state_.attempt_sequence = std::max(state_.attempt_sequence, record.id.value());
  return retain_attempt_locked(record);
}

Status Fabric::persist_lineage_locked(const LineageEntry& entry) {
  const std::vector<std::byte> payload = encode_lineage_record(entry);
  if (payload.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode lineage record");
  }
  Status status = append_locked(RecordType::lineage_entry, payload);
  if (!status.ok()) {
    return status;
  }
  state_.decision_sequence = std::max(state_.decision_sequence, entry.decision.value());
  if (state_.lineage.size() >= limits::max_lineage) {
    state_.lineage.erase(state_.lineage.begin());
    state_.lineage_truncated = true;
  }
  state_.lineage.push_back(entry);
  return status;
}

Status Fabric::persist_fence_locked(const FenceRecord& fence) {
  if (state_.fences.size() >= limits::max_fences) {
    // Fences are authority bearing and are never silently evicted.
    ++state_.fence_overflow;
    return Status(Code::exhausted, Reason::table_full, "fence table full");
  }
  const std::vector<std::byte> payload = encode_fence_record(fence);
  if (payload.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "cannot encode fence record");
  }
  Status status = append_locked(RecordType::fence_recorded, payload);
  if (!status.ok()) {
    return status;
  }
  state_.fences.push_back(fence);
  state_.fence_sequence = std::max(state_.fence_sequence, fence.id.value());
  return status;
}

Status Fabric::record_fence_locked(AttemptId attempt, PathId path, Reason reason) {
  if (state_.fence_sequence == UINT64_MAX) {
    return Status(Code::exhausted, Reason::table_full, "fence sequence exhausted");
  }
  FenceRecord fence;
  fence.id = FenceId::from_value(state_.fence_sequence + 1);
  fence.epoch = identity_.epoch;
  fence.boot = identity_.boot;
  fence.attempt = attempt;
  fence.path = path;
  fence.reason = reason;
  fence.at = state_.authority;
  fence.sequence = state_.last_sequence + 1;
  return persist_fence_locked(fence);
}

std::vector<PathId> Fabric::fenced_path_ids_locked() const {
  std::vector<PathId> paths;
  paths.reserve(path_fences_.size());
  for (const auto& [path, reason] : path_fences_) {
    (void)reason;
    paths.push_back(path);
  }
  return paths;
}

Status Fabric::fence_attempts_locked(Reason reason, AttemptState state,
                                     const AuthorityVector& at) {
  std::vector<AttemptId> targets;
  for (const auto& [id, attempt] : attempts_) {
    if (is_active_state(attempt.state)) {
      targets.push_back(id);
    }
  }
  for (const AttemptId id : targets) {
    AttemptRecord* record = find_attempt_locked(id);
    if (record == nullptr) {
      continue;
    }
    record->state = state;
    record->terminal_reason = reason;
    record->terminal_sequence = state_.last_sequence;
    Status attempt_status = persist_attempt_locked(*record, false);
    if (!attempt_status.ok()) {
      return attempt_status;
    }
    Status fence_status = record_fence_locked(id, PathId::absent(), reason);
    if (!fence_status.ok()) {
      return fence_status;
    }
    ++stats_.fences_recorded;
    (void)at;
  }
  if (!targets.empty()) {
    active_attempt_ = AttemptId::absent();
    for (const AttemptId id : targets) {
      grants_.erase(id);
    }
  }
  return ok_status();
}

Status Fabric::validate_target_locked(const PathEvidence& evidence, Reason& reason) const {
  Status valid = evidence.validate();
  if (!valid.ok()) {
    reason = valid.reason;
    return valid;
  }
  const AuthorityComparison comparison = classify(evidence.observed_under, state_.authority);
  if (comparison.overall != AuthorityClass::current) {
    reason = authority_reason(comparison.overall);
    return Status(Code::stale, reason, "target evidence is not bound to current authority");
  }
  if (path_fences_.count(evidence.path) != 0) {
    reason = Reason::path_fenced;
    return Status(Code::fenced, reason, "target path is fenced");
  }
  if (evidence.freshness != Freshness::fresh) {
    reason = evidence.freshness == Freshness::stale ? Reason::evidence_stale : Reason::evidence_unknown;
    return Status(Code::stale, reason, "target evidence is not fresh");
  }
  if (evidence.eligibility != Eligibility::eligible) {
    reason = evidence.eligibility == Eligibility::ineligible ? Reason::eligibility_ineligible
                                                             : Reason::eligibility_unknown;
    return Status(Code::refused, reason, "target is not proven eligible");
  }
  if (!state_.has_obligations) {
    reason = Reason::obligations_invalid;
    return Status(Code::refused, reason, "no service obligations installed");
  }
  const ServiceObligations& obligations = state_.obligations;
  if (obligations.require_reachable && evidence.reachability != Reachability::reachable) {
    reason = evidence.reachability == Reachability::unreachable ? Reason::reachability_unreachable
                                                               : Reason::reachability_unknown;
    return Status(Code::refused, reason, "target is not proven reachable");
  }
  if (obligations.require_capacity && evidence.capacity != CapacityClass::sufficient) {
    reason = evidence.capacity == CapacityClass::insufficient ? Reason::capacity_insufficient
                                                             : Reason::capacity_unknown;
    return Status(Code::refused, reason, "target capacity is not proven sufficient");
  }
  if ((evidence.capabilities & obligations.required_capabilities) !=
      obligations.required_capabilities) {
    reason = Reason::capability_missing;
    return Status(Code::refused, reason, "target lacks a required capability");
  }
  if (evidence.health_ppm < obligations.min_health_ppm) {
    reason = Reason::health_below_obligation;
    return Status(Code::refused, reason, "target health is below the obligation");
  }
  if (evidence.cost_units > obligations.max_cost_units) {
    reason = Reason::cost_above_obligation;
    return Status(Code::refused, reason, "target cost is above the obligation");
  }
  reason = Reason::authority_current;
  return ok_status();
}

Result<PromotionDecision> Fabric::select_locked(PathId incumbent, IncumbentCondition condition,
                                                AlternateSetId set_id, DecisionId decision_id,
                                                AttemptId attempt, AuthorityKind kind) {
  (void)condition;
  SelectionInput input;
  input.incumbent = incumbent;
  input.current = state_.authority;
  input.decision_id = decision_id;
  input.attempt = attempt;
  input.provenance = options_.provenance;

  PromotionDecision decision;
  decision.id = decision_id;
  decision.incumbent = incumbent;
  decision.basis = state_.authority;
  decision.attempt = attempt;
  decision.provenance = options_.provenance;
  decision.authority_kind = kind;
  decision.set = set_id;

  const auto set_it = sets_.find(set_id);
  if (set_it == sets_.end()) {
    decision.outcome = PromotionOutcome::refused_invalid_input;
    decision.reason = Reason::domain_invalid;
    decision.explanation.emplace_back(Reason::domain_invalid, 0);
    return decision;
  }
  if (!state_.has_policy) {
    decision.outcome = PromotionOutcome::refused_policy_invalid;
    decision.reason = Reason::policy_invalid;
    decision.explanation.emplace_back(Reason::policy_invalid, 0);
    return decision;
  }
  if (!state_.has_obligations) {
    decision.outcome = PromotionOutcome::refused_obligations_invalid;
    decision.reason = Reason::obligations_invalid;
    decision.explanation.emplace_back(Reason::obligations_invalid, 0);
    return decision;
  }

  input.set = &set_it->second;
  input.policy = &state_.policy;
  input.obligations = &state_.obligations;
  input.expected_set_generation = set_it->second.generation;
  const std::vector<PathId> fences = fenced_path_ids_locked();
  input.fenced_paths = std::span<const PathId>(fences.data(), fences.size());

  auto result = Selector::evaluate(input);
  if (!result.ok()) {
    decision.outcome = PromotionOutcome::refused_invalid_input;
    decision.reason = result.status().reason;
    decision.explanation.emplace_back(result.status().reason, 0);
    return decision;
  }
  PromotionDecision out = std::move(result).value();
  out.authority_kind = kind;
  return out;
}

Status Fabric::compact_if_needed_locked() {
  if (!options_.enable_compaction) {
    return ok_status();
  }
  if (store_->journal_bytes() < options_.compaction_journal_bytes) {
    return ok_status();
  }
  return store_->write_snapshot(snapshot_state_locked()) ;
}

DurableState Fabric::snapshot_state_locked() const {
  DurableState snapshot = state_;
  snapshot.last_sequence = state_.last_sequence;
  snapshot.attempts.clear();
  snapshot.attempts.reserve(attempts_.size());
  for (const auto& [id, attempt] : attempts_) {
    snapshot.attempts.push_back(attempt);
  }
  return snapshot;
}

// ---- ingest ----------------------------------------------------------------

Status Fabric::apply_policy(const FailoverPolicy& policy) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    Status valid = policy.validate();
    if (!valid.ok()) {
      return valid;
    }
    if (state_.has_policy) {
      if (policy.generation < state_.policy.generation) {
        return Status(Code::stale, Reason::policy_generation_stale,
                      "policy generation is behind the installed policy");
      }
      if (policy.generation == state_.policy.generation) {
        if (policy == state_.policy) {
          return ok_status();
        }
        return Status(Code::conflict, Reason::generation_conflict,
                      "policy generation reused with different content");
      }
    }
    const std::vector<std::byte> payload = encode_policy_record(policy);
    if (payload.empty()) {
      return Status(Code::invalid, Reason::invalid_payload, "cannot encode policy");
    }
    Status status = append_locked(RecordType::policy_set, payload);
    if (!status.ok()) {
      return status;
    }
    state_.policy = policy;
    state_.has_policy = true;
    state_.authority.policy = policy.generation;
    // A policy generation is authority bearing: attempts bound to the previous
    // generation are fenced rather than reinterpreted under the new policy.
    status = fence_attempts_locked(Reason::authority_fenced, AttemptState::fenced,
                                   state_.authority);
    if (!status.ok()) {
      return status;
    }
    return compact_if_needed_locked();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::apply_obligations(const ServiceObligations& obligations) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    Status valid = obligations.validate();
    if (!valid.ok()) {
      return valid;
    }
    if (state_.has_obligations) {
      if (obligations.generation < state_.obligations.generation) {
        return Status(Code::stale, Reason::obligations_generation_stale,
                      "obligation generation is behind the installed set");
      }
      if (obligations.generation == state_.obligations.generation) {
        if (obligations == state_.obligations) {
          return ok_status();
        }
        return Status(Code::conflict, Reason::generation_conflict,
                      "obligation generation reused with different content");
      }
    }
    const std::vector<std::byte> payload = encode_obligations_record(obligations);
    if (payload.empty()) {
      return Status(Code::invalid, Reason::invalid_payload, "cannot encode obligations");
    }
    Status status = append_locked(RecordType::obligations_set, payload);
    if (!status.ok()) {
      return status;
    }
    state_.obligations = obligations;
    state_.has_obligations = true;
    state_.authority.obligations = obligations.generation;
    status = fence_attempts_locked(Reason::authority_fenced, AttemptState::fenced,
                                   state_.authority);
    if (!status.ok()) {
      return status;
    }
    return compact_if_needed_locked();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::apply_alternate_set(const AlternateSet& set) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    Status valid = set.validate();
    if (!valid.ok()) {
      return valid;
    }
    const auto existing = sets_.find(set.id);
    if (existing != sets_.end()) {
      if (set.generation < existing->second.generation) {
        return Status(Code::stale, Reason::set_generation_mismatch,
                      "alternate set generation is behind the installed set");
      }
      if (set.generation == existing->second.generation) {
        if (set.completeness == existing->second.completeness &&
            set.candidates.size() == existing->second.candidates.size()) {
          bool identical = true;
          for (std::size_t i = 0; i < set.candidates.size(); ++i) {
            const AlternateCandidate& a = set.candidates[i];
            const AlternateCandidate& b = existing->second.candidates[i];
            if (a.path != b.path || a.upstream_rank != b.upstream_rank ||
                a.withdrawn != b.withdrawn) {
              identical = false;
              break;
            }
          }
          if (identical) {
            return ok_status();
          }
        }
        return Status(Code::conflict, Reason::generation_conflict,
                      "alternate set generation reused with different content");
      }
    } else if (sets_.size() >= limits::max_alternate_sets) {
      return Status(Code::exhausted, Reason::table_full, "alternate set table full");
    }

    const std::vector<std::byte> payload = encode_set_record(set);
    if (payload.empty()) {
      return Status(Code::invalid, Reason::invalid_payload, "cannot encode alternate set");
    }
    Status status = append_locked(RecordType::alternate_set_put, payload);
    if (!status.ok()) {
      return status;
    }
    sets_[set.id] = set;
    const auto stored = std::find_if(state_.sets.begin(), state_.sets.end(),
                                     [&set](const AlternateSet& candidate) {
                                       return candidate.id == set.id;
                                     });
    if (stored == state_.sets.end()) {
      state_.sets.push_back(set);
    } else {
      *stored = set;
    }
    return compact_if_needed_locked();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::apply_evidence(const PathEvidence& evidence) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    Status valid = evidence.validate();
    if (!valid.ok()) {
      ++stats_.evidence_rejected;
      return valid;
    }
    const auto existing = evidence_.find(evidence.path);
    if (existing != evidence_.end()) {
      if (evidence.observation_seq <= existing->second.observation_seq) {
        ++stats_.evidence_rejected;
        return Status(Code::stale, Reason::evidence_superseded,
                      "evidence observation sequence is not newer than the stored one");
      }
    } else if (evidence_.size() >= limits::max_evidence_entries) {
      ++stats_.evidence_rejected;
      return Status(Code::exhausted, Reason::table_full, "evidence table full");
    }
    evidence_.insert_or_assign(evidence.path, evidence);
    ++stats_.evidence_accepted;
    (void)lock;
    return ok_status();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::set_authority(const AuthorityVector& authority) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    if (authority.topology < state_.authority.topology ||
        authority.path_authority < state_.authority.path_authority) {
      return Status(Code::stale, Reason::topology_generation_stale,
                    "topology or path authority generation regressed");
    }
    if (authority.epoch.valid() && authority.epoch != state_.authority.epoch) {
      return Status(Code::stale, Reason::epoch_stale,
                    "the coordinator epoch is owned by the runtime, not by callers");
    }
    if (authority.boot.valid() && authority.boot != state_.authority.boot) {
      return Status(Code::stale, Reason::boot_mismatch,
                    "the boot identity is owned by the runtime, not by callers");
    }
    if (authority.topology == state_.authority.topology &&
        authority.path_authority == state_.authority.path_authority) {
      return ok_status();
    }
    state_.authority.topology = authority.topology;
    state_.authority.path_authority = authority.path_authority;
    const std::vector<std::byte> payload =
        encode_epoch_record(state_.authority, state_.restart_count, state_.attempt_sequence,
                            state_.fence_sequence, state_.decision_sequence);
    if (payload.empty()) {
      return Status(Code::invalid, Reason::invalid_payload, "cannot encode authority update");
    }
    Status status = append_locked(RecordType::epoch_advance, payload);
    if (!status.ok()) {
      return status;
    }
    status = fence_attempts_locked(Reason::authority_fenced, AttemptState::fenced,
                                   state_.authority);
    if (!status.ok()) {
      return status;
    }
    return compact_if_needed_locked();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::fence_path(PathId path, Reason reason) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    if (!path.valid()) {
      return Status(Code::invalid, Reason::domain_invalid, "fence path identity absent");
    }
    if (path_fences_.count(path) != 0) {
      return ok_status();
    }
    Status status = record_fence_locked(AttemptId::absent(), path, reason);
    if (!status.ok()) {
      return status;
    }
    path_fences_.emplace(path, reason);
    ++stats_.fences_recorded;
    // A fence revokes authority for any in-flight attempt that targets the path.
    for (auto& [id, attempt] : attempts_) {
      if (is_active_state(attempt.state) && attempt.selected == path) {
        AttemptRecord* record = find_attempt_locked(id);
        record->state = AttemptState::fenced;
        record->terminal_reason = Reason::path_fenced;
        record->terminal_sequence = state_.last_sequence;
        status = persist_attempt_locked(*record, false);
        if (!status.ok()) {
          return status;
        }
        grants_.erase(id);
        if (active_attempt_ == id) {
          active_attempt_ = AttemptId::absent();
        }
      }
    }
    return compact_if_needed_locked();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::fence_all(Reason reason) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    return fence_attempts_locked(reason, AttemptState::fenced, state_.authority);
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

// ---- decisions -------------------------------------------------------------

Result<PromotionDecision> Fabric::evaluate(PathId incumbent, IncumbentCondition condition,
                                          AlternateSetId set) {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_.decision_sequence == UINT64_MAX) {
    return Status(Code::exhausted, Reason::table_full, "decision sequence exhausted");
  }
  const DecisionId decision_id = DecisionId::from_value(state_.decision_sequence + 1);
  auto result = select_locked(incumbent, condition, set, decision_id, AttemptId::absent(),
                              AuthorityKind::recommendation);
  if (!result.ok()) {
    return result.status();
  }
  ++state_.decision_sequence;
  PromotionDecision decision = std::move(result).value();
  decision.authority_kind = AuthorityKind::recommendation;
  return decision;
}

Result<PromotionAuthorization> Fabric::begin_promotion(PathId incumbent,
                                                         IncumbentCondition condition,
                                                         AlternateSetId set) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    if (!incumbent.valid() || !set.valid()) {
      return Status(Code::invalid, Reason::domain_invalid, "promotion request identities absent");
    }
    if (active_attempt_.valid()) {
      const AttemptRecord* active = nullptr;
      const auto it = attempts_.find(active_attempt_);
      if (it != attempts_.end()) {
        active = &it->second;
      }
      if (active != nullptr && is_active_state(active->state)) {
        ++stats_.promotions_refused;
        // Contention is an admission refusal, not an error: it is reported as a
        // decision document so that the caller sees exactly why it lost.
        PromotionAuthorization refusal;
        refusal.decision.id = DecisionId::from_value(state_.decision_sequence + 1);
        refusal.decision.outcome = PromotionOutcome::refused_already_owned;
        refusal.decision.authority_kind = AuthorityKind::recommendation;
        refusal.decision.incumbent = incumbent;
        refusal.decision.set = set;
        refusal.decision.basis = state_.authority;
        refusal.decision.provenance = options_.provenance;
        refusal.decision.reason = Reason::attempt_active;
        refusal.decision.explanation.emplace_back(Reason::attempt_active,
                                                  active_attempt_.value());
        return refusal;
      }
      active_attempt_ = AttemptId::absent();
    }
    if (state_.attempt_sequence == UINT64_MAX || state_.decision_sequence == UINT64_MAX) {
      return Status(Code::exhausted, Reason::table_full, "attempt sequence exhausted");
    }

    const AttemptId attempt_id = AttemptId::from_value(state_.attempt_sequence + 1);
    const DecisionId decision_id = DecisionId::from_value(state_.decision_sequence + 1);

    auto selected = select_locked(incumbent, condition, set, decision_id, attempt_id,
                                  AuthorityKind::grant);
    if (!selected.ok()) {
      return selected.status();
    }
    PromotionDecision decision = std::move(selected).value();
    ++state_.decision_sequence;

    // A structural refusal is not a decision about a candidate surface, so it
    // allocates no attempt and writes no lineage.
    if (decision.outcome == PromotionOutcome::refused_invalid_input ||
        decision.outcome == PromotionOutcome::refused_policy_invalid ||
        decision.outcome == PromotionOutcome::refused_obligations_invalid) {
      ++stats_.promotions_refused;
      PromotionAuthorization refusal;
      refusal.decision = std::move(decision);
      return refusal;
    }

    AttemptRecord record;
    record.id = attempt_id;
    record.epoch = identity_.epoch;
    record.boot = identity_.boot;
    record.incumbent = incumbent;
    record.selected = decision.selected;
    record.set = set;
    record.set_generation = decision.set_generation;
    record.basis = state_.authority;
    record.state = AttemptState::claimed;
    record.terminal_reason = Reason::none;
    record.begin_sequence = state_.last_sequence + 1;
    record.terminal_sequence = 0;

    Status status = persist_attempt_locked(record, true);
    if (!status.ok()) {
      return status;
    }
    ++state_.attempt_sequence;

    if (decision.outcome != PromotionOutcome::authorized || !decision.selected.valid()) {
      record.state = AttemptState::refused;
      record.terminal_reason = decision.reason;
      record.terminal_sequence = state_.last_sequence;
      status = persist_attempt_locked(record, false);
      if (!status.ok()) {
        return status;
      }
      LineageEntry entry;
      entry.decision = decision_id;
      entry.attempt = attempt_id;
      entry.epoch = identity_.epoch;
      entry.boot = identity_.boot;
      entry.outcome = decision.outcome;
      entry.incumbent = incumbent;
      entry.selected = PathId::absent();
      entry.set = set;
      entry.basis = state_.authority;
      entry.reason = decision.reason;
      entry.sequence = state_.last_sequence;
      status = persist_lineage_locked(entry);
      if (!status.ok()) {
        return status;
      }
      if (is_indeterminate(decision.outcome)) {
        ++stats_.promotions_indeterminate;
      } else {
        ++stats_.promotions_refused;
      }
      status = compact_if_needed_locked();
      if (!status.ok()) {
        return status;
      }
      PromotionAuthorization refusal;
      refusal.decision = std::move(decision);
      return refusal;
    }

    record.state = AttemptState::authorized;
    status = persist_attempt_locked(record, false);
    if (!status.ok()) {
      return status;
    }
    active_attempt_ = attempt_id;
    PromotionAuthorization authorization;
    authorization.decision = decision;
    authorization.grant.id = GrantId::from_value(attempt_id.value());
    authorization.grant.attempt = attempt_id;
    authorization.grant.path = decision.selected;
    authorization.grant.basis = state_.authority;
    authorization.grant.kind = AuthorityKind::grant;
    grants_[attempt_id] = authorization.grant;
    ++stats_.promotions_authorized;
    status = compact_if_needed_locked();
    if (!status.ok()) {
      return status;
    }
    return authorization;
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::record_effect(AttemptId attempt_id, const EffectEvidence& effect) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    AttemptRecord* record = find_attempt_locked(attempt_id);
    if (record == nullptr) {
      return Status(Code::not_found, Reason::attempt_not_found, "unknown attempt");
    }
    if (record->state == AttemptState::committed) {
      return Status(Code::refused, Reason::attempt_already_committed,
                    "attempt is already committed");
    }
    if (record->state == AttemptState::fenced) {
      ++stats_.stale_completions_rejected;
      return Status(Code::fenced, Reason::attempt_fenced, "attempt was fenced");
    }
    if (record->state == AttemptState::superseded || record->state == AttemptState::interrupted) {
      ++stats_.stale_completions_rejected;
      return Status(Code::superseded, Reason::attempt_superseded, "attempt was superseded");
    }
    if (is_terminal_state(record->state)) {
      return Status(Code::refused, Reason::attempt_terminal, "attempt is terminal");
    }
    if (active_attempt_ != attempt_id) {
      ++stats_.stale_completions_rejected;
      return Status(Code::superseded, Reason::attempt_superseded,
                    "a newer attempt owns the transition");
    }
    Status valid = effect.validate();
    if (!valid.ok()) {
      return valid;
    }
    const AuthorityComparison comparison = classify(effect.observed_under, state_.authority);
    if (comparison.overall != AuthorityClass::current) {
      return Status(Code::stale, Reason::effect_authority_mismatch,
                    "verified effect is not bound to current authority");
    }
    if (effect.path != record->selected) {
      return Status(Code::invalid, Reason::effect_path_mismatch,
                    "verified effect belongs to a different path");
    }
    if (const auto stored = evidence_.find(effect.path); stored != evidence_.end()) {
      if (effect.observation_seq <= stored->second.observation_seq) {
        return Status(Code::stale, Reason::evidence_superseded,
                      "effect observation sequence is not newer than the stored one");
      }
    }

    record->state = AttemptState::committed;
    record->terminal_reason = Reason::none;
    record->terminal_sequence = state_.last_sequence;
    Status status = persist_attempt_locked(*record, false);
    if (!status.ok()) {
      return status;
    }

    LineageEntry entry;
    entry.decision = DecisionId::from_value(state_.decision_sequence);
    entry.attempt = attempt_id;
    entry.epoch = identity_.epoch;
    entry.boot = identity_.boot;
    entry.outcome = PromotionOutcome::promoted;
    entry.incumbent = record->incumbent;
    entry.selected = record->selected;
    entry.set = record->set;
    entry.basis = state_.authority;
    entry.reason = Reason::selected;
    entry.sequence = state_.last_sequence;
    status = persist_lineage_locked(entry);
    if (!status.ok()) {
      return status;
    }
    evidence_.insert_or_assign(effect.path,
                               PathEvidence{.id = effect.id,
                                            .path = effect.path,
                                            .freshness = Freshness::fresh,
                                            .eligibility = Eligibility::eligible,
                                            .reachability = Reachability::reachable,
                                            .capacity = CapacityClass::sufficient,
                                            .capabilities = 0,
                                            .health_ppm = 0,
                                            .cost_units = 0,
                                            .observation_seq = effect.observation_seq,
                                            .observed_under = effect.observed_under,
                                            .provenance = effect.provenance});
    active_attempt_ = AttemptId::absent();
    grants_.erase(attempt_id);
    ++stats_.promotions_committed;
    return compact_if_needed_locked();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::abandon_attempt(AttemptId attempt_id, Reason reason) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    AttemptRecord* record = find_attempt_locked(attempt_id);
    if (record == nullptr) {
      return Status(Code::not_found, Reason::attempt_not_found, "unknown attempt");
    }
    if (is_terminal_state(record->state)) {
      return Status(Code::refused, Reason::attempt_terminal, "attempt is terminal");
    }
    record->state = AttemptState::abandoned;
    record->terminal_reason = reason;
    record->terminal_sequence = state_.last_sequence;
    Status status = persist_attempt_locked(*record, false);
    if (!status.ok()) {
      return status;
    }
    LineageEntry entry;
    entry.decision = DecisionId::from_value(state_.decision_sequence);
    entry.attempt = attempt_id;
    entry.epoch = identity_.epoch;
    entry.boot = identity_.boot;
    entry.outcome = PromotionOutcome::refused_superseded_attempt;
    entry.incumbent = record->incumbent;
    entry.selected = PathId::absent();
    entry.set = record->set;
    entry.basis = state_.authority;
    entry.reason = reason;
    entry.sequence = state_.last_sequence;
    status = persist_lineage_locked(entry);
    if (!status.ok()) {
      return status;
    }
    grants_.erase(attempt_id);
    if (active_attempt_ == attempt_id) {
      active_attempt_ = AttemptId::absent();
    }
    return compact_if_needed_locked();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Status Fabric::rollback(AttemptId attempt_id, const PathEvidence& incumbent_evidence) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    AttemptRecord* record = find_attempt_locked(attempt_id);
    if (record == nullptr) {
      return Status(Code::not_found, Reason::attempt_not_found, "unknown attempt");
    }
    if (record->state == AttemptState::committed) {
      return Status(Code::refused, Reason::rollback_not_committed,
                    "a committed promotion is reverted, not rolled back");
    }
    if (is_terminal_state(record->state)) {
      return Status(Code::refused, Reason::attempt_terminal, "attempt is terminal");
    }
    if (active_attempt_ != attempt_id) {
      ++stats_.stale_completions_rejected;
      return Status(Code::superseded, Reason::attempt_superseded,
                    "a newer attempt owns the transition");
    }
    if (!incumbent_evidence.path.valid() || incumbent_evidence.path != record->incumbent) {
      return Status(Code::invalid, Reason::effect_path_mismatch,
                    "rollback evidence does not describe the incumbent path");
    }
    Reason target_reason = Reason::none;
    Status target_status = validate_target_locked(incumbent_evidence, target_reason);
    if (!target_status.ok()) {
      return Status(Code::refused, Reason::rollback_incumbent_invalid, target_status.detail);
    }
    ++stats_.revalidations;
    if (state_.has_policy && record->revalidations >= state_.policy.max_revalidations) {
      return Status(Code::exhausted, Reason::revalidation_exhausted,
                    "revalidation budget exhausted");
    }
    record->revalidations += 1;
    record->state = AttemptState::rolled_back;
    record->terminal_reason = target_reason;
    record->terminal_sequence = state_.last_sequence;
    Status status = persist_attempt_locked(*record, false);
    if (!status.ok()) {
      return status;
    }
    LineageEntry entry;
    entry.decision = DecisionId::from_value(state_.decision_sequence);
    entry.attempt = attempt_id;
    entry.epoch = identity_.epoch;
    entry.boot = identity_.boot;
    entry.outcome = PromotionOutcome::rolled_back;
    entry.incumbent = record->incumbent;
    entry.selected = record->selected;
    entry.set = record->set;
    entry.basis = state_.authority;
    entry.reason = target_reason;
    entry.sequence = state_.last_sequence;
    status = persist_lineage_locked(entry);
    if (!status.ok()) {
      return status;
    }
    grants_.erase(attempt_id);
    if (active_attempt_ == attempt_id) {
      active_attempt_ = AttemptId::absent();
    }
    ++stats_.rollbacks;
    return compact_if_needed_locked();
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

Result<PromotionDecision> Fabric::revert(AttemptId committed_attempt,
                                        const PathEvidence& target) {
  try {
    std::lock_guard<std::mutex> lock(mu_);
    const AttemptRecord* committed = nullptr;
    const auto it = attempts_.find(committed_attempt);
    if (it == attempts_.end()) {
      return Status(Code::not_found, Reason::attempt_not_found, "unknown attempt");
    }
    committed = &it->second;
    if (committed->state != AttemptState::committed) {
      return Status(Code::refused, Reason::rollback_not_committed,
                    "reversion applies to a committed promotion");
    }
    Reason target_reason = Reason::none;
    Status target_status = validate_target_locked(target, target_reason);
    if (!target_status.ok()) {
      return Status(Code::refused, Reason::rollback_incumbent_invalid, target_status.detail);
    }
    if (state_.has_policy && committed->revalidations >= state_.policy.max_revalidations) {
      return Status(Code::exhausted, Reason::revalidation_exhausted,
                    "revalidation budget exhausted");
    }
    if (active_attempt_.valid()) {
      return Status(Code::refused, Reason::attempt_active,
                    "another promotion attempt owns the transition");
    }
    if (state_.attempt_sequence == UINT64_MAX || state_.decision_sequence == UINT64_MAX) {
      return Status(Code::exhausted, Reason::table_full, "attempt sequence exhausted");
    }

    const AttemptId attempt_id = AttemptId::from_value(state_.attempt_sequence + 1);
    const DecisionId decision_id = DecisionId::from_value(state_.decision_sequence + 1);
    ++stats_.revalidations;

    PromotionDecision decision;
    decision.id = decision_id;
    decision.outcome = PromotionOutcome::reverted;
    decision.authority_kind = AuthorityKind::grant;
    decision.incumbent = committed->selected;
    decision.selected = target.path;
    decision.basis = state_.authority;
    decision.attempt = attempt_id;
    decision.reason = target_reason;
    decision.provenance = options_.provenance;
    decision.explanation.emplace_back(target_reason, static_cast<std::int64_t>(target.path.value()));

    AttemptRecord record;
    record.id = attempt_id;
    record.epoch = identity_.epoch;
    record.boot = identity_.boot;
    record.incumbent = committed->selected;
    record.selected = target.path;
    record.set = committed->set;
    record.set_generation = committed->set_generation;
    record.basis = state_.authority;
    record.state = AttemptState::committed;
    record.terminal_reason = target_reason;
    record.begin_sequence = state_.last_sequence + 1;
    record.terminal_sequence = state_.last_sequence;
    record.revalidations = committed->revalidations + 1;

    Status status = persist_attempt_locked(record, true);
    if (!status.ok()) {
      return status;
    }
    ++state_.attempt_sequence;
    ++state_.decision_sequence;

    LineageEntry entry;
    entry.decision = decision_id;
    entry.attempt = attempt_id;
    entry.epoch = identity_.epoch;
    entry.boot = identity_.boot;
    entry.outcome = PromotionOutcome::reverted;
    entry.incumbent = record.incumbent;
    entry.selected = record.selected;
    entry.set = record.set;
    entry.basis = state_.authority;
    entry.reason = target_reason;
    entry.sequence = state_.last_sequence;
    status = persist_lineage_locked(entry);
    if (!status.ok()) {
      return status;
    }
    ++stats_.reversions;
    status = compact_if_needed_locked();
    if (!status.ok()) {
      return status;
    }
    return decision;
  } catch (const std::bad_alloc&) {
    return Status(Code::exhausted, Reason::allocation_denied, "out of memory");
  }
}

// ---- queries ---------------------------------------------------------------

FabricIdentity Fabric::identity() const {
  std::lock_guard<std::mutex> lock(mu_);
  return identity_;
}

AuthorityVector Fabric::authority() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.authority;
}

FabricStats Fabric::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

const RecoveryReport& Fabric::recovery() const noexcept { return store_->recovery(); }

bool Fabric::policy_installed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.has_policy;
}

bool Fabric::obligations_installed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.has_obligations;
}

std::size_t Fabric::alternate_set_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return sets_.size();
}

std::size_t Fabric::active_attempt_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::size_t count = 0;
  for (const auto& [id, attempt] : attempts_) {
    if (is_active_state(attempt.state)) {
      ++count;
    }
  }
  return count;
}

Result<AttemptRecord> Fabric::attempt(AttemptId id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = attempts_.find(id);
  if (it == attempts_.end()) {
    return Status(Code::not_found, Reason::attempt_not_found, "unknown attempt");
  }
  return it->second;
}

std::vector<AttemptRecord> Fabric::attempts() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<AttemptRecord> out;
  out.reserve(attempts_.size());
  for (const auto& [id, attempt] : attempts_) {
    out.push_back(attempt);
  }
  return out;
}

std::vector<AttemptRecord> Fabric::active_attempts() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<AttemptRecord> out;
  for (const auto& [id, attempt] : attempts_) {
    if (is_active_state(attempt.state)) {
      out.push_back(attempt);
    }
  }
  return out;
}

std::vector<LineageEntry> Fabric::lineage(std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<LineageEntry> out;
  const std::size_t bounded = std::min(limit, state_.lineage.size());
  out.reserve(bounded);
  for (std::size_t i = state_.lineage.size() - bounded; i < state_.lineage.size(); ++i) {
    out.push_back(state_.lineage[i]);
  }
  return out;
}

std::vector<FenceRecord> Fabric::fences() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_.fences;
}

Result<AlternateSet> Fabric::alternate_set(AlternateSetId id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = sets_.find(id);
  if (it == sets_.end()) {
    return Status(Code::not_found, Reason::domain_invalid, "unknown alternate set");
  }
  return it->second;
}

Result<FailoverPolicy> Fabric::policy() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!state_.has_policy) {
    return Status(Code::not_found, Reason::policy_invalid, "no policy installed");
  }
  return state_.policy;
}

Result<ServiceObligations> Fabric::obligations() const {
  std::lock_guard<std::mutex> lock(mu_);
  if (!state_.has_obligations) {
    return Status(Code::not_found, Reason::obligations_invalid, "no obligations installed");
  }
  return state_.obligations;
}

Result<PathEvidence> Fabric::evidence(PathId path) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = evidence_.find(path);
  if (it == evidence_.end()) {
    return Status(Code::not_found, Reason::evidence_unknown, "no evidence for path");
  }
  return it->second;
}

}  // namespace pff
