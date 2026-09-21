// Path Failover Fabric - versioned, integrity-checked durable storage.
//
// Artifacts are written with append/flush/commit ordering: the journal record is
// fully flushed before the operation it describes is acknowledged, and snapshots
// use transactional replacement (write staging, flush, atomic rename).
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "pff/canonical.hpp"
#include "pff/records.hpp"
#include "pff/status.hpp"

namespace pff {

enum class RecordType : std::uint16_t {
  policy_set = 1,
  obligations_set = 2,
  alternate_set_put = 3,
  fence_recorded = 4,
  attempt_begin = 5,
  attempt_state = 6,
  lineage_entry = 7,
  epoch_advance = 8,
  snapshot_marker = 9,
  counters = 10,
};

const char* to_string(RecordType t) noexcept;
[[nodiscard]] bool is_known_record_type(std::uint16_t raw) noexcept;

// On-disk framing constants. A journal record is
//   [header 24 bytes][payload][payload crc 4 bytes]
// where the header is
//   magic u32 | version u16 | type u16 | sequence u64 | payload_length u32 | header_crc u32
// and header_crc covers the first 20 bytes, so payload_length is authenticated
// before it is ever used to size an allocation.
inline constexpr std::uint32_t journal_magic = 0x31464650u;   // "PFF1"
inline constexpr std::size_t journal_header_bytes = 24;
inline constexpr std::size_t journal_trailer_bytes = 4;

inline constexpr std::uint32_t snapshot_magic = 0x53464650u;  // "PFFS"
inline constexpr std::uint32_t snapshot_footer_magic = 0x45464650u;  // "PFFE"
inline constexpr std::size_t snapshot_header_bytes = 40;

struct JournalRecordHeader {
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t type = 0;
  std::uint64_t sequence = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t header_crc = 0;
};

[[nodiscard]] std::vector<std::byte> encode_journal_record(RecordType type, std::uint64_t sequence,
                                                           std::span<const std::byte> payload);
[[nodiscard]] Result<JournalRecordHeader> decode_journal_header(
    std::span<const std::byte> header_bytes);

// Payload encoders. They are public so that adversarial persistence tests can
// craft corrupt, truncated and replayed records at byte level.
[[nodiscard]] std::vector<std::byte> encode_policy_record(const FailoverPolicy& policy);
[[nodiscard]] std::vector<std::byte> encode_obligations_record(const ServiceObligations& obligations);
[[nodiscard]] std::vector<std::byte> encode_set_record(const AlternateSet& set);
[[nodiscard]] std::vector<std::byte> encode_fence_record(const FenceRecord& fence);
[[nodiscard]] std::vector<std::byte> encode_attempt_record(const AttemptRecord& attempt);
[[nodiscard]] std::vector<std::byte> encode_lineage_record(const LineageEntry& entry);
[[nodiscard]] std::vector<std::byte> encode_epoch_record(const AuthorityVector& authority,
                                                         std::uint64_t restart_count,
                                                         std::uint64_t attempt_sequence,
                                                         std::uint64_t fence_sequence,
                                                         std::uint64_t decision_sequence);
[[nodiscard]] std::vector<std::byte> encode_counters_record(const DurableState& state);

// Applies one decoded record to a materialized state. Never throws; on a
// malformed payload it returns a status and leaves the state untouched.
[[nodiscard]] Status apply_record(DurableState& state, RecordType type,
                                  std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode_snapshot(const DurableState& state);
[[nodiscard]] Result<DurableState> decode_snapshot(std::span<const std::byte> bytes);

struct StoreOptions {
  std::filesystem::path directory;
  // inspection_only never takes the writer lock and never mutates the files; a
  // torn tail is reported rather than repaired.
  bool inspection_only = false;
  bool create_directory = true;
};

class Store {
 public:
  static Result<std::unique_ptr<Store>> open(const StoreOptions& options);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Reads the snapshot, replays the journal and returns the recovered state.
  Result<DurableState> load();

  // Appends one record and flushes it to stable storage before returning.
  Status append(RecordType type, std::span<const std::byte> payload);

  // Transactionally replaces the snapshot and then resets the journal.
  Status write_snapshot(const DurableState& state);

  Status close();
  void reset_journal_for_test();

  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] bool inspection_only() const noexcept { return inspection_only_; }
  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
  [[nodiscard]] std::uint64_t journal_bytes() const noexcept { return journal_bytes_; }

 private:
  Store() = default;

  Status open_files();
  Status load_snapshot(DurableState& state);
  Status replay_journal(DurableState& state);

  std::filesystem::path directory_;
  std::filesystem::path journal_path_;
  std::filesystem::path snapshot_path_;
  std::filesystem::path lock_path_;
  std::FILE* journal_ = nullptr;
  void* lock_handle_ = nullptr;
  bool inspection_only_ = false;
  bool closed_ = false;
  std::uint64_t last_sequence_ = 0;
  std::uint64_t journal_bytes_ = 0;
  RecoveryReport recovery_;
};

}  // namespace pff
