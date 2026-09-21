// Path Failover Fabric - the deterministic selector.
//
// The selector is a pure function: no clock, no state, no randomness, no
// container-order dependence. Given the same inputs it produces the same
// decision document, byte for byte. It returns a RECOMMENDATION, never
// authority; Fabric::begin_promotion turns a selection into a grant.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <span>

#include "pff/decision.hpp"
#include "pff/model.hpp"

namespace pff {

struct SelectionInput {
  PathId incumbent;
  const AlternateSet* set = nullptr;
  const ServiceObligations* obligations = nullptr;
  const FailoverPolicy* policy = nullptr;
  // The authority vector the caller currently holds. A decision is legal only
  // against this exact vector, and it is embedded in the result.
  AuthorityVector current;
  // The set generation the caller believes it is evaluating. When present it
  // must match the set exactly, because a decision is legal only for one
  // concrete set generation.
  Generation expected_set_generation;
  // Paths carrying a durable fence. Fence state is authority-bearing: a fenced
  // path is definitely not promotable regardless of how good it looks.
  std::span<const PathId> fenced_paths;
  DecisionId decision_id;
  AttemptId attempt;
  Provenance provenance = Provenance::synthetic;
};

class Selector {
 public:
  // Total: returns a decision for every input. Invalid input yields an explicit
  // refusal outcome rather than an exception or a partial document.
  [[nodiscard]] static Result<PromotionDecision> evaluate(const SelectionInput& input);

  // Canonical total order over two assessments: score descending, then upstream
  // rank ascending, then path identity ascending. Path identity is unique within
  // a validated set, so the order is total and permutation-independent.
  [[nodiscard]] static bool canonical_less(const CandidateAssessment& a,
                                           const CandidateAssessment& b) noexcept;
};

}  // namespace pff
