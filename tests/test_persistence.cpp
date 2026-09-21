// Persistence suite: durable format, crash recovery, adversarial bytes.
//
// Every adversarial case works on real files through the real Store: corrupt
// headers, impossible lengths, regressed sequences, unsupported versions,
// trailing garbage, partial records and integrity failures.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <string>
#include <vector>

#include "framework.hpp"
#include "pff/crc32c.hpp"
#include "pff/store.hpp"
#include "pff/version.hpp"
#include "support.hpp"
#include "tmpdir.hpp"

using namespace pff;
using pfftest::TempDir;

namespace {

StoreOptions writer_options(const TempDir& dir) {
  StoreOptions options;
  options.directory = dir.path();
  options.inspection_only = false;
  return options;
}

StoreOptions inspector_options(const TempDir& dir) {
  StoreOptions options;
  options.directory = dir.path();
  options.inspection_only = true;
  options.create_directory = false;
  return options;
}

std::vector<std::byte> policy_payload(std::uint64_t generation) {
  return encode_policy_record(pfftest::make_policy(generation));
}

std::vector<std::byte> epoch_payload(const AuthorityVector& authority,
                                     std::uint64_t restart_count) {
  return encode_epoch_record(authority, restart_count, 0, 0, 0);
}

AlternateSet make_set(std::uint64_t id, std::uint64_t generation, std::size_t count) {
  AlternateSet set;
  set.id = AlternateSetId::from_value(id);
  set.generation = Generation::from_value(generation);
  set.completeness = SetCompleteness::complete;
  const AuthorityVector authority = pfftest::make_authority(1, 1, 1, 1, 1, 1);
  for (std::size_t index = 0; index < count; ++index) {
    set.candidates.push_back(pfftest::make_candidate(PathId::from_value(10 + index),
                                                     static_cast<std::uint32_t>(index), authority));
  }
  return set;
}

AttemptRecord make_attempt(std::uint64_t id, AttemptState state) {
  AttemptRecord attempt;
  attempt.id = AttemptId::from_value(id);
  attempt.epoch = Epoch::from_value(2);
  attempt.boot = BootId::from_value(7);
  attempt.incumbent = PathId::from_value(1);
  attempt.selected = PathId::from_value(10);
  attempt.set = AlternateSetId::from_value(3);
  attempt.set_generation = Generation::from_value(4);
  attempt.basis = pfftest::make_authority(1, 1, 1, 1, 2, 7);
  attempt.state = state;
  attempt.terminal_reason = Reason::none;
  attempt.begin_sequence = 1;
  return attempt;
}

}  // namespace

PFF_TEST(persistence, journal_round_trip_and_lineage) {
  TempDir dir("journal");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.ok());
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
    ServiceObligations obligations = pfftest::make_obligations(2);
    PFF_CHECK(store.value()->append(RecordType::obligations_set,
                                    encode_obligations_record(obligations))
                  .ok());
    const AlternateSet set = make_set(3, 4, 3);
    PFF_CHECK(store.value()->append(RecordType::alternate_set_put, encode_set_record(set)).ok());
    AttemptRecord attempt = make_attempt(1, AttemptState::authorized);
    PFF_CHECK(store.value()->append(RecordType::attempt_begin, encode_attempt_record(attempt)).ok());
    attempt.state = AttemptState::fenced;
    attempt.terminal_reason = Reason::authority_fenced;
    PFF_CHECK(store.value()->append(RecordType::attempt_state, encode_attempt_record(attempt)).ok());
    FenceRecord fence;
    fence.id = FenceId::from_value(1);
    fence.epoch = Epoch::from_value(2);
    fence.boot = BootId::from_value(7);
    fence.attempt = AttemptId::from_value(1);
    fence.reason = Reason::authority_fenced;
    fence.at = attempt.basis;
    fence.sequence = 5;
    PFF_CHECK(store.value()->append(RecordType::fence_recorded, encode_fence_record(fence)).ok());
    LineageEntry entry;
    entry.decision = DecisionId::from_value(1);
    entry.attempt = AttemptId::from_value(1);
    entry.epoch = Epoch::from_value(2);
    entry.boot = BootId::from_value(7);
    entry.outcome = PromotionOutcome::authorized;
    entry.incumbent = PathId::from_value(1);
    entry.selected = PathId::from_value(10);
    entry.set = AlternateSetId::from_value(3);
    entry.basis = attempt.basis;
    entry.reason = Reason::selected;
    entry.sequence = 6;
    PFF_CHECK(store.value()->append(RecordType::lineage_entry, encode_lineage_record(entry)).ok());
    PFF_CHECK_EQ(store.value()->last_sequence(), std::uint64_t{7});
  }
  auto reopened = Store::open(writer_options(dir));
  PFF_CHECK(reopened.ok());
  auto state = reopened.value()->load();
  PFF_CHECK(state.ok());
  PFF_CHECK(state.value().has_policy);
  PFF_CHECK_EQ(state.value().policy.generation.value(), std::uint64_t{1});
  PFF_CHECK(state.value().has_obligations);
  PFF_CHECK_EQ(state.value().sets.size(), std::size_t{1});
  PFF_CHECK_EQ(state.value().sets[0].candidates.size(), std::size_t{3});
  PFF_CHECK_EQ(state.value().attempts.size(), std::size_t{1});
  PFF_CHECK(state.value().attempts[0].state == AttemptState::fenced);
  PFF_CHECK_EQ(state.value().fences.size(), std::size_t{1});
  PFF_CHECK_EQ(state.value().lineage.size(), std::size_t{1});
  PFF_CHECK(state.value().lineage[0].outcome == PromotionOutcome::promoted ||
            state.value().lineage[0].outcome == PromotionOutcome::authorized);
  PFF_CHECK_EQ(state.value().last_sequence, std::uint64_t{7});
  PFF_CHECK_EQ(reopened.value()->recovery().records_replayed, std::size_t{7});
  PFF_CHECK(!reopened.value()->recovery().torn_tail_present);
}

PFF_TEST(persistence, snapshot_replaces_journal_and_replays_forward) {
  TempDir dir("snapshot");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
    PFF_CHECK(store.value()->append(RecordType::alternate_set_put,
                                    encode_set_record(make_set(3, 1, 2)))
                  .ok());
    auto state = store.value()->load();
    PFF_CHECK(state.ok());
    PFF_CHECK(store.value()->write_snapshot(state.value()).ok());
    PFF_CHECK_EQ(store.value()->journal_bytes(), std::uint64_t{0});
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(2)).ok());
    PFF_CHECK_EQ(store.value()->last_sequence(), std::uint64_t{3});
  }
  auto reopened = Store::open(writer_options(dir));
  auto state = reopened.value()->load();
  PFF_CHECK(state.ok());
  PFF_CHECK(reopened.value()->recovery().snapshot_loaded);
  PFF_CHECK_EQ(reopened.value()->recovery().snapshot_sequence, std::uint64_t{2});
  PFF_CHECK_EQ(reopened.value()->recovery().records_replayed, std::size_t{1});
  PFF_CHECK_EQ(state.value().policy.generation.value(), std::uint64_t{2});
  PFF_CHECK_EQ(state.value().sets.size(), std::size_t{1});
  PFF_CHECK_EQ(state.value().last_sequence, std::uint64_t{3});
}

PFF_TEST(persistence, snapshot_refuses_to_discard_journal_records) {
  TempDir dir("snapshot-guard");
  auto store = Store::open(writer_options(dir));
  PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  auto state = store.value()->load();
  PFF_CHECK(state.ok());
  DurableState stale = state.value();
  stale.last_sequence = 0;
  const Status status = store.value()->write_snapshot(stale);
  PFF_CHECK(!status.ok());
  PFF_CHECK_EQ(status.reason, Reason::state_inconsistent);
}

PFF_TEST(persistence, partial_header_tail_is_recovered) {
  TempDir dir("torn-header");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  dir.append_raw("fabric.journal", std::vector<std::byte>(7, std::byte{0x5A}));

  auto store = Store::open(writer_options(dir));
  PFF_CHECK(store.ok());
  auto state = store.value()->load();
  PFF_CHECK(state.ok());
  PFF_CHECK(store.value()->recovery().torn_tail_present);
  PFF_CHECK(store.value()->recovery().torn_tail_recovered);
  PFF_CHECK_EQ(store.value()->recovery().truncated_bytes, std::uint64_t{7});
  PFF_CHECK(state.value().has_policy);
  PFF_CHECK_EQ(state.value().policy.generation.value(), std::uint64_t{1});
  PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(2)).ok());
}

PFF_TEST(persistence, partial_payload_tail_is_recovered) {
  TempDir dir("torn-payload");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  const std::vector<std::byte> payload = policy_payload(2);
  const std::vector<std::byte> record = encode_journal_record(RecordType::policy_set, 2, payload);
  // Keep a valid header plus half of the payload: a genuine torn write.
  const std::size_t keep = journal_header_bytes + payload.size() / 2;
  dir.append_raw("fabric.journal",
                 std::vector<std::byte>(record.begin(), record.begin() + static_cast<std::ptrdiff_t>(keep)));

  auto store = Store::open(writer_options(dir));
  auto state = store.value()->load();
  PFF_CHECK(state.ok());
  PFF_CHECK(store.value()->recovery().torn_tail_present);
  PFF_CHECK_EQ(store.value()->recovery().truncated_bytes, static_cast<std::uint64_t>(keep));
  PFF_CHECK(!state.value().has_policy || state.value().policy.generation.value() == 1);
}

PFF_TEST(persistence, complete_but_corrupt_header_is_refused) {
  TempDir dir("bad-header");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  std::vector<std::byte> header = encode_journal_record(RecordType::policy_set, 2, policy_payload(2));
  header.resize(journal_header_bytes);
  header[journal_header_bytes - 1] = static_cast<std::byte>(0x7F);  // corrupt header crc
  dir.append_raw("fabric.journal", header);

  auto store = Store::open(writer_options(dir));
  PFF_CHECK(store.ok());
  auto state = store.value()->load();
  PFF_CHECK(!state.ok());
  PFF_CHECK_EQ(state.status().reason, Reason::corrupt_header);
}

PFF_TEST(persistence, payload_bitflip_is_refused) {
  TempDir dir("bitflip");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(2)).ok());
  }
  std::vector<std::byte> bytes = dir.read_raw("fabric.journal");
  PFF_CHECK(bytes.size() > journal_header_bytes + 4);
  bytes[bytes.size() - 6] = static_cast<std::byte>(static_cast<unsigned char>(bytes[bytes.size() - 6]) ^ 0x01);
  dir.write_raw("fabric.journal", bytes);

  auto store = Store::open(writer_options(dir));
  auto state = store.value()->load();
  PFF_CHECK(!state.ok());
  PFF_CHECK_EQ(state.status().reason, Reason::integrity_failure);
}

PFF_TEST(persistence, sequence_regression_is_refused) {
  TempDir dir("sequence");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  // A second record that repeats sequence 1.
  dir.append_raw("fabric.journal",
                 encode_journal_record(RecordType::policy_set, 1, policy_payload(2)));
  auto store = Store::open(writer_options(dir));
  auto state = store.value()->load();
  PFF_CHECK(state.ok());  // an older sequence is skipped, not fatal
  PFF_CHECK_EQ(store.value()->recovery().records_skipped, std::size_t{1});
  PFF_CHECK_EQ(state.value().policy.generation.value(), std::uint64_t{1});

  // A gap is a regression and is refused.
  TempDir gap_dir("sequence-gap");
  {
    auto store2 = Store::open(writer_options(gap_dir));
    PFF_CHECK(store2.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  gap_dir.append_raw("fabric.journal",
                     encode_journal_record(RecordType::policy_set, 5, policy_payload(2)));
  auto store3 = Store::open(writer_options(gap_dir));
  auto gap_state = store3.value()->load();
  PFF_CHECK(!gap_state.ok());
  PFF_CHECK_EQ(gap_state.status().reason, Reason::sequence_regression);
}

PFF_TEST(persistence, unsupported_version_and_unknown_type_are_refused) {
  TempDir version_dir("version");
  {
    auto store = Store::open(writer_options(version_dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  std::vector<std::byte> record = encode_journal_record(RecordType::policy_set, 2, policy_payload(2));
  record[4] = static_cast<std::byte>(0x09);  // version
  std::vector<std::byte> header(record.begin(), record.begin() + journal_header_bytes);
  ByteWriter rewrite(32, 64);
  rewrite.u32(journal_magic);
  rewrite.u16(9);
  rewrite.u16(static_cast<std::uint16_t>(RecordType::policy_set));
  rewrite.u64(2);
  rewrite.u32(static_cast<std::uint32_t>(policy_payload(2).size()));
  rewrite.u32(crc32c(rewrite.data()));
  version_dir.append_raw("fabric.journal", std::move(rewrite).take());
  auto store = Store::open(writer_options(version_dir));
  auto state = store.value()->load();
  PFF_CHECK(!state.ok());
  PFF_CHECK_EQ(state.status().reason, Reason::unsupported_version);

  TempDir type_dir("type");
  {
    auto store2 = Store::open(writer_options(type_dir));
    PFF_CHECK(store2.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  ByteWriter bad_type(32, 64);
  bad_type.u32(journal_magic);
  bad_type.u16(durable_format_version);
  bad_type.u16(0x00FFu);
  bad_type.u64(2);
  bad_type.u32(0);
  bad_type.u32(crc32c(bad_type.data()));
  type_dir.append_raw("fabric.journal", std::move(bad_type).take());
  auto store3 = Store::open(writer_options(type_dir));
  auto state3 = store3.value()->load();
  PFF_CHECK(!state3.ok());
  PFF_CHECK_EQ(state3.status().reason, Reason::record_type_invalid);
}

PFF_TEST(persistence, impossible_declared_length_is_refused) {
  TempDir dir("length");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  ByteWriter header(32, 64);
  header.u32(journal_magic);
  header.u16(durable_format_version);
  header.u16(static_cast<std::uint16_t>(RecordType::policy_set));
  header.u64(2);
  header.u32(0xFFFFFFF0u);  // far above the record bound
  header.u32(crc32c(header.data()));
  dir.append_raw("fabric.journal", std::move(header).take());

  auto store = Store::open(writer_options(dir));
  auto state = store.value()->load();
  PFF_CHECK(!state.ok());
  PFF_CHECK_EQ(state.status().reason, Reason::length_invalid);
}

PFF_TEST(persistence, trailing_garbage_after_snapshot_is_refused) {
  TempDir dir("snapshot-trailing");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
    auto state = store.value()->load();
    PFF_CHECK(store.value()->write_snapshot(state.value()).ok());
  }
  std::vector<std::byte> bytes = dir.read_raw("fabric.snapshot");
  PFF_CHECK(!bytes.empty());
  bytes.push_back(static_cast<std::byte>(0x11));
  dir.write_raw("fabric.snapshot", bytes);
  auto store = Store::open(writer_options(dir));
  auto state = store.value()->load();
  PFF_CHECK(!state.ok());
  PFF_CHECK_EQ(state.status().reason, Reason::trailing_garbage);
}

PFF_TEST(persistence, snapshot_field_corruption_is_refused) {
  TempDir dir("snapshot-fields");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
    auto state = store.value()->load();
    PFF_CHECK(store.value()->write_snapshot(state.value()).ok());
  }
  const std::vector<std::byte> pristine = dir.read_raw("fabric.snapshot");
  PFF_CHECK(pristine.size() > snapshot_header_bytes + 8);

  // Magic.
  std::vector<std::byte> magic = pristine;
  magic[0] = static_cast<std::byte>(0x00);
  PFF_CHECK_EQ(decode_snapshot(std::span<const std::byte>(magic.data(), magic.size())).status().reason,
               Reason::format_magic_mismatch);
  // Version.
  std::vector<std::byte> version = pristine;
  version[4] = static_cast<std::byte>(0x7F);
  PFF_CHECK_EQ(decode_snapshot(std::span<const std::byte>(version.data(), version.size())).status().reason,
               Reason::unsupported_version);
  // Flags must be zero.
  std::vector<std::byte> flags = pristine;
  flags[6] = static_cast<std::byte>(0x01);
  PFF_CHECK_EQ(decode_snapshot(std::span<const std::byte>(flags.data(), flags.size())).status().reason,
               Reason::domain_invalid);
  // Declared payload length beyond the bound.
  std::vector<std::byte> length = pristine;
  length[20] = static_cast<std::byte>(0xFF);
  length[21] = static_cast<std::byte>(0xFF);
  length[22] = static_cast<std::byte>(0xFF);
  length[23] = static_cast<std::byte>(0x7F);
  // The declared length is validated before anything is sized or allocated, so
  // an impossible length is reported as a length failure even though the header
  // integrity field is also stale.
  PFF_CHECK_EQ(decode_snapshot(std::span<const std::byte>(length.data(), length.size())).status().reason,
               Reason::length_invalid);
  // Header integrity.
  std::vector<std::byte> header_crc = pristine;
  header_crc[32] = static_cast<std::byte>(static_cast<unsigned char>(header_crc[32]) ^ 0xFF);
  PFF_CHECK_EQ(decode_snapshot(std::span<const std::byte>(header_crc.data(), header_crc.size())).status().reason,
               Reason::corrupt_header);
  // Total integrity.
  std::vector<std::byte> total = pristine;
  total[total.size() - 8] = static_cast<std::byte>(static_cast<unsigned char>(total[total.size() - 8]) ^ 0xFF);
  PFF_CHECK_EQ(decode_snapshot(std::span<const std::byte>(total.data(), total.size())).status().reason,
               Reason::integrity_failure);
  // Footer magic.
  std::vector<std::byte> footer = pristine;
  footer[footer.size() - 1] = static_cast<std::byte>(0x00);
  // The footer is covered by nothing, so only the magic check can catch it.
  PFF_CHECK_EQ(decode_snapshot(std::span<const std::byte>(footer.data(), footer.size())).status().reason,
               Reason::corrupt_header);
  // Truncation.
  std::vector<std::byte> short_bytes(pristine.begin(), pristine.end() - 3);
  PFF_CHECK_EQ(decode_snapshot(std::span<const std::byte>(short_bytes.data(), short_bytes.size())).status().reason,
               Reason::length_invalid);
  // A valid document still decodes.
  PFF_CHECK(decode_snapshot(std::span<const std::byte>(pristine.data(), pristine.size())).ok());
}

PFF_TEST(persistence, snapshot_record_count_mismatch_is_refused) {
  TempDir dir("snapshot-count");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
    auto state = store.value()->load();
    PFF_CHECK(store.value()->write_snapshot(state.value()).ok());
  }
  std::vector<std::byte> bytes = dir.read_raw("fabric.snapshot");
  // record_count lives at offset 16..19.
  bytes[16] = static_cast<std::byte>(static_cast<unsigned char>(bytes[16]) + 1);
  // Recompute the header crc so the count check is what refuses the document.
  ByteWriter write_back(0, 64);
  write_back.bytes(std::span<const std::byte>(bytes.data(), 32));
  const std::uint32_t new_crc = crc32c(write_back.data());
  bytes[32] = static_cast<std::byte>(new_crc & 0xFFu);
  bytes[33] = static_cast<std::byte>((new_crc >> 8) & 0xFFu);
  bytes[34] = static_cast<std::byte>((new_crc >> 16) & 0xFFu);
  bytes[35] = static_cast<std::byte>((new_crc >> 24) & 0xFFu);
  const auto decoded = decode_snapshot(std::span<const std::byte>(bytes.data(), bytes.size()));
  PFF_CHECK(!decoded.ok());
  PFF_CHECK_EQ(decoded.status().reason, Reason::state_inconsistent);
}

PFF_TEST(persistence, apply_record_rejects_malformed_payloads) {
  DurableState state;
  const RecordType types[] = {
      RecordType::policy_set,      RecordType::obligations_set, RecordType::alternate_set_put,
      RecordType::fence_recorded,  RecordType::attempt_begin,   RecordType::attempt_state,
      RecordType::lineage_entry,   RecordType::epoch_advance,   RecordType::snapshot_marker,
      RecordType::counters,
  };
  const std::vector<std::byte> empty;
  for (const RecordType type : types) {
    const Status status = apply_record(state, type, std::span<const std::byte>(empty));
    PFF_CHECK_MSG(!status.ok(), to_string(type));
  }
  // Every truncated prefix of a valid payload must be refused.
  const std::vector<std::byte> payload = policy_payload(1);
  for (std::size_t length = 0; length < payload.size(); ++length) {
    DurableState target;
    const Status status = apply_record(target, RecordType::policy_set,
                                       std::span<const std::byte>(payload.data(), length));
    PFF_CHECK(!status.ok());
    PFF_CHECK(!target.has_policy);
  }
  // Trailing bytes are refused too.
  std::vector<std::byte> extended = payload;
  extended.push_back(std::byte{0x00});
  PFF_CHECK(!apply_record(state, RecordType::policy_set,
                          std::span<const std::byte>(extended.data(), extended.size()))
                 .ok());
}

PFF_TEST(persistence, append_rejects_oversized_payload) {
  TempDir dir("oversize");
  auto store = Store::open(writer_options(dir));
  const std::vector<std::byte> oversized(limits::max_record_payload + 1, std::byte{0});
  const Status status = store.value()->append(RecordType::counters, oversized);
  PFF_CHECK(!status.ok());
  PFF_CHECK_EQ(status.reason, Reason::length_invalid);
}

PFF_TEST(persistence, inspection_mode_never_writes_or_repairs) {
  TempDir dir("inspect");
  {
    auto store = Store::open(writer_options(dir));
    PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(1)).ok());
  }
  dir.append_raw("fabric.journal", std::vector<std::byte>(9, std::byte{0x33}));
  const std::uint64_t before = std::filesystem::file_size(dir.file("fabric.journal"));

  auto inspector = Store::open(inspector_options(dir));
  PFF_CHECK(inspector.ok());
  auto state = inspector.value()->load();
  PFF_CHECK(state.ok());
  PFF_CHECK(inspector.value()->recovery().torn_tail_present);
  PFF_CHECK(!inspector.value()->recovery().torn_tail_recovered);
  PFF_CHECK_EQ(std::filesystem::file_size(dir.file("fabric.journal")), before);
  const Status append_status = inspector.value()->append(RecordType::counters, {});
  PFF_CHECK(!append_status.ok());
  PFF_CHECK_EQ(append_status.reason, Reason::read_only);
}

PFF_TEST(persistence, directory_lock_excludes_a_second_writer) {
  TempDir dir("lock");
  auto first = Store::open(writer_options(dir));
  PFF_CHECK(first.ok());
  auto second = Store::open(writer_options(dir));
  PFF_CHECK(!second.ok());
  PFF_CHECK_EQ(second.status().reason, Reason::directory_locked);
  static_cast<void>(first.value()->close());
  auto third = Store::open(writer_options(dir));
  PFF_CHECK(third.ok());
}

PFF_TEST(persistence, missing_directory_is_created_and_reported) {
  TempDir root("missing");
  StoreOptions options;
  options.directory = root.file("nested") / "state";
  auto store = Store::open(options);
  PFF_CHECK(store.ok());
  PFF_CHECK(std::filesystem::exists(options.directory));
  auto state = store.value()->load();
  PFF_CHECK(state.ok());
  PFF_CHECK(store.value()->recovery().journal_absent);
  PFF_CHECK_EQ(state.value().last_sequence, std::uint64_t{0});
}

PFF_TEST(persistence, state_reopens_after_snapshot_compaction_cycle) {
  TempDir dir("cycle");
  {
    auto store = Store::open(writer_options(dir));
    for (std::uint64_t generation = 1; generation <= 5; ++generation) {
      PFF_CHECK(store.value()->append(RecordType::policy_set, policy_payload(generation)).ok());
      auto state = store.value()->load();
      PFF_CHECK(store.value()->write_snapshot(state.value()).ok());
    }
    PFF_CHECK_EQ(store.value()->last_sequence(), std::uint64_t{5});
  }
  auto reopened = Store::open(writer_options(dir));
  auto state = reopened.value()->load();
  PFF_CHECK(state.ok());
  PFF_CHECK_EQ(state.value().policy.generation.value(), std::uint64_t{5});
  PFF_CHECK_EQ(state.value().last_sequence, std::uint64_t{5});
}

PFF_TEST_MAIN()
