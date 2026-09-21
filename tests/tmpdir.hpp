// Scratch directory helper for filesystem and process tests.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace pfftest {

class TempDir {
 public:
  explicit TempDir(const std::string& label) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t index = counter.fetch_add(1);
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    char buffer[96] = {};
    std::snprintf(buffer, sizeof(buffer), "pff-test-%llu-%llu",
                  static_cast<unsigned long long>(stamp & 0xFFFFFFull),
                  static_cast<unsigned long long>(index));
    path_ = std::filesystem::temp_directory_path() / (std::string(buffer) + "-" + label);
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] std::filesystem::path file(const std::string& name) const {
    return path_ / name;
  }

  void append_raw(const std::string& name, const std::vector<std::byte>& bytes) const {
    std::ofstream stream(file(name), std::ios::binary | std::ios::app);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  void write_raw(const std::string& name, const std::vector<std::byte>& bytes) const {
    std::ofstream stream(file(name), std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  [[nodiscard]] std::vector<std::byte> read_raw(const std::string& name) const {
    std::ifstream stream(file(name), std::ios::binary);
    std::vector<std::byte> bytes;
    char chunk[4096];
    while (stream.read(chunk, sizeof(chunk)) || stream.gcount() > 0) {
      const auto count = static_cast<std::size_t>(stream.gcount());
      for (std::size_t i = 0; i < count; ++i) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(chunk[i])));
      }
    }
    return bytes;
  }

 private:
  std::filesystem::path path_;
};

}  // namespace pfftest
