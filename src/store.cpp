// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/store.hpp"

#include <algorithm>
#include <cstring>
#include <system_error>
#include <utility>

#include "pff/crc32c.hpp"
#include "pff/version.hpp"
#include "pff/wire.hpp"
#include "platform.hpp"

namespace pff {
namespace {

constexpr std::size_t kSnapshotHeaderCrcOffset = 32;

std::vector<std::byte> finish(ByteWriter& writer) {
  if (!writer.ok()) {
    return {};
  }
  return std::move(writer).take();
}

Status end_of(ByteReader& reader) {
  if (!reader.ok()) {
    return reader.status();
  }
  return reader.require_end();
}

std::uint32_t read_u32_at(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + i])) << (8u * i);
  }
  return value;
}

}  // namespace

const char* to_string(RecordType t) noexcept {
  switch (t) {
    case RecordType::policy_set:
      return "policy_set";
    case RecordType::obligations_set:
      return "obligations_set";
    case RecordType::alternate_set_put:
      return "alternate_set_put";
    case RecordType::fence_recorded:
      return "fence_recorded";
    case RecordType::attempt_begin:
      return "attempt_begin";
    case RecordType::attempt_state:
      return "attempt_state";
    case RecordType::lineage_entry:
      return "lineage_entry";
    case RecordType::epoch_advance:
      return "epoch_advance";
    case RecordType::snapshot_marker:
      return "snapshot_marker";
    case RecordType::counters:
      return "counters";
  }
  return "invalid";
}

bool is_known_record_type(std::uint16_t raw) noexcept {
  return raw >= static_cast<std::uint16_t>(RecordType::policy_set) &&
         raw <= static_cast<std::uint16_t>(RecordType::counters);
}

// ---- framing ---------------------------------------------------------------

std::vector<std::byte> encode_journal_record(RecordType type, std::uint64_t sequence,
                                             std::span<const std::byte> payload) {
  if (payload.size() > limits::max_record_payload || sequence == 0) {
    return {};
  }
  ByteWriter writer(journal_header_bytes + payload.size() + journal_trailer_bytes,
                    limits::max_record_payload + journal_header_bytes + journal_trailer_bytes);
  writer.u32(journal_magic);
  writer.u16(durable_format_version);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u64(sequence);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  if (!writer.ok()) {
    return {};
  }
  writer.u32(crc32c(writer.data()));
  writer.bytes(payload);
  if (!writer.ok()) {
    return {};
  }
  writer.u32(crc32c(payload));
  return finish(writer);
}

Result<JournalRecordHeader> decode_journal_header(std::span<const std::byte> header_bytes) {
  if (header_bytes.size() != journal_header_bytes) {
    return Status(Code::invalid, Reason::length_invalid, "journal header length");
  }
  ByteReader reader(header_bytes);
  JournalRecordHeader header;
  header.magic = reader.u32();
  header.version = reader.u16();
  header.type = reader.u16();
  header.sequence = reader.u64();
  header.payload_length = reader.u32();
  header.header_crc = reader.u32();
  if (!reader.ok() || !reader.at_end()) {
    return Status(Code::invalid, Reason::length_invalid, "journal header truncated");
  }
  if (header.magic != journal_magic) {
    return Status(Code::corrupt, Reason::format_magic_mismatch, "journal magic mismatch");
  }
  if (header.version != durable_format_version) {
    return Status(Code::version_unsupported, Reason::unsupported_version,
                  "journal format version unsupported");
  }
  if (!is_known_record_type(header.type)) {
    return Status(Code::invalid, Reason::record_type_invalid, "journal record type invalid");
  }
  if (header.sequence == 0) {
    return Status(Code::corrupt, Reason::sequence_regression, "journal sequence zero");
  }
  if (header.payload_length > limits::max_record_payload) {
    return Status(Code::corrupt, Reason::length_invalid, "journal payload length exceeds bound");
  }
  const std::uint32_t expected_crc =
      crc32c(header_bytes.first(journal_header_bytes - journal_trailer_bytes));
  if (expected_crc != header.header_crc) {
    return Status(Code::corrupt, Reason::corrupt_header, "journal header integrity failure");
  }
  return header;
}

// ---- payload encoders ------------------------------------------------------

std::vector<std::byte> encode_policy_record(const FailoverPolicy& policy) {
  ByteWriter writer(64, limits::max_record_payload);
  wire::put(writer, policy);
  return finish(writer);
}

std::vector<std::byte> encode_obligations_record(const ServiceObligations& obligations) {
  ByteWriter writer(64, limits::max_record_payload);
  wire::put(writer, obligations);
  return finish(writer);
}

std::vector<std::byte> encode_set_record(const AlternateSet& set) {
  ByteWriter writer(256, limits::max_record_payload);
  wire::put(writer, set);
  return finish(writer);
}

std::vector<std::byte> encode_fence_record(const FenceRecord& fence) {
  ByteWriter writer(128, limits::max_record_payload);
  wire::put(writer, fence);
  return finish(writer);
}

std::vector<std::byte> encode_attempt_record(const AttemptRecord& attempt) {
  ByteWriter writer(160, limits::max_record_payload);
  wire::put(writer, attempt);
  return finish(writer);
}

std::vector<std::byte> encode_lineage_record(const LineageEntry& entry) {
  ByteWriter writer(160, limits::max_record_payload);
  wire::put(writer, entry);
  return finish(writer);
}

std::vector<std::byte> encode_epoch_record(const AuthorityVector& authority,
                                           std::uint64_t restart_count,
                                           std::uint64_t attempt_sequence,
                                           std::uint64_t fence_sequence,
                                           std::uint64_t decision_sequence) {
  ByteWriter writer(96, limits::max_record_payload);
  wire::put(writer, authority);
  writer.u64(restart_count);
  writer.u64(attempt_sequence);
  writer.u64(fence_sequence);
  writer.u64(decision_sequence);
  return finish(writer);
}

std::vector<std::byte> encode_counters_record(const DurableState& state) {
  ByteWriter writer(64, limits::max_record_payload);
  writer.u64(state.attempt_sequence);
  writer.u64(state.fence_sequence);
  writer.u64(state.decision_sequence);
  writer.u64(state.fence_overflow);
  writer.u64(state.attempts_dropped);
  writer.boolean(state.lineage_truncated);
  return finish(writer);
}

// ---- record application ----------------------------------------------------

Status apply_record(DurableState& state, RecordType type, std::span<const std::byte> payload) {
  ByteReader reader(payload);
  switch (type) {
    case RecordType::policy_set: {
      auto value = wire::get_policy(reader);
      if (!value.ok()) {
        return value.status();
      }
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      state.policy = value.value();
      state.has_policy = true;
      state.authority.policy = state.policy.generation;
      return ok_status();
    }
    case RecordType::obligations_set: {
      auto value = wire::get_obligations(reader);
      if (!value.ok()) {
        return value.status();
      }
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      state.obligations = value.value();
      state.has_obligations = true;
      state.authority.obligations = state.obligations.generation;
      return ok_status();
    }
    case RecordType::alternate_set_put: {
      auto value = wire::get_set(reader);
      if (!value.ok()) {
        return value.status();
      }
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      const AlternateSet& set = value.value();
      const auto existing = std::find_if(state.sets.begin(), state.sets.end(),
                                         [&set](const AlternateSet& candidate) {
                                           return candidate.id == set.id;
                                         });
      if (existing == state.sets.end()) {
        if (state.sets.size() >= limits::max_alternate_sets) {
          return Status(Code::table_full, Reason::table_full, "alternate set table full");
        }
        state.sets.push_back(set);
      } else {
        *existing = set;
      }
      return ok_status();
    }
    case RecordType::fence_recorded: {
      auto value = wire::get_fence(reader);
      if (!value.ok()) {
        return value.status();
      }
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      if (state.fences.size() >= limits::max_fences) {
        // A fence is authority bearing: it is never silently dropped.
        return Status(Code::table_full, Reason::table_full, "fence table full");
      }
      state.fences.push_back(value.value());
      return ok_status();
    }
    case RecordType::attempt_begin:
    case RecordType::attempt_state: {
      auto value = wire::get_attempt(reader);
      if (!value.ok()) {
        return value.status();
      }
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      const AttemptRecord& record = value.value();
      const auto existing = std::find_if(state.attempts.begin(), state.attempts.end(),
                                         [&record](const AttemptRecord& candidate) {
                                           return candidate.id == record.id;
                                         });
      if (existing != state.attempts.end()) {
        *existing = record;
        return ok_status();
      }
      if (state.attempts.size() >= limits::max_attempts) {
        const auto evictable = std::find_if(state.attempts.begin(), state.attempts.end(),
                                            [](const AttemptRecord& candidate) {
                                              return is_terminal_state(candidate.state);
                                            });
        if (evictable == state.attempts.end()) {
          return Status(Code::table_full, Reason::table_full, "attempt table full");
        }
        state.attempts.erase(evictable);
        ++state.attempts_dropped;
      }
      state.attempts.push_back(record);
      return ok_status();
    }
    case RecordType::lineage_entry: {
      auto value = wire::get_lineage(reader);
      if (!value.ok()) {
        return value.status();
      }
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      if (state.lineage.size() >= limits::max_lineage) {
        state.lineage.erase(state.lineage.begin());
        state.lineage_truncated = true;
      }
      state.lineage.push_back(value.value());
      return ok_status();
    }
    case RecordType::epoch_advance: {
      auto authority = wire::get_authority(reader);
      if (!authority.ok()) {
        return authority.status();
      }
      const std::uint64_t restart_count = reader.u64();
      const std::uint64_t attempt_sequence = reader.u64();
      const std::uint64_t fence_sequence = reader.u64();
      const std::uint64_t decision_sequence = reader.u64();
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      state.authority = authority.value();
      state.restart_count = restart_count;
      state.attempt_sequence = std::max(state.attempt_sequence, attempt_sequence);
      state.fence_sequence = std::max(state.fence_sequence, fence_sequence);
      state.decision_sequence = std::max(state.decision_sequence, decision_sequence);
      return ok_status();
    }
    case RecordType::snapshot_marker: {
      const std::uint64_t marker = reader.u64();
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      (void)marker;
      return ok_status();
    }
    case RecordType::counters: {
      state.attempt_sequence = std::max(state.attempt_sequence, reader.u64());
      state.fence_sequence = std::max(state.fence_sequence, reader.u64());
      state.decision_sequence = std::max(state.decision_sequence, reader.u64());
      state.fence_overflow = reader.u64();
      state.attempts_dropped = reader.u64();
      const bool truncated = reader.boolean();
      Status end = end_of(reader);
      if (!end.ok()) {
        return end;
      }
      state.lineage_truncated = state.lineage_truncated || truncated;
      return ok_status();
    }
  }
  return Status(Code::invalid, Reason::record_type_invalid, "unknown record type");
}

// ---- snapshot --------------------------------------------------------------

std::vector<std::byte> encode_snapshot(const DurableState& state) {
  ByteWriter body(4096, limits::max_snapshot_bytes);
  std::uint32_t record_count = 0;
  std::uint64_t frame_sequence = 0;

  bool encoding_failed = false;
  const auto emit = [&](RecordType type, const std::vector<std::byte>& payload) {
    if (payload.empty()) {
      encoding_failed = true;
      return;
    }
    const std::vector<std::byte> record = encode_journal_record(type, ++frame_sequence, payload);
    if (record.empty()) {
      encoding_failed = true;
      return;
    }
    body.bytes(record);
    ++record_count;
  };

  emit(RecordType::counters, encode_counters_record(state));
  emit(RecordType::epoch_advance,
       encode_epoch_record(state.authority, state.restart_count, state.attempt_sequence,
                           state.fence_sequence, state.decision_sequence));
  if (state.has_policy) {
    emit(RecordType::policy_set, encode_policy_record(state.policy));
  }
  if (state.has_obligations) {
    emit(RecordType::obligations_set, encode_obligations_record(state.obligations));
  }
  for (const AlternateSet& set : state.sets) {
    emit(RecordType::alternate_set_put, encode_set_record(set));
  }
  for (const FenceRecord& fence : state.fences) {
    emit(RecordType::fence_recorded, encode_fence_record(fence));
  }
  for (const AttemptRecord& attempt : state.attempts) {
    emit(RecordType::attempt_state, encode_attempt_record(attempt));
  }
  for (const LineageEntry& entry : state.lineage) {
    emit(RecordType::lineage_entry, encode_lineage_record(entry));
  }
  if (!body.ok() || encoding_failed) {
    return {};
  }

  ByteWriter out(snapshot_header_bytes + body.size() + 8, limits::max_snapshot_bytes + 64);
  out.u32(snapshot_magic);
  out.u16(durable_format_version);
  out.u16(0);
  out.u64(state.last_sequence);
  out.u32(record_count);
  out.u32(static_cast<std::uint32_t>(body.size()));
  out.u32(0);
  out.u32(0);
  if (!out.ok()) {
    return {};
  }
  out.u32(crc32c(out.data()));
  out.u32(0);
  out.bytes(body.data());
  if (!out.ok()) {
    return {};
  }
  out.u32(crc32c(out.data()));
  out.u32(snapshot_footer_magic);
  return finish(out);
}

Result<DurableState> decode_snapshot(std::span<const std::byte> bytes) {
  if (bytes.size() < snapshot_header_bytes + 8) {
    return Status(Code::corrupt, Reason::length_invalid, "snapshot shorter than header");
  }
  ByteReader header(bytes.first(snapshot_header_bytes));
  const std::uint32_t magic = header.u32();
  const std::uint16_t version = header.u16();
  const std::uint16_t flags = header.u16();
  const std::uint64_t sequence = header.u64();
  const std::uint32_t record_count = header.u32();
  const std::uint32_t payload_length = header.u32();
  (void)header.u32();
  (void)header.u32();
  const std::uint32_t header_crc = header.u32();
  (void)header.u32();
  if (!header.ok() || !header.at_end()) {
    return Status(Code::corrupt, Reason::corrupt_header, "snapshot header truncated");
  }
  if (magic != snapshot_magic) {
    return Status(Code::corrupt, Reason::format_magic_mismatch, "snapshot magic mismatch");
  }
  if (version != durable_format_version) {
    return Status(Code::version_unsupported, Reason::unsupported_version,
                  "snapshot format version unsupported");
  }
  if (flags != 0) {
    return Status(Code::invalid, Reason::domain_invalid, "snapshot flags must be zero");
  }
  if (payload_length > limits::max_snapshot_bytes) {
    return Status(Code::corrupt, Reason::length_invalid, "snapshot payload length exceeds bound");
  }
  if (record_count > limits::max_snapshot_records) {
    return Status(Code::corrupt, Reason::length_invalid, "snapshot record count exceeds bound");
  }
  if (crc32c(bytes.first(kSnapshotHeaderCrcOffset)) != header_crc) {
    return Status(Code::corrupt, Reason::corrupt_header, "snapshot header integrity failure");
  }
  const std::size_t expected_size =
      snapshot_header_bytes + static_cast<std::size_t>(payload_length) + 8;
  if (bytes.size() > expected_size) {
    return Status(Code::corrupt, Reason::trailing_garbage, "trailing bytes after snapshot");
  }
  if (bytes.size() < expected_size) {
    return Status(Code::corrupt, Reason::length_invalid, "snapshot truncated");
  }
  const std::uint32_t stored_total_crc = read_u32_at(bytes, snapshot_header_bytes + payload_length);
  const std::uint32_t computed_total_crc = crc32c(bytes.first(snapshot_header_bytes + payload_length));
  if (stored_total_crc != computed_total_crc) {
    return Status(Code::corrupt, Reason::integrity_failure, "snapshot integrity failure");
  }
  const std::uint32_t footer =
      read_u32_at(bytes, snapshot_header_bytes + payload_length + 4);
  if (footer != snapshot_footer_magic) {
    return Status(Code::corrupt, Reason::corrupt_header, "snapshot footer magic mismatch");
  }

  DurableState state;
  state.last_sequence = sequence;
  const std::span<const std::byte> body =
      bytes.subspan(snapshot_header_bytes, static_cast<std::size_t>(payload_length));
  std::size_t offset = 0;
  std::uint32_t seen = 0;
  while (offset < body.size()) {
    if (body.size() - offset < journal_header_bytes) {
      return Status(Code::corrupt, Reason::length_invalid, "snapshot record header truncated");
    }
    auto record_header = decode_journal_header(body.subspan(offset, journal_header_bytes));
    if (!record_header.ok()) {
      return record_header.status();
    }
    const std::size_t record_total =
        journal_header_bytes + static_cast<std::size_t>(record_header.value().payload_length) +
        journal_trailer_bytes;
    if (body.size() - offset < record_total) {
      return Status(Code::corrupt, Reason::length_invalid, "snapshot record truncated");
    }
    const std::span<const std::byte> payload =
        body.subspan(offset + journal_header_bytes, record_header.value().payload_length);
    const std::uint32_t stored_payload_crc =
        read_u32_at(body, offset + journal_header_bytes + record_header.value().payload_length);
    if (stored_payload_crc != crc32c(payload)) {
      return Status(Code::corrupt, Reason::integrity_failure, "snapshot record integrity failure");
    }
    Status applied =
        apply_record(state, static_cast<RecordType>(record_header.value().type), payload);
    if (!applied.ok()) {
      return applied;
    }
    offset += record_total;
    ++seen;
  }
  if (seen != record_count) {
    return Status(Code::corrupt, Reason::state_inconsistent,
                  "snapshot record count does not match payload");
  }
  return state;
}

// ---- store -----------------------------------------------------------------

Store::~Store() { static_cast<void>(close()); }

Result<std::unique_ptr<Store>> Store::open(const StoreOptions& options) {
  auto store = std::unique_ptr<Store>(new Store());
  store->directory_ = options.directory;
  store->inspection_only_ = options.inspection_only;
  store->journal_path_ = store->directory_ / "fabric.journal";
  store->snapshot_path_ = store->directory_ / "fabric.snapshot";
  store->lock_path_ = store->directory_ / "fabric.lock";

  if (options.create_directory) {
    std::error_code ec;
    std::filesystem::create_directories(store->directory_, ec);
    if (!platform::file_exists(store->directory_)) {
      return Status(Code::io_failure, Reason::io_failure, "cannot create state directory");
    }
  }
  Status status = store->open_files();
  if (!status.ok()) {
    return status;
  }
  return store;
}

Status Store::open_files() {
  if (!inspection_only_) {
    lock_handle_ = platform::acquire_lock(lock_path_);
    if (lock_handle_ == nullptr) {
      return Status(Code::refused, Reason::directory_locked,
                    "another runtime already owns this state directory");
    }
  }
  journal_ = platform::open_file(journal_path_, "r+b");
  if (journal_ == nullptr) {
    recovery_.journal_absent = true;
    if (inspection_only_) {
      return ok_status();
    }
    journal_ = platform::open_file(journal_path_, "w+b");
    if (journal_ == nullptr) {
      return Status(Code::io_failure, Reason::io_failure, "cannot create journal");
    }
  }
  return ok_status();
}

Result<DurableState> Store::load() {
  if (closed_) {
    return Status(Code::invalid, Reason::state_inconsistent, "store already closed");
  }
  DurableState state;
  Status snapshot = load_snapshot(state);
  if (!snapshot.ok()) {
    return snapshot;
  }
  Status replay = replay_journal(state);
  if (!replay.ok()) {
    return replay;
  }
  return state;
}

Status Store::load_snapshot(DurableState& state) {
  if (!platform::file_exists(snapshot_path_)) {
    recovery_.snapshot_loaded = false;
    last_sequence_ = 0;
    return ok_status();
  }
  std::uint64_t size = 0;
  if (!platform::file_size(snapshot_path_, size)) {
    return Status(Code::io_failure, Reason::io_failure, "cannot stat snapshot");
  }
  if (size > limits::max_snapshot_bytes + 64) {
    return Status(Code::corrupt, Reason::length_invalid, "snapshot file exceeds bound");
  }
  std::FILE* file = platform::open_file(snapshot_path_, "rb");
  if (file == nullptr) {
    return Status(Code::io_failure, Reason::io_failure, "cannot open snapshot");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  const std::size_t read = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
  std::fclose(file);
  if (read != bytes.size()) {
    return Status(Code::io_failure, Reason::io_failure, "short read on snapshot");
  }
  auto decoded = decode_snapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
  if (!decoded.ok()) {
    return decoded.status();
  }
  state = std::move(decoded).value();
  last_sequence_ = state.last_sequence;
  recovery_.snapshot_loaded = true;
  recovery_.snapshot_sequence = state.last_sequence;
  return ok_status();
}

Status Store::replay_journal(DurableState& state) {
  last_sequence_ = state.last_sequence;
  if (journal_ == nullptr) {
    recovery_.journal_absent = true;
    return ok_status();
  }
  std::uint64_t size = 0;
  if (!platform::file_size(journal_path_, size)) {
    recovery_.journal_absent = true;
    return ok_status();
  }
  recovery_.journal_bytes = size;
  if (size == 0) {
    return ok_status();
  }
  if (std::fseek(journal_, 0, SEEK_SET) != 0) {
    return Status(Code::io_failure, Reason::io_failure, "cannot seek journal");
  }

  std::vector<std::byte> header_bytes(journal_header_bytes);
  std::vector<std::byte> payload;
  std::uint64_t position = 0;
  std::uint64_t truncated_at = size;
  bool torn = false;

  while (position < size) {
    const std::uint64_t remaining = size - position;
    if (remaining < journal_header_bytes) {
      torn = true;
      truncated_at = position;
      break;
    }
    if (std::fread(header_bytes.data(), 1, journal_header_bytes, journal_) !=
        journal_header_bytes) {
      return Status(Code::io_failure, Reason::io_failure, "short read on journal header");
    }
    auto header = decode_journal_header(std::span<const std::byte>(header_bytes.data(),
                                                                   header_bytes.size()));
    if (!header.ok()) {
      // A fully present but invalid header is never silently repaired: it can
      // equally be corruption or tampering.
      return header.status();
    }
    const std::uint64_t record_total = journal_header_bytes +
                                       static_cast<std::uint64_t>(header.value().payload_length) +
                                       journal_trailer_bytes;
    if (remaining < record_total) {
      torn = true;
      truncated_at = position;
      break;
    }
    payload.assign(header.value().payload_length, std::byte{0});
    if (!payload.empty() &&
        std::fread(payload.data(), 1, payload.size(), journal_) != payload.size()) {
      return Status(Code::io_failure, Reason::io_failure, "short read on journal payload");
    }
    std::byte crc_bytes[journal_trailer_bytes] = {};
    if (std::fread(crc_bytes, 1, journal_trailer_bytes, journal_) != journal_trailer_bytes) {
      return Status(Code::io_failure, Reason::io_failure, "short read on journal trailer");
    }
    const std::uint32_t stored_crc = read_u32_at(
        std::span<const std::byte>(crc_bytes, journal_trailer_bytes), 0);
    if (stored_crc != crc32c(std::span<const std::byte>(payload.data(), payload.size()))) {
      return Status(Code::corrupt, Reason::integrity_failure, "journal payload integrity failure");
    }
    if (header.value().sequence <= state.last_sequence) {
      ++recovery_.records_skipped;
      position += record_total;
      continue;
    }
    if (header.value().sequence != state.last_sequence + 1) {
      return Status(Code::corrupt, Reason::sequence_regression,
                    "journal sequence is not contiguous");
    }
    Status applied =
        apply_record(state, static_cast<RecordType>(header.value().type),
                     std::span<const std::byte>(payload.data(), payload.size()));
    if (!applied.ok()) {
      return applied;
    }
    state.last_sequence = header.value().sequence;
    ++recovery_.records_replayed;
    position += record_total;
  }

  last_sequence_ = state.last_sequence;
  recovery_.journal_bytes = truncated_at;
  if (torn) {
    recovery_.torn_tail_present = true;
    recovery_.truncated_bytes = size - truncated_at;
    if (!inspection_only_) {
      if (!platform::truncate_stream(journal_, truncated_at)) {
        return Status(Code::io_failure, Reason::io_failure, "cannot truncate torn journal tail");
      }
      recovery_.torn_tail_recovered = true;
    }
  } else {
    truncated_at = size;
  }
  if (std::fseek(journal_, 0, SEEK_END) != 0) {
    return Status(Code::io_failure, Reason::io_failure, "cannot seek journal end");
  }
  journal_bytes_ = truncated_at;
  return ok_status();
}

Status Store::append(RecordType type, std::span<const std::byte> payload) {
  if (closed_) {
    return Status(Code::invalid, Reason::state_inconsistent, "store already closed");
  }
  if (inspection_only_) {
    return Status(Code::refused, Reason::read_only, "store opened for inspection only");
  }
  if (payload.size() > limits::max_record_payload) {
    return Status(Code::invalid, Reason::length_invalid, "record payload exceeds bound");
  }
  const std::uint64_t header_and_trailer = journal_header_bytes + journal_trailer_bytes;
  if (journal_bytes_ + payload.size() + header_and_trailer > limits::max_journal_bytes) {
    return Status(Code::exhausted, Reason::table_full, "journal bound reached");
  }
  if (last_sequence_ == UINT64_MAX) {
    return Status(Code::exhausted, Reason::table_full, "journal sequence exhausted");
  }
  const std::uint64_t sequence = last_sequence_ + 1;
  const std::vector<std::byte> record = encode_journal_record(type, sequence, payload);
  if (record.empty()) {
    return Status(Code::invalid, Reason::invalid_payload, "record encoding failed");
  }
  if (std::fseek(journal_, 0, SEEK_END) != 0) {
    return Status(Code::io_failure, Reason::io_failure, "cannot seek journal end");
  }
  if (std::fwrite(record.data(), 1, record.size(), journal_) != record.size()) {
    return Status(Code::io_failure, Reason::io_failure, "cannot write journal record");
  }
  // Append, flush, then push to stable storage before reporting success.
  if (!platform::sync_stream(journal_)) {
    return Status(Code::io_failure, Reason::io_failure, "cannot flush journal record");
  }
  journal_bytes_ += record.size();
  last_sequence_ = sequence;
  return ok_status();
}

Status Store::write_snapshot(const DurableState& state) {
  if (closed_) {
    return Status(Code::invalid, Reason::state_inconsistent, "store already closed");
  }
  if (inspection_only_) {
    return Status(Code::refused, Reason::read_only, "store opened for inspection only");
  }
  if (state.last_sequence != last_sequence_) {
    return Status(Code::invalid, Reason::state_inconsistent,
                  "snapshot would discard journal records");
  }
  const std::vector<std::byte> bytes = encode_snapshot(state);
  if (bytes.empty()) {
    return Status(Code::exhausted, Reason::allocation_denied, "snapshot encoding failed");
  }
  const std::filesystem::path staging = directory_ / "fabric.snapshot.tmp";
  std::FILE* file = platform::open_file(staging, "wb");
  if (file == nullptr) {
    return Status(Code::io_failure, Reason::io_failure, "cannot open snapshot staging file");
  }
  const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool synced = wrote && platform::sync_stream(file);
  std::fclose(file);
  if (!synced) {
    static_cast<void>(platform::remove_file(staging));
    return Status(Code::io_failure, Reason::io_failure, "cannot write snapshot staging file");
  }
  if (!platform::atomic_replace(staging, snapshot_path_)) {
    static_cast<void>(platform::remove_file(staging));
    return Status(Code::io_failure, Reason::io_failure, "cannot replace snapshot");
  }
  static_cast<void>(platform::sync_directory(directory_));

  if (journal_ != nullptr) {
    std::fclose(journal_);
    journal_ = platform::open_file(journal_path_, "w+b");
    if (journal_ == nullptr) {
      return Status(Code::io_failure, Reason::io_failure, "cannot reset journal");
    }
  }
  journal_bytes_ = 0;
  recovery_.snapshot_loaded = true;
  recovery_.snapshot_sequence = state.last_sequence;
  return ok_status();
}

void Store::reset_journal_for_test() {
  if (journal_ == nullptr || inspection_only_) {
    return;
  }
  if (!platform::truncate_stream(journal_, 0)) {
    closed_ = false;  // no durable change was made; keep the store usable
  }
  static_cast<void>(std::fseek(journal_, 0, SEEK_END));
  journal_bytes_ = 0;
}

Status Store::close() {
  if (closed_) {
    return ok_status();
  }
  closed_ = true;
  if (journal_ != nullptr) {
    static_cast<void>(std::fflush(journal_));
    std::fclose(journal_);
    journal_ = nullptr;
  }
  if (lock_handle_ != nullptr) {
    void* const handle = lock_handle_;
    lock_handle_ = nullptr;
    platform::release_lock(handle);
  }
  return ok_status();
}

}  // namespace pff
