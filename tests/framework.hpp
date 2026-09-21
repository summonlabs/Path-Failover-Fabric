// Minimal deterministic test framework.
//
// No third party dependency, no test level timeouts, deterministic ordering and
// deterministic failure reporting. Every check records the exact expression.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace pfftest {

struct TestCase {
  std::string suite;
  std::string name;
  std::function<void()> body;
};

class Registry {
 public:
  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  void add(std::string suite, std::string name, std::function<void()> body) {
    cases_.push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
  }

  int run(const std::string& filter) {
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    for (const TestCase& test : cases_) {
      const std::string full = test.suite + "." + test.name;
      if (!filter.empty() && full.find(filter) == std::string::npos) {
        ++skipped;
        continue;
      }
      current_ = &test;
      failures_.clear();
      try {
        test.body();
      } catch (const std::exception& error) {
        failures_.push_back(std::string("uncaught exception: ") + error.what());
      } catch (...) {
        failures_.push_back("uncaught non standard exception");
      }
      if (failures_.empty()) {
        ++passed;
        std::printf("[ PASS ] %s\n", full.c_str());
      } else {
        ++failed;
        std::printf("[ FAIL ] %s\n", full.c_str());
        for (const std::string& failure : failures_) {
          std::printf("         %s\n", failure.c_str());
        }
      }
      std::fflush(stdout);
    }
    std::printf("\n%s: %zu passed, %zu failed, %zu filtered out (of %zu)\n",
                failed == 0 ? "SUITE OK" : "SUITE FAILED", passed, failed, skipped, cases_.size());
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
  }

  void report_failure(const char* file, int line, const std::string& message) {
    failures_.push_back(std::string(file) + ":" + std::to_string(line) + ": " + message);
  }

 private:
  std::vector<TestCase> cases_;
  std::vector<std::string> failures_;
  const TestCase* current_ = nullptr;
};

struct Registrar {
  Registrar(const char* suite, const char* name, std::function<void()> body) {
    Registry::instance().add(suite, name, std::move(body));
  }
};

}  // namespace pfftest

#define PFF_TEST(suite, name)                                                          \
  static void pff_test_body_##suite##_##name();                                        \
  static const pfftest::Registrar pff_test_registrar_##suite##_##name(                 \
      #suite, #name, &pff_test_body_##suite##_##name);                                 \
  static void pff_test_body_##suite##_##name()

#define PFF_CHECK(expr)                                                                \
  do {                                                                                 \
    if (!(expr)) {                                                                     \
      pfftest::Registry::instance().report_failure(__FILE__, __LINE__,                 \
                                                   "check failed: " #expr);            \
    }                                                                                  \
  } while (false)

// Values are copied on purpose: binding a reference would dangle when the
// operand is a member of a temporary (for example some_result().status().reason),
// which is a stack-use-after-scope that ASan catches and that a reference
// binding here would silently introduce.
#define PFF_CHECK_EQ(lhs, rhs)                                                         \
  do {                                                                                 \
    const auto pff_lhs_value = (lhs);                                                  \
    const auto pff_rhs_value = (rhs);                                                  \
    if (!(pff_lhs_value == pff_rhs_value)) {                                           \
      pfftest::Registry::instance().report_failure(__FILE__, __LINE__,                 \
                                                   "expected " #lhs " == " #rhs);      \
    }                                                                                  \
  } while (false)

// Reports and returns from the test body. Used wherever continuing would
// dereference a value that was not produced.
#define PFF_REQUIRE(expr)                                                                do {                                                                                     if (!(expr)) {                                                                           pfftest::Registry::instance().report_failure(__FILE__, __LINE__,                                                                    "requirement failed: " #expr);            return;                                                                              }                                                                                    } while (false)

#define PFF_CHECK_MSG(expr, message)                                                   \
  do {                                                                                 \
    if (!(expr)) {                                                                     \
      pfftest::Registry::instance().report_failure(                                    \
          __FILE__, __LINE__, std::string("check failed: " #expr ": ") + (message));   \
    }                                                                                  \
  } while (false)

#define PFF_TEST_MAIN()                                                                \
  int main(int argc, char** argv) {                                                    \
    const std::string filter = argc > 1 ? std::string(argv[1]) : std::string();        \
    return pfftest::Registry::instance().run(filter);                                  \
  }
