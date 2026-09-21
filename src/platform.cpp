// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "platform.hpp"

#include <chrono>
#include <cstring>
#include <random>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pff::platform {

std::FILE* open_file(const std::filesystem::path& path, const char* mode) noexcept {
#ifdef _WIN32
  wchar_t wide_mode[8] = {};
  for (std::size_t i = 0; i < 7 && mode[i] != '\0'; ++i) {
    wide_mode[i] = static_cast<wchar_t>(mode[i]);
  }
  return ::_wfopen(path.c_str(), wide_mode);
#else
  return std::fopen(path.c_str(), mode);
#endif
}

bool sync_stream(std::FILE* stream) noexcept {
  if (stream == nullptr) {
    return false;
  }
  if (std::fflush(stream) != 0) {
    return false;
  }
#ifdef _WIN32
  return ::_commit(::_fileno(stream)) == 0;
#else
  return ::fsync(::fileno(stream)) == 0;
#endif
}

bool truncate_stream(std::FILE* stream, std::uint64_t length) noexcept {
  if (stream == nullptr) {
    return false;
  }
  if (std::fflush(stream) != 0) {
    return false;
  }
#ifdef _WIN32
  return ::_chsize_s(::_fileno(stream), static_cast<__int64>(length)) == 0;
#else
  return ::ftruncate(::fileno(stream), static_cast<off_t>(length)) == 0;
#endif
}

bool atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) noexcept {
#ifdef _WIN32
  return ::MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool sync_directory(const std::filesystem::path& directory) noexcept {
#ifdef _WIN32
  // Windows has no portable directory fsync. The replacement above used
  // MOVEFILE_WRITE_THROUGH, which is the strongest guarantee available here.
  (void)directory;
  return true;
#else
  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

void* acquire_lock(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
  HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return nullptr;
  }
  return reinterpret_cast<void*>(handle);
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) {
    return nullptr;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return nullptr;
  }
  return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
#endif
}

void release_lock(void* handle) noexcept {
  if (handle == nullptr) {
    return;
  }
#ifdef _WIN32
  ::CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
  ::flock(fd, LOCK_UN);
  ::close(fd);
#endif
}

std::uint64_t process_id() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t unix_micros() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::uint64_t random_u64() noexcept {
  std::random_device device;
  std::uint64_t value = (static_cast<std::uint64_t>(device()) << 32) ^
                        static_cast<std::uint64_t>(device());
  value ^= unix_micros();
  value ^= (process_id() << 17);
  value *= 0x9E3779B97F4A7C15ull;
  value ^= value >> 29;
  if (value == 0) {
    value = 0xA5A5A5A5A5A5A5A5ull;
  }
  return value;
}

bool file_size(const std::filesystem::path& path, std::uint64_t& size) noexcept {
  std::error_code ec;
  const auto value = std::filesystem::file_size(path, ec);
  if (ec) {
    return false;
  }
  size = static_cast<std::uint64_t>(value);
  return true;
}

bool file_exists(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

bool remove_file(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  return std::filesystem::remove(path, ec) && !ec;
}

}  // namespace pff::platform
