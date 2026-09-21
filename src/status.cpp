// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/status.hpp"

#include "pff/limits.hpp"

namespace pff {
namespace {

const char* const kCodeNames[] = {
    "ok",           "invalid",       "unknown",        "stale",         "conflict",
    "unsupported",  "refused",       "exhausted",      "corrupt",       "not_found",
    "already_exists", "fenced",      "superseded",     "interrupted",   "search_limited",
    "proven_absent", "version_unsupported", "not_owner", "io_failure",  "table_full",
};

const char* const kReasonNames[] = {
    "none",
    "incumbent_is_self",
    "withdrawn",
    "authority_stale",
    "authority_unknown",
    "authority_conflict",
    "eligibility_ineligible",
    "eligibility_unknown",
    "evidence_stale",
    "evidence_unknown",
    "reachability_unreachable",
    "reachability_unknown",
    "capacity_insufficient",
    "capacity_unknown",
    "capability_missing",
    "health_below_obligation",
    "cost_above_obligation",
    "score_below_policy",
    "score_overflow",
    "path_fenced",
    "duplicate_path",
    "rank_out_of_range",
    "candidates_too_many",
    "set_generation_mismatch",
    "set_incomplete",
    "set_completeness_unknown",
    "search_limit_reached",
    "policy_invalid",
    "obligations_invalid",
    "policy_generation_stale",
    "obligations_generation_stale",
    "topology_generation_stale",
    "path_authority_generation_stale",
    "epoch_stale",
    "boot_mismatch",
    "generation_conflict",
    "incumbent_not_failed",
    "authority_current",
    "attempt_active",
    "attempt_not_found",
    "attempt_fenced",
    "attempt_superseded",
    "attempt_already_committed",
    "attempt_terminal",
    "effect_unverified",
    "effect_authority_mismatch",
    "effect_path_mismatch",
    "rollback_incumbent_invalid",
    "rollback_no_prior_incumbent",
    "rollback_not_committed",
    "revalidation_exhausted",
    "rollback_authority_mismatch",
    "no_active_attempt",
    "read_only",
    "corrupt_header",
    "corrupt_payload",
    "unsupported_version",
    "length_invalid",
    "sequence_regression",
    "trailing_garbage",
    "torn_tail_recovered",
    "integrity_failure",
    "directory_locked",
    "io_failure",
    "state_inconsistent",
    "format_magic_mismatch",
    "record_type_invalid",
    "snapshot_missing",
    "frame_magic",
    "frame_truncated",
    "frame_oversize",
    "frame_trailing_bytes",
    "enum_invalid",
    "domain_invalid",
    "session_unknown",
    "session_stale",
    "session_limit",
    "request_limit",
    "message_unsupported",
    "protocol_version_mismatch",
    "table_full",
    "allocation_denied",
    "invalid_payload",
    "evidence_superseded",
    "selected",
    "promotable_not_selected",
    "excluded",
    "indeterminate",
    "proven_no_alternate",
    "complete_set_exhausted",
    "restart_fenced",
    "authority_fenced",
};

static_assert(sizeof(kCodeNames) / sizeof(kCodeNames[0]) == 20,
              "code name table must cover every Code enumerator");
static_assert(sizeof(kReasonNames) / sizeof(kReasonNames[0]) == 92,
              "reason name table must cover every Reason enumerator");

}  // namespace

const char* to_string(Code c) noexcept {
  const auto index = static_cast<std::size_t>(c);
  if (index >= (sizeof(kCodeNames) / sizeof(kCodeNames[0]))) {
    return "invalid_code";
  }
  return kCodeNames[index];
}

const char* to_string(Reason r) noexcept {
  const auto index = static_cast<std::size_t>(r);
  if (index >= (sizeof(kReasonNames) / sizeof(kReasonNames[0]))) {
    return "invalid_reason";
  }
  return kReasonNames[index];
}

Status make_status(Code c, Reason r) noexcept { return Status(c, r); }

Status with_detail(Status s, std::string detail) {
  if (detail.size() > limits::max_reason_bytes) {
    detail.resize(limits::max_reason_bytes);
  }
  s.detail = std::move(detail);
  return s;
}

std::string Status::describe() const {
  std::string out;
  out.reserve(64);
  out += to_string(code);
  out += ':';
  out += to_string(reason);
  if (!detail.empty()) {
    out += ':';
    out += detail;
  }
  return out;
}

}  // namespace pff
