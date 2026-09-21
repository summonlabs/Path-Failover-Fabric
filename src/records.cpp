// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/records.hpp"

namespace pff {

const char* to_string(AttemptState s) noexcept {
  switch (s) {
    case AttemptState::claimed:
      return "CLAIMED";
    case AttemptState::authorized:
      return "AUTHORIZED";
    case AttemptState::effect_recorded:
      return "EFFECT_RECORDED";
    case AttemptState::committed:
      return "COMMITTED";
    case AttemptState::refused:
      return "REFUSED";
    case AttemptState::fenced:
      return "FENCED";
    case AttemptState::superseded:
      return "SUPERSEDED";
    case AttemptState::abandoned:
      return "ABANDONED";
    case AttemptState::rolled_back:
      return "ROLLED_BACK";
    case AttemptState::interrupted:
      return "INTERRUPTED";
  }
  return "INVALID";
}

bool is_terminal_state(AttemptState s) noexcept {
  switch (s) {
    case AttemptState::committed:
    case AttemptState::refused:
    case AttemptState::fenced:
    case AttemptState::superseded:
    case AttemptState::abandoned:
    case AttemptState::rolled_back:
    case AttemptState::interrupted:
      return true;
    case AttemptState::claimed:
    case AttemptState::authorized:
    case AttemptState::effect_recorded:
      return false;
  }
  return true;
}

bool is_active_state(AttemptState s) noexcept { return !is_terminal_state(s); }

bool is_grant_state(AttemptState s) noexcept {
  return s == AttemptState::authorized || s == AttemptState::effect_recorded;
}

}  // namespace pff
