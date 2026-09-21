// Minimal command line helpers shared by the tools.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace pff::tools {

inline std::optional<std::string> arg_value(int argc, char** argv, std::string_view key) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] != nullptr && key == argv[i]) {
      return std::string(argv[i + 1]);
    }
  }
  return std::nullopt;
}

inline bool flag_present(int argc, char** argv, std::string_view key) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr && key == argv[i]) {
      return true;
    }
  }
  return false;
}

inline std::string arg_or(int argc, char** argv, std::string_view key, std::string fallback) {
  const auto value = arg_value(argc, argv, key);
  return value.has_value() ? *value : std::move(fallback);
}

inline std::uint64_t arg_u64(int argc, char** argv, std::string_view key, std::uint64_t fallback) {
  const auto value = arg_value(argc, argv, key);
  if (!value.has_value()) {
    return fallback;
  }
  return std::strtoull(value->c_str(), nullptr, 10);
}

inline std::optional<std::string> command(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == nullptr) {
      continue;
    }
    const std::string_view view(argv[i]);
    if (view.size() >= 2 && view[0] == '-' && view[1] == '-') {
      ++i;
      continue;
    }
    return std::string(view);
  }
  return std::nullopt;
}

}  // namespace pff::tools
