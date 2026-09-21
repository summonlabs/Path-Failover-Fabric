// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/decision.hpp"

#include "pff/canonical.hpp"
#include "pff/crc32c.hpp"
#include "pff/ids.hpp"
#include "pff/wire.hpp"

namespace pff {

const char* to_string(PromotionOutcome o) noexcept {
  switch (o) {
    case PromotionOutcome::authorized:
      return "AUTHORIZED";
    case PromotionOutcome::promoted:
      return "PROMOTED";
    case PromotionOutcome::refused_proven_no_alternate:
      return "REFUSED_PROVEN_NO_ALTERNATE";
    case PromotionOutcome::refused_invalid_input:
      return "REFUSED_INVALID_INPUT";
    case PromotionOutcome::refused_policy_invalid:
      return "REFUSED_POLICY_INVALID";
    case PromotionOutcome::refused_obligations_invalid:
      return "REFUSED_OBLIGATIONS_INVALID";
    case PromotionOutcome::refused_stale_authority:
      return "REFUSED_STALE_AUTHORITY";
    case PromotionOutcome::refused_conflict:
      return "REFUSED_CONFLICT";
    case PromotionOutcome::refused_fenced:
      return "REFUSED_FENCED";
    case PromotionOutcome::refused_resource_exhausted:
      return "REFUSED_RESOURCE_EXHAUSTED";
    case PromotionOutcome::refused_read_only:
      return "REFUSED_READ_ONLY";
    case PromotionOutcome::refused_already_owned:
      return "REFUSED_ALREADY_OWNED";
    case PromotionOutcome::refused_superseded_attempt:
      return "REFUSED_SUPERSEDED_ATTEMPT";
    case PromotionOutcome::indeterminate_incomplete_set:
      return "INDETERMINATE_INCOMPLETE_SET";
    case PromotionOutcome::indeterminate_unknown_member:
      return "INDETERMINATE_UNKNOWN_MEMBER";
    case PromotionOutcome::indeterminate_search_limit:
      return "INDETERMINATE_SEARCH_LIMIT";
    case PromotionOutcome::unsupported:
      return "UNSUPPORTED";
    case PromotionOutcome::reverted:
      return "REVERTED";
    case PromotionOutcome::rolled_back:
      return "ROLLED_BACK";
  }
  return "INVALID_OUTCOME";
}

const char* to_string(AuthorityKind k) noexcept {
  switch (k) {
    case AuthorityKind::observation:
      return "OBSERVATION";
    case AuthorityKind::eligibility:
      return "ELIGIBILITY";
    case AuthorityKind::recommendation:
      return "RECOMMENDATION";
    case AuthorityKind::acknowledgement:
      return "ACKNOWLEDGEMENT";
    case AuthorityKind::grant:
      return "GRANT";
    case AuthorityKind::verified_effect:
      return "VERIFIED_EFFECT";
  }
  return "UNSUPPORTED";
}

const char* to_string(CandidateVerdict v) noexcept {
  switch (v) {
    case CandidateVerdict::selected:
      return "SELECTED";
    case CandidateVerdict::promotable_not_selected:
      return "PROMOTABLE_NOT_SELECTED";
    case CandidateVerdict::excluded:
      return "EXCLUDED";
    case CandidateVerdict::indeterminate:
      return "INDETERMINATE";
  }
  return "INVALID_VERDICT";
}

std::vector<std::byte> canonical_bytes(const PromotionDecision& decision) {
  ByteWriter writer(256, limits::max_record_payload);
  wire::put(writer, decision);
  if (!writer.ok()) {
    return {};
  }
  return std::move(writer).take();
}

std::uint64_t decision_digest(const PromotionDecision& decision) noexcept {
  const std::vector<std::byte> bytes = canonical_bytes(decision);
  return fnv1a64(std::span<const std::byte>(bytes.data(), bytes.size()));
}

std::string describe(const PromotionDecision& decision) {
  std::string out;
  out.reserve(256);
  out += to_string(decision.outcome);
  out += " reason=";
  out += to_string(decision.reason);
  out += " incumbent=";
  out += id_hex(decision.incumbent);
  out += " selected=";
  out += decision.selected.valid() ? id_hex(decision.selected) : std::string("none");
  out += " examined=";
  out += std::to_string(decision.examined);
  out += "/";
  out += std::to_string(decision.supplied);
  out += " policy_gen=";
  out += id_hex(decision.policy_generation);
  out += " evidence=";
  out += to_string(decision.provenance);
  out += " authority=";
  out += to_string(decision.authority_kind);
  return out;
}

}  // namespace pff
