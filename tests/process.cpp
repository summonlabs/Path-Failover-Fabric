// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "process.hpp"

#include <cstring>

namespace pfftest {
namespace {

#ifdef _WIN32
std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::wstring quote(const std::string& argument) {
  std::wstring out = L"\"";
  for (const char character : argument) {
    if (character == '"') {
      out += L"\\\"";
    } else {
      out += static_cast<wchar_t>(static_cast<unsigned char>(character));
    }
  }
  out += L"\"";
  return out;
}
#endif

}  // namespace

void ChildProcess::move_from(ChildProcess& other) noexcept {
  valid_ = other.valid_;
  exited_ = other.exited_;
  exit_code_ = other.exit_code_;
  pending_ = std::move(other.pending_);
#ifdef _WIN32
  process_ = other.process_;
  pipe_ = other.pipe_;
  other.process_ = nullptr;
  other.pipe_ = nullptr;
#else
  pid_ = other.pid_;
  pipe_ = other.pipe_;
  other.pid_ = -1;
  other.pipe_ = -1;
#endif
  other.valid_ = false;
}

ChildProcess ChildProcess::spawn(const std::string& executable,
                                 const std::vector<std::string>& arguments) {
  ChildProcess child;
#ifdef _WIN32
  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (::CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
    return child;
  }
  ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  std::wstring command = quote(executable);
  for (const std::string& argument : arguments) {
    command += L" ";
    command += quote(argument);
  }

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION information = {};
  const std::wstring application = widen(executable);
  const BOOL created = ::CreateProcessW(application.c_str(), command.data(), nullptr, nullptr,
                                        TRUE, 0, nullptr, nullptr, &startup, &information);
  ::CloseHandle(write_end);
  if (created == 0) {
    ::CloseHandle(read_end);
    return child;
  }
  ::CloseHandle(information.hThread);
  child.process_ = information.hProcess;
  child.pipe_ = read_end;
  child.valid_ = true;
#else
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return child;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return child;
  }
  if (pid == 0) {
    ::close(fds[0]);
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(fds[1]);
  child.pid_ = static_cast<int>(pid);
  child.pipe_ = fds[0];
  child.valid_ = true;
#endif
  return child;
}

std::string ChildProcess::read_line() {
  if (!valid_) {
    return std::string();
  }
  while (true) {
    const std::size_t newline = pending_.find('\n');
    if (newline != std::string::npos) {
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
    char buffer[1] = {};
#ifdef _WIN32
    DWORD read = 0;
    if (::ReadFile(static_cast<HANDLE>(pipe_), buffer, 1, &read, nullptr) == 0 || read == 0) {
      break;
    }
#else
    const ssize_t read = ::read(pipe_, buffer, 1);
    if (read <= 0) {
      break;
    }
#endif
    pending_.push_back(buffer[0]);
  }
  std::string remainder;
  remainder.swap(pending_);
  if (!remainder.empty() && remainder.back() == '\r') {
    remainder.pop_back();
  }
  return remainder;
}

int ChildProcess::wait() {
  if (!valid_) {
    return -1;
  }
  if (exited_) {
    return exit_code_;
  }
#ifdef _WIN32
  ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == 0) {
    return -1;
  }
  exit_code_ = static_cast<int>(code);
#else
  int status = 0;
  if (::waitpid(pid_, &status, 0) < 0) {
    return -1;
  }
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  exited_ = true;
  return exit_code_;
}

void ChildProcess::terminate() {
  if (!valid_) {
    return;
  }
#ifdef _WIN32
  ::TerminateProcess(static_cast<HANDLE>(process_), 9);
#else
  ::kill(pid_, SIGKILL);
#endif
  static_cast<void>(wait());
}

void ChildProcess::close() {
  if (!valid_) {
    return;
  }
  if (!exited_) {
    terminate();
  }
#ifdef _WIN32
  if (pipe_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(pipe_));
    pipe_ = nullptr;
  }
  if (process_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
#else
  if (pipe_ >= 0) {
    ::close(pipe_);
    pipe_ = -1;
  }
#endif
  valid_ = false;
}

}  // namespace pfftest
