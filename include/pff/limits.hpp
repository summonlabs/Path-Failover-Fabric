// Path Failover Fabric - hard resource bounds.
//
// Every table, queue, document, history and allocation owned by the runtime is
// bounded by a compile-time constant in this header. The selector and the
// coordinator refuse work that would exceed a bound instead of growing without
// limit, so exhaustion is a deterministic refusal and never process instability.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>

namespace pff {
namespace limits {

// ---- candidate surface -----------------------------------------------------
inline constexpr std::size_t max_candidates_per_set = 4096;
inline constexpr std::size_t max_alternate_sets = 64;
inline constexpr std::uint32_t default_search_limit = 4096;
inline constexpr std::uint32_t max_search_limit = 1u << 20;

// ---- retained history ------------------------------------------------------
inline constexpr std::size_t max_fences = 4096;
inline constexpr std::size_t max_lineage = 4096;
inline constexpr std::size_t max_attempts = 1024;
inline constexpr std::size_t max_assessments_reported = 4096;
inline constexpr std::size_t max_indeterminate_reported = 256;
inline constexpr std::size_t max_explanation_steps = 64;
inline constexpr std::size_t max_reason_bytes = 128;

// ---- text ------------------------------------------------------------------
inline constexpr std::size_t max_name_bytes = 96;

// ---- durable artifacts -----------------------------------------------------
inline constexpr std::size_t max_record_payload = 1u << 20;      // 1 MiB
inline constexpr std::size_t max_journal_bytes = 64u << 20;      // 64 MiB
inline constexpr std::size_t max_snapshot_bytes = 64u << 20;     // 64 MiB
inline constexpr std::size_t max_snapshot_records = 100000;

// ---- framed protocol -------------------------------------------------------
inline constexpr std::size_t max_frame_payload = 1u << 20;       // 1 MiB
inline constexpr std::size_t frame_header_bytes = 28;
inline constexpr std::size_t frame_trailer_bytes = 4;
inline constexpr std::size_t max_frame_bytes =
    frame_header_bytes + max_frame_payload + frame_trailer_bytes;

// ---- sessions --------------------------------------------------------------
inline constexpr std::size_t max_sessions = 64;
inline constexpr std::uint64_t default_max_requests_per_session = 100000;

// ---- evidence --------------------------------------------------------------
inline constexpr std::size_t max_evidence_entries = 8192;

// ---- checked arithmetic ----------------------------------------------------
inline constexpr std::int64_t max_weight = 1000000;
inline constexpr std::int64_t cost_scale = 1000000000;   // max cost_units
inline constexpr std::int64_t rank_scale = 65535;        // max upstream rank
inline constexpr std::int64_t health_scale = 1000000;    // health in parts per million

}  // namespace limits
}  // namespace pff
