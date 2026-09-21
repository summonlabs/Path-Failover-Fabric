// Path Failover Fabric - strongly typed identities.
//
// Identities are distinct types: a PathId can never be passed where a
// Generation is expected, and no identity converts implicitly to an integer.
// Zero is reserved and means "absent"; every issued identity starts at one.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace pff {

template <typename Tag>
class Id {
 public:
  using rep = std::uint64_t;

  constexpr Id() noexcept = default;

  [[nodiscard]] static constexpr Id from_value(rep v) noexcept {
    Id id;
    id.value_ = v;
    return id;
  }

  [[nodiscard]] static constexpr Id absent() noexcept { return Id{}; }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool absent_value() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(Id a, Id b) noexcept { return a.value_ == b.value_; }
  friend constexpr auto operator<=>(Id a, Id b) noexcept { return a.value_ <=> b.value_; }

 private:
  rep value_ = 0;
};

struct PathTag;
struct AlternateSetTag;
struct GenerationTag;
struct EpochTag;
struct BootTag;
struct AttemptTag;
struct EvidenceTag;
struct FenceTag;
struct DecisionTag;
struct SessionTag;
struct GrantTag;
struct IncarnationTag;

using PathId = Id<PathTag>;
using AlternateSetId = Id<AlternateSetTag>;
using Generation = Id<GenerationTag>;
using Epoch = Id<EpochTag>;
using BootId = Id<BootTag>;
using AttemptId = Id<AttemptTag>;
using EvidenceId = Id<EvidenceTag>;
using FenceId = Id<FenceTag>;
using DecisionId = Id<DecisionTag>;
using SessionId = Id<SessionTag>;
using GrantId = Id<GrantTag>;
using IncarnationId = Id<IncarnationTag>;

// Hex rendering helper used by diagnostics and by canonical text output.
[[nodiscard]] std::string id_hex(std::uint64_t value);

template <typename Tag>
[[nodiscard]] std::string id_hex(Id<Tag> id) {
  return id_hex(id.value());
}

}  // namespace pff

namespace std {
template <typename Tag>
struct hash<pff::Id<Tag>> {
  std::size_t operator()(const pff::Id<Tag>& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};
}  // namespace std
