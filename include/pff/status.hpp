// Path Failover Fabric - status, reason and result vocabulary.
//
// The vocabulary is deliberately explicit: UNKNOWN, STALE, CONFLICT, INVALID and
// UNSUPPORTED are first-class outcomes and are never folded into success or into
// ordinary absence.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace pff {

enum class Code : std::uint8_t {
  ok = 0,
  invalid = 1,
  unknown = 2,
  stale = 3,
  conflict = 4,
  unsupported = 5,
  refused = 6,
  exhausted = 7,
  corrupt = 8,
  not_found = 9,
  already_exists = 10,
  fenced = 11,
  superseded = 12,
  interrupted = 13,
  search_limited = 14,
  proven_absent = 15,
  version_unsupported = 16,
  not_owner = 17,
  io_failure = 18,
  table_full = 19,
};

const char* to_string(Code c) noexcept;

enum class Reason : std::uint16_t {
  none = 0,
  // ---- per-candidate gates -------------------------------------------------
  incumbent_is_self = 1,
  withdrawn = 2,
  authority_stale = 3,
  authority_unknown = 4,
  authority_conflict = 5,
  eligibility_ineligible = 6,
  eligibility_unknown = 7,
  evidence_stale = 8,
  evidence_unknown = 9,
  reachability_unreachable = 10,
  reachability_unknown = 11,
  capacity_insufficient = 12,
  capacity_unknown = 13,
  capability_missing = 14,
  health_below_obligation = 15,
  cost_above_obligation = 16,
  score_below_policy = 17,
  score_overflow = 18,
  path_fenced = 19,
  duplicate_path = 20,
  rank_out_of_range = 21,
  candidates_too_many = 22,
  // ---- set / authority inputs ---------------------------------------------
  set_generation_mismatch = 23,
  set_incomplete = 24,
  set_completeness_unknown = 25,
  search_limit_reached = 26,
  policy_invalid = 27,
  obligations_invalid = 28,
  policy_generation_stale = 29,
  obligations_generation_stale = 30,
  topology_generation_stale = 31,
  path_authority_generation_stale = 32,
  epoch_stale = 33,
  boot_mismatch = 34,
  generation_conflict = 35,
  incumbent_not_failed = 36,
  authority_current = 37,
  // ---- attempt lifecycle ---------------------------------------------------
  attempt_active = 38,
  attempt_not_found = 39,
  attempt_fenced = 40,
  attempt_superseded = 41,
  attempt_already_committed = 42,
  attempt_terminal = 43,
  effect_unverified = 44,
  effect_authority_mismatch = 45,
  effect_path_mismatch = 46,
  rollback_incumbent_invalid = 47,
  rollback_no_prior_incumbent = 48,
  rollback_not_committed = 49,
  revalidation_exhausted = 50,
  rollback_authority_mismatch = 51,
  no_active_attempt = 52,
  read_only = 53,
  // ---- persistence ---------------------------------------------------------
  corrupt_header = 54,
  corrupt_payload = 55,
  unsupported_version = 56,
  length_invalid = 57,
  sequence_regression = 58,
  trailing_garbage = 59,
  torn_tail_recovered = 60,
  integrity_failure = 61,
  directory_locked = 62,
  io_failure = 63,
  state_inconsistent = 64,
  format_magic_mismatch = 65,
  record_type_invalid = 66,
  snapshot_missing = 67,
  // ---- protocol ------------------------------------------------------------
  frame_magic = 68,
  frame_truncated = 69,
  frame_oversize = 70,
  frame_trailing_bytes = 71,
  enum_invalid = 72,
  domain_invalid = 73,
  session_unknown = 74,
  session_stale = 75,
  session_limit = 76,
  request_limit = 77,
  message_unsupported = 78,
  protocol_version_mismatch = 79,
  // ---- resource ------------------------------------------------------------
  table_full = 80,
  allocation_denied = 81,
  invalid_payload = 82,
  evidence_superseded = 83,
  // ---- selection summary ---------------------------------------------------
  selected = 84,
  promotable_not_selected = 85,
  excluded = 86,
  indeterminate = 87,
  proven_no_alternate = 88,
  complete_set_exhausted = 89,
  restart_fenced = 90,
  authority_fenced = 91,
};

const char* to_string(Reason r) noexcept;

// Highest valid enumerator of each vocabulary. Encoders and decoders use these
// bounds so that adding a reason or code can never silently invalidate a
// previously written document.
inline constexpr std::uint16_t max_reason_value =
    static_cast<std::uint16_t>(Reason::authority_fenced);
inline constexpr std::uint8_t max_code_value = static_cast<std::uint8_t>(Code::table_full);

// Stable, bounded status value. detail is truncated to limits::max_reason_bytes
// when it is constructed through with_detail().
struct Status {
  Code code = Code::ok;
  Reason reason = Reason::none;
  std::string detail;

  Status() = default;
  Status(Code c, Reason r) noexcept : code(c), reason(r) {}
  Status(Code c, Reason r, std::string d) : code(c), reason(r), detail(std::move(d)) {}

  [[nodiscard]] bool ok() const noexcept { return code == Code::ok; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] std::string describe() const;
};

Status make_status(Code c, Reason r) noexcept;
Status with_detail(Status s, std::string detail);

inline Status ok_status() noexcept { return Status{}; }

template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {
    // A Result is either a value or a failure. Building one from a success
    // status would silently produce a failed result that reports "ok", so it is
    // a programming error in this library and is caught in debug builds.
    assert(!status_.ok());
  }

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & { return *value_; }
  const T& value() const& { return *value_; }
  T&& value() && { return std::move(*value_); }

  const Status& status() const noexcept { return status_; }

 private:
  std::optional<T> value_;
  Status status_;
};

// Clamp helper used wherever an out-of-range numeric input must be refused or
// bounded rather than silently wrapped.
[[nodiscard]] constexpr bool in_range(std::int64_t v, std::int64_t lo, std::int64_t hi) noexcept {
  return v >= lo && v <= hi;
}

}  // namespace pff
