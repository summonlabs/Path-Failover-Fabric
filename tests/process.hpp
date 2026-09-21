// Real independent operating system process helper for the multiprocess proofs.
//
// On Windows the child is created with a redirected stdout pipe so the parent
// can read protocol lines, and it is terminated with TerminateProcess, which is
// a genuine hard kill. The POSIX path is implemented but has not been exercised
// on this host and is therefore labelled UNSUPPORTED in the evidence matrix.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pfftest {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() { close(); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept { move_from(other); }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      close();
      move_from(other);
    }
    return *this;
  }

  // Spawns an executable with the given arguments. The child's stdout and stderr
  // are captured through a pipe.
  static ChildProcess spawn(const std::string& executable,
                            const std::vector<std::string>& arguments);

  [[nodiscard]] bool valid() const { return valid_; }

  // Blocking single line read. Returns what is available at end of file.
  std::string read_line();

  // Blocking wait. Returns the exit code, or -1 if the process could not be
  // waited for.
  int wait();

  // Hard kill. Does not flush anything in the child.
  void terminate();

  [[nodiscard]] int exit_code() const { return exit_code_; }
  [[nodiscard]] bool exited() const { return exited_; }

 private:
  void close();
  void move_from(ChildProcess& other) noexcept;

  bool valid_ = false;
  bool exited_ = false;
  int exit_code_ = -1;
  std::string pending_;
#ifdef _WIN32
  void* process_ = nullptr;
  void* pipe_ = nullptr;
#else
  int pid_ = -1;
  int pipe_ = -1;
#endif
};

}  // namespace pfftest
