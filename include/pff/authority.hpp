// Path Failover Fabric - authority vectors and generation classification.
//
// Matching identity is not matching generation. A decision is legal only for the
// exact generations it was computed against, so every externally visible answer
// carries the authority vector that made it legal and every input carries the
// authority vector under which it was produced.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>

#include "pff/ids.hpp"
#include "pff/status.hpp"

namespace pff {

// How a supplied authority vector relates to the authority vector the runtime
// currently holds. The ordering of the enumerators is the precedence used when
// folding per-field classifications into one overall class: conflict is the most
// severe, then unknown, then stale, then current.
enum class AuthorityClass : std::uint8_t {
  current = 0,
  stale = 1,
  unknown = 2,
  conflict = 3,
};

const char* to_string(AuthorityClass c) noexcept;

// The exact set of generations that make a decision, a grant, an attempt or an
// observation legal.
struct AuthorityVector {
  Generation topology;
  Generation path_authority;
  Generation policy;
  Generation obligations;
  Epoch epoch;
  BootId boot;

  friend bool operator==(const AuthorityVector& a, const AuthorityVector& b) noexcept {
    return a.topology == b.topology && a.path_authority == b.path_authority &&
           a.policy == b.policy && a.obligations == b.obligations && a.epoch == b.epoch &&
           a.boot == b.boot;
  }
  friend bool operator!=(const AuthorityVector& a, const AuthorityVector& b) noexcept {
    return !(a == b);
  }
};

struct AuthorityComparison {
  AuthorityClass overall = AuthorityClass::unknown;
  AuthorityClass topology = AuthorityClass::unknown;
  AuthorityClass path_authority = AuthorityClass::unknown;
  AuthorityClass policy = AuthorityClass::unknown;
  AuthorityClass obligations = AuthorityClass::unknown;
  AuthorityClass epoch = AuthorityClass::unknown;
  AuthorityClass boot = AuthorityClass::unknown;
  Reason reason = Reason::none;

  [[nodiscard]] bool all_current() const noexcept { return overall == AuthorityClass::current; }
};

// Per-field classification of a generation counter.
[[nodiscard]] AuthorityClass classify_generation(Generation supplied, Generation current) noexcept;

// Full classification of a supplied vector against the current vector.
[[nodiscard]] AuthorityComparison classify(const AuthorityVector& supplied,
                                           const AuthorityVector& current) noexcept;

// The severity ordering used to fold per-field classes.
[[nodiscard]] constexpr std::uint8_t severity(AuthorityClass c) noexcept {
  return static_cast<std::uint8_t>(c);
}

}  // namespace pff
