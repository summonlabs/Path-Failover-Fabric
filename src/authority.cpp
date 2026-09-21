// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/authority.hpp"

namespace pff {
namespace {

AuthorityClass merge(AuthorityClass a, AuthorityClass b) noexcept {
  return severity(a) >= severity(b) ? a : b;
}

}  // namespace

const char* to_string(AuthorityClass c) noexcept {
  switch (c) {
    case AuthorityClass::current:
      return "CURRENT";
    case AuthorityClass::stale:
      return "STALE";
    case AuthorityClass::unknown:
      return "UNKNOWN";
    case AuthorityClass::conflict:
      return "CONFLICT";
  }
  return "INVALID";
}

AuthorityClass classify_generation(Generation supplied, Generation current) noexcept {
  // An absent generation is not generation zero: it means the supplier could not
  // state which generation its claim belongs to, so the claim is UNKNOWN.
  if (!supplied.valid() || !current.valid()) {
    return AuthorityClass::unknown;
  }
  if (supplied == current) {
    return AuthorityClass::current;
  }
  return supplied < current ? AuthorityClass::stale : AuthorityClass::conflict;
}

AuthorityComparison classify(const AuthorityVector& supplied,
                             const AuthorityVector& current) noexcept {
  AuthorityComparison out;
  out.topology = classify_generation(supplied.topology, current.topology);
  out.path_authority = classify_generation(supplied.path_authority, current.path_authority);
  out.policy = classify_generation(supplied.policy, current.policy);
  out.obligations = classify_generation(supplied.obligations, current.obligations);

  if (!supplied.epoch.valid() || !current.epoch.valid()) {
    out.epoch = AuthorityClass::unknown;
  } else if (supplied.epoch == current.epoch) {
    out.epoch = AuthorityClass::current;
  } else {
    out.epoch = supplied.epoch < current.epoch ? AuthorityClass::stale : AuthorityClass::conflict;
  }

  if (!supplied.boot.valid() || !current.boot.valid()) {
    out.boot = AuthorityClass::unknown;
  } else if (supplied.boot == current.boot) {
    out.boot = AuthorityClass::current;
  } else {
    // A different incarnation is never "newer": it is simply not the current
    // one, so it is STALE rather than CONFLICT.
    out.boot = AuthorityClass::stale;
  }

  out.overall = out.topology;
  out.overall = merge(out.overall, out.path_authority);
  out.overall = merge(out.overall, out.policy);
  out.overall = merge(out.overall, out.obligations);
  out.overall = merge(out.overall, out.epoch);
  out.overall = merge(out.overall, out.boot);

  switch (out.overall) {
    case AuthorityClass::current:
      out.reason = Reason::authority_current;
      break;
    case AuthorityClass::conflict:
      out.reason = Reason::generation_conflict;
      break;
    case AuthorityClass::unknown:
      out.reason = Reason::authority_unknown;
      break;
    case AuthorityClass::stale:
      if (out.epoch == AuthorityClass::stale) {
        out.reason = Reason::epoch_stale;
      } else if (out.boot == AuthorityClass::stale) {
        out.reason = Reason::boot_mismatch;
      } else if (out.topology == AuthorityClass::stale) {
        out.reason = Reason::topology_generation_stale;
      } else if (out.path_authority == AuthorityClass::stale) {
        out.reason = Reason::path_authority_generation_stale;
      } else if (out.policy == AuthorityClass::stale) {
        out.reason = Reason::policy_generation_stale;
      } else if (out.obligations == AuthorityClass::stale) {
        out.reason = Reason::obligations_generation_stale;
      } else {
        out.reason = Reason::authority_stale;
      }
      break;
  }
  return out;
}

}  // namespace pff
