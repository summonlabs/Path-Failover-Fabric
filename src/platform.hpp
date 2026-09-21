// Internal platform helpers. Not installed.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace pff::platform {

// Opens a file with a wide-character path on Windows and a narrow path
// elsewhere, so non-ASCII state directories work.
[[nodiscard]] std::FILE* open_file(const std::filesystem::path& path, const char* mode) noexcept;

// Flushes and pushes a stream's contents to stable storage.
[[nodiscard]] bool sync_stream(std::FILE* stream) noexcept;

// Truncates an already open file to the given length.
[[nodiscard]] bool truncate_stream(std::FILE* stream, std::uint64_t length) noexcept;

// Atomically replaces the destination with the source.
[[nodiscard]] bool atomic_replace(const std::filesystem::path& from,
                                  const std::filesystem::path& to) noexcept;

// Best-effort directory metadata sync after a rename.
[[nodiscard]] bool sync_directory(const std::filesystem::path& directory) noexcept;

// Takes an exclusive advisory lock on the given path. Returns nullptr when the
// lock is already held by another process. The returned handle must be released
// with release_lock.
[[nodiscard]] void* acquire_lock(const std::filesystem::path& path) noexcept;
void release_lock(void* handle) noexcept;

[[nodiscard]] std::uint64_t process_id() noexcept;
[[nodiscard]] std::uint64_t unix_micros() noexcept;
[[nodiscard]] std::uint64_t random_u64() noexcept;

[[nodiscard]] bool file_size(const std::filesystem::path& path, std::uint64_t& size) noexcept;
[[nodiscard]] bool file_exists(const std::filesystem::path& path) noexcept;
[[nodiscard]] bool remove_file(const std::filesystem::path& path) noexcept;

}  // namespace pff::platform
