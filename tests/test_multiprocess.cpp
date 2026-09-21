// Multiprocess, crash and restart suite.
//
// Every scenario here runs the real daemon, the real scenario worker and the real
// command line client as independent operating system processes over real
// loopback sockets, and hard kills them at meaningful durable boundaries.
// Threads are never used as a substitute for process proof.
//
// The worker reports its crash boundary by exiting with code 9 after flushing
// what it had already printed, so the parent can distinguish "reached the
// boundary" from "failed earlier".
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "framework.hpp"
#include "pff/fabric.hpp"
#include "pff/protocol.hpp"
#include "pff/server.hpp"
#include "process.hpp"
#include "support.hpp"
#include "tmpdir.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

using namespace pff;
using pfftest::ChildProcess;
using pfftest::TempDir;

namespace {

constexpr int kCrashExitCode = 9;

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string run_tool(const std::string& executable, const std::vector<std::string>& arguments,
                     int& exit_code) {
  ChildProcess child = ChildProcess::spawn(executable, arguments);
  PFF_CHECK(child.valid());
  std::string output;
  while (true) {
    const std::string line = child.read_line();
    if (line.empty()) {
      break;
    }
    output += line;
    output += "\n";
  }
  exit_code = child.wait();
  return output;
}

std::string host_scenario(const std::string& directory,
                          const std::vector<std::string>& arguments, int& exit_code) {
  std::vector<std::string> full = {"--dir", directory};
  full.insert(full.end(), arguments.begin(), arguments.end());
  const std::string output = run_tool(PFF_HOST_PATH, full, exit_code);
  std::printf("      host [%s] exit=%d\n", arguments.empty() ? "" : arguments[0].c_str(), exit_code);
  return output;
}

struct Daemon {
  ChildProcess process;
  std::uint16_t port = 0;
  std::string banner;

  bool start(const std::string& directory, const std::vector<std::string>& extra = {}) {
    std::vector<std::string> arguments = {"--dir", directory, "--port", "0"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    process = ChildProcess::spawn(PFF_DAEMON_PATH, arguments);
    if (!process.valid()) {
      return false;
    }
    const std::string listening = process.read_line();
    if (!contains(listening, "LISTENING ")) {
      banner = listening;
      return false;
    }
    port = static_cast<std::uint16_t>(
        std::stoul(listening.substr(std::string("LISTENING ").size())));
    const std::string identity = process.read_line();
    banner = listening + "\n" + identity;
    return port != 0;
  }
};

// Drives the adjacent systems through the real protocol: establish the topology
// and path authority generations, install policy and obligations, then supply
// the alternate set bound to the authority the coordinator now holds.
Result<AuthorityVector> install_baseline_over_client(Client& client, AlternateSetId set_id) {
  auto initial = client.status();
  if (!initial.ok()) {
    return initial.status();
  }
  AuthorityVector advance;
  advance.topology = Generation::from_value(initial.value().authority.topology.value() + 1);
  advance.path_authority = Generation::from_value(initial.value().authority.path_authority.value() + 1);
  Status status = client.set_authority(advance);
  if (!status.ok()) {
    return status;
  }
  FailoverPolicy policy = pfftest::make_policy(initial.value().authority.policy.value() + 1);
  status = client.set_policy(policy);
  if (!status.ok()) {
    return status;
  }
  ServiceObligations obligations =
      pfftest::make_obligations(initial.value().authority.obligations.value() + 1);
  status = client.set_obligations(obligations);
  if (!status.ok()) {
    return status;
  }
  auto installed = client.status();
  if (!installed.ok()) {
    return installed.status();
  }
  const AuthorityVector authority = installed.value().authority;
  AlternateSet set;
  set.id = set_id;
  set.generation = Generation::from_value(1);
  set.completeness = SetCompleteness::complete;
  for (std::size_t index = 0; index < 3; ++index) {
    AlternateCandidate candidate = pfftest::make_candidate(
        PathId::from_value(4100 + index), static_cast<std::uint32_t>(index), authority);
    candidate.evidence.health_ppm = static_cast<std::int64_t>(900000 - index);
    set.candidates.push_back(candidate);
  }
  status = client.put_set(set);
  if (!status.ok()) {
    return status;
  }
  for (AlternateCandidate candidate : set.candidates) {
    candidate.evidence.observation_seq = 1;
    status = client.put_evidence(candidate.evidence);
    if (!status.ok()) {
      return status;
    }
  }
  return authority;
}

#ifdef _WIN32
// A raw framed connection, used to prove that session authority cannot be
// borrowed, replayed or carried across an incarnation boundary.
struct RawConnection {
  SOCKET socket = INVALID_SOCKET;
  FrameDecoder decoder;

  ~RawConnection() { close(); }

  bool connect(std::uint16_t port) {
    WSADATA data = {};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      return false;
    }
    socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
      return false;
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      close();
      return false;
    }
    return true;
  }

  void close() {
    if (socket != INVALID_SOCKET) {
      ::closesocket(socket);
      socket = INVALID_SOCKET;
    }
  }

  bool send_raw(std::span<const std::byte> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const int written =
          ::send(socket, reinterpret_cast<const char*>(data.data() + sent),
                 static_cast<int>(data.size() - sent), 0);
      if (written <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(written);
    }
    return true;
  }

  bool send(MsgType type, std::uint64_t sequence, std::span<const std::byte> payload) {
    const std::vector<std::byte> frame = encode_frame(type, sequence, 0, payload);
    return !frame.empty() && send_raw(std::span<const std::byte>(frame.data(), frame.size()));
  }

  Result<Frame> receive() {
    std::byte buffer[4096];
    while (true) {
      auto popped = decoder.pop();
      if (popped.ok()) {
        return popped;
      }
      if (decoder.failed()) {
        return decoder.status();
      }
      const int got = ::recv(socket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
      if (got <= 0) {
        return Status(Code::io_failure, Reason::io_failure, "connection closed");
      }
      const Status fed = decoder.feed(std::span<const std::byte>(buffer, static_cast<std::size_t>(got)));
      if (!fed.ok()) {
        return fed;
      }
    }
  }

  Result<HelloResponse> handshake() {
    HelloRequest hello;
    hello.protocol = protocol_version;
    hello.client = "raw-test";
    const std::vector<std::byte> payload = encode_hello_request(hello);
    if (!send(MsgType::hello, 1, std::span<const std::byte>(payload.data(), payload.size()))) {
      return Status(Code::io_failure, Reason::io_failure, "cannot send hello");
    }
    auto frame = receive();
    if (!frame.ok()) {
      return frame.status();
    }
    if (frame.value().type != MsgType::hello_ack) {
      return Status(Code::refused, Reason::message_unsupported, "handshake refused");
    }
    return decode_hello_response(
        std::span<const std::byte>(frame.value().payload.data(), frame.value().payload.size()));
  }
};
#endif

}  // namespace

PFF_TEST(multiprocess, daemon_serves_a_full_promotion_over_loopback) {
  TempDir dir("daemon");
  Daemon daemon;
  PFF_REQUIRE(daemon.start(dir.path().string()));
  PFF_CHECK_MSG(contains(daemon.banner, "IDENTITY epoch=1"), daemon.banner);

  ClientOptions client_options;
  client_options.port = daemon.port;
  client_options.name = "multiprocess-test";
  auto client = Client::connect(client_options);
  PFF_REQUIRE(client.ok());
  PFF_CHECK(client.value()->hello().epoch == Epoch::from_value(1));

  auto status = client.value()->status();
  PFF_REQUIRE(status.ok());
  PFF_CHECK(!status.value().policy_installed);
  PFF_CHECK(status.value().active_sessions >= 1);

  const AlternateSetId set_id = AlternateSetId::from_value(31);
  auto authority_result = install_baseline_over_client(*client.value(), set_id);
  PFF_REQUIRE(authority_result.ok());
  const AuthorityVector authority = authority_result.value();

  PromotionRequest request;
  request.incumbent = PathId::from_value(1);
  request.set = set_id;
  request.condition = IncumbentCondition::failed;
  request.policy_generation = authority.policy;
  request.obligations_generation = authority.obligations;
  request.topology_generation = authority.topology;
  request.path_authority_generation = authority.path_authority;
  request.epoch = authority.epoch;
  request.boot = authority.boot;

  auto decision = client.value()->evaluate(request);
  PFF_REQUIRE(decision.ok());
  PFF_CHECK(decision.value().outcome == PromotionOutcome::authorized);
  PFF_CHECK(decision.value().authority_kind == AuthorityKind::recommendation);

  auto promotion = client.value()->promote(request);
  PFF_REQUIRE(promotion.ok());
  PFF_CHECK(promotion.value().decision.authority_kind == AuthorityKind::grant);
  PFF_CHECK(promotion.value().decision.selected.valid());

  EffectEvidence effect;
  effect.id = EvidenceId::from_value(4242);
  effect.path = promotion.value().decision.selected;
  // Observations are monotonic per path; the verified effect is newer than the
  // observation that made the path eligible.
  effect.observation_seq = 2;
  effect.effect_verified = true;
  effect.observed_under = authority;
  PFF_REQUIRE(client.value()->record_effect(promotion.value().attempt, effect).ok());

  auto lineage = client.value()->lineage(8);
  PFF_REQUIRE(lineage.ok());
  PFF_REQUIRE(!lineage.value().entries.empty());
  PFF_CHECK(lineage.value().entries.back().outcome == PromotionOutcome::promoted);

  auto attempt = client.value()->attempt(promotion.value().attempt);
  PFF_REQUIRE(attempt.ok());
  PFF_CHECK(attempt.value().found);
  PFF_CHECK(attempt.value().record.state == AttemptState::committed);

  PFF_REQUIRE(client.value()->shutdown().ok());
  PFF_CHECK_EQ(daemon.process.wait(), 0);
}

PFF_TEST(multiprocess, daemon_owns_the_directory_and_releases_it_on_kill) {
  TempDir dir("daemon-lock");
  Daemon daemon;
  PFF_REQUIRE(daemon.start(dir.path().string()));

  int exit_code = 0;
  const std::string denied =
      run_tool(PFF_HOST_PATH, {"--dir", dir.path().string(), "try-open"}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(denied, "OPEN denied refused directory_locked"), denied);

  daemon.process.terminate();
  const std::string allowed =
      run_tool(PFF_HOST_PATH, {"--dir", dir.path().string(), "try-open"}, exit_code);
  PFF_CHECK_MSG(contains(allowed, "OPEN ok"), allowed);
}

PFF_TEST(multiprocess, second_daemon_on_the_same_directory_is_refused) {
  TempDir dir("daemon-second");
  Daemon first;
  PFF_REQUIRE(first.start(dir.path().string()));

  int exit_code = 0;
  const std::string output =
      run_tool(PFF_DAEMON_PATH, {"--dir", dir.path().string(), "--port", "0"}, exit_code);
  PFF_CHECK(exit_code != 0);
  PFF_CHECK_MSG(contains(output, "cannot open state directory"), output);
  first.process.terminate();
}

PFF_TEST(multiprocess, hard_kill_between_authorization_and_completion_is_fenced) {
  TempDir dir("kill-grant");
  int exit_code = 0;
  const std::string granted =
      host_scenario(dir.path().string(),
                    {"promote", "--set", "1", "--paths", "4", "--incumbent", "1", "--crash",
                     "after-grant"},
                    exit_code);
  PFF_CHECK_EQ(exit_code, kCrashExitCode);
  PFF_CHECK_MSG(contains(granted, "GRANTED attempt=1"), granted);
  PFF_CHECK(!contains(granted, "PROMOTED"));

  const std::string verified =
      host_scenario(dir.path().string(),
                    {"verify", "--probe-set", "1", "--probe-incumbent", "1"}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(verified, "EPOCH 2"), verified);
  PFF_CHECK_MSG(contains(verified, "ATTEMPT 1 INTERRUPTED restart_fenced"), verified);
  PFF_CHECK_MSG(contains(verified, "ACTIVE_ATTEMPTS 0"), verified);
  PFF_CHECK_MSG(contains(verified, "PROBE refused REFUSED_STALE_AUTHORITY"), verified);
  PFF_CHECK_MSG(contains(verified, "COMMITTED 0"), verified);

  const std::string late =
      host_scenario(dir.path().string(), {"late-effect", "--attempt", "1", "--path", "1000"},
                    exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(late, "LATE-EFFECT superseded"), late);
}

PFF_TEST(multiprocess, hard_kill_after_durable_commit_before_acknowledgement) {
  TempDir dir("kill-commit");
  int exit_code = 0;
  const std::string output =
      host_scenario(dir.path().string(),
                    {"promote", "--set", "1", "--paths", "4", "--incumbent", "1", "--crash",
                     "before-ack"},
                    exit_code);
  PFF_CHECK_EQ(exit_code, kCrashExitCode);
  PFF_CHECK_MSG(contains(output, "GRANTED attempt=1"), output);
  PFF_CHECK(!contains(output, "PROMOTED"));

  const std::string verified = host_scenario(dir.path().string(), {"verify"}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(verified, "ATTEMPT 1 COMMITTED"), verified);
  PFF_CHECK_MSG(contains(verified, "LINEAGE PROMOTED attempt=1"), verified);
  // Runtime counters are per incarnation and are deliberately not restored; the
  // durable evidence of the committed promotion is the attempt state and the
  // lineage entry above.
  PFF_CHECK_MSG(contains(verified, "EPOCH 2"), verified);
}

PFF_TEST(multiprocess, repeated_crashes_never_reuse_an_attempt_identity) {
  TempDir dir("kill-repeat");
  int exit_code = 0;
  for (int round = 0; round < 3; ++round) {
    const std::string output =
        host_scenario(dir.path().string(),
                      {"promote", "--set", "1", "--paths", "4", "--incumbent", "1", "--crash",
                       "after-grant"},
                      exit_code);
    PFF_CHECK_EQ(exit_code, kCrashExitCode);
    PFF_CHECK_MSG(contains(output, "GRANTED attempt=" + std::to_string(round + 1)), output);
  }
  const std::string verified = host_scenario(dir.path().string(), {"verify"}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(verified, "EPOCH 4"), verified);
  PFF_CHECK_MSG(contains(verified, "ACTIVE_ATTEMPTS 0"), verified);
  PFF_CHECK_MSG(contains(verified, "COMMITTED 0"), verified);
  PFF_CHECK_MSG(contains(verified, "ATTEMPT 3 INTERRUPTED restart_fenced"), verified);
}

PFF_TEST(multiprocess, worker_killed_while_holding_the_directory) {
  TempDir dir("worker-hold");
  ChildProcess holder =
      ChildProcess::spawn(PFF_HOST_PATH, {"--dir", dir.path().string(), "hold"});
  PFF_REQUIRE(holder.valid());
  const std::string held = holder.read_line();
  PFF_CHECK_MSG(contains(held, "HELD epoch=1"), held);

  int exit_code = 0;
  const std::string denied =
      run_tool(PFF_HOST_PATH, {"--dir", dir.path().string(), "try-open"}, exit_code);
  PFF_CHECK_MSG(contains(denied, "OPEN denied refused directory_locked"), denied);

  holder.terminate();
  const std::string allowed =
      run_tool(PFF_HOST_PATH, {"--dir", dir.path().string(), "try-open"}, exit_code);
  PFF_CHECK_MSG(contains(allowed, "OPEN ok"), allowed);
}

PFF_TEST(multiprocess, stopped_daemon_restart_advances_the_incarnation) {
  TempDir dir("daemon-restart");
  Daemon first;
  PFF_REQUIRE(first.start(dir.path().string()));
  PFF_CHECK_MSG(contains(first.banner, "IDENTITY epoch=1"), first.banner);

  ClientOptions options;
  options.port = first.port;
  auto client = Client::connect(options);
  PFF_REQUIRE(client.ok());
  PFF_REQUIRE(client.value()->shutdown().ok());
  PFF_CHECK_EQ(first.process.wait(), 0);

  Daemon second;
  PFF_REQUIRE(second.start(dir.path().string()));
  PFF_CHECK_MSG(contains(second.banner, "IDENTITY epoch=2"), second.banner);

  ClientOptions second_options;
  second_options.port = second.port;
  auto second_client = Client::connect(second_options);
  PFF_REQUIRE(second_client.ok());
  auto status = second_client.value()->status();
  PFF_REQUIRE(status.ok());
  PFF_CHECK_EQ(status.value().stats.restarts, std::uint64_t{2});
  second.process.terminate();

  int exit_code = 0;
  const std::string inspected =
      run_tool(PFF_CTL_PATH, {"inspect", "--dir", dir.path().string()}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(inspected, "epoch=2"), inspected);
  PFF_CHECK_MSG(contains(inspected, "restarts=2"), inspected);
}

PFF_TEST(multiprocess, command_line_client_uses_the_installed_runtime) {
  TempDir dir("ctl");
  Daemon daemon;
  PFF_REQUIRE(daemon.start(dir.path().string()));
  const std::string port = std::to_string(daemon.port);

  int exit_code = 0;
  const std::string status =
      run_tool(PFF_CTL_PATH, {"--port", port, "status"}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(status, "session="), status);
  PFF_CHECK_MSG(contains(status, "policy_installed=0"), status);

  const std::string lineage = run_tool(PFF_CTL_PATH, {"--port", port, "lineage"}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(lineage, "entries=0"), lineage);

  const std::string stopping = run_tool(PFF_CTL_PATH, {"--port", port, "shutdown"}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(stopping, "stopping"), stopping);
  PFF_CHECK_EQ(daemon.process.wait(), 0);

  const std::string inspected =
      run_tool(PFF_CTL_PATH, {"inspect", "--dir", dir.path().string()}, exit_code);
  PFF_CHECK_EQ(exit_code, 0);
  PFF_CHECK_MSG(contains(inspected, "restarts=1"), inspected);
}

#ifdef _WIN32
PFF_TEST(multiprocess, session_authority_cannot_be_borrowed_or_replayed) {
  TempDir dir("session-binding");
  Daemon daemon;
  PFF_REQUIRE(daemon.start(dir.path().string()));

  ClientOptions options;
  options.port = daemon.port;
  options.name = "session-a";
  auto client = Client::connect(options);
  PFF_REQUIRE(client.ok());
  const SessionHeader authority = client.value()->header();
  PFF_CHECK(authority.session.valid());

  {
    // Another session presents the first session's identity.
    RawConnection raw;
    PFF_REQUIRE(raw.connect(daemon.port));
    auto hello = raw.handshake();
    PFF_REQUIRE(hello.ok());
    PFF_CHECK(hello.value().session != authority.session);
    SessionHeader stolen = authority;
    stolen.request_sequence = 1;
    const std::vector<std::byte> payload =
        encode_session_request(stolen, {});
    PFF_REQUIRE(raw.send(MsgType::status_req, 2,
                         std::span<const std::byte>(payload.data(), payload.size())));
    auto response = raw.receive();
    PFF_REQUIRE(response.ok());
    PFF_CHECK(response.value().type == MsgType::error_rsp);
    auto error = decode_error_response(
        std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
    PFF_REQUIRE(error.ok());
    PFF_CHECK_EQ(error.value().reason, Reason::session_unknown);
  }

  {
    // A replayed request sequence is refused.
    RawConnection raw;
    PFF_REQUIRE(raw.connect(daemon.port));
    auto hello = raw.handshake();
    PFF_REQUIRE(hello.ok());
    SessionHeader header;
    header.session = hello.value().session;
    header.epoch = hello.value().epoch;
    header.boot = hello.value().boot;
    header.request_sequence = 5;
    const std::vector<std::byte> payload = encode_session_request(header, {});
    PFF_REQUIRE(raw.send(MsgType::status_req, 2,
                         std::span<const std::byte>(payload.data(), payload.size())));
    auto first = raw.receive();
    PFF_REQUIRE(first.ok());
    PFF_CHECK(first.value().type == MsgType::status_rsp);

    RawConnection replay;
    PFF_REQUIRE(replay.connect(daemon.port));
    auto replay_hello = replay.handshake();
    PFF_REQUIRE(replay_hello.ok());
    SessionHeader replay_header = header;
    replay_header.session = replay_hello.value().session;
    replay_header.epoch = replay_hello.value().epoch;
    replay_header.boot = replay_hello.value().boot;
    const std::vector<std::byte> replay_payload = encode_session_request(replay_header, {});
    PFF_REQUIRE(replay.send(MsgType::status_req, 2,
                            std::span<const std::byte>(replay_payload.data(), replay_payload.size())));
    auto accepted = replay.receive();
    PFF_REQUIRE(accepted.ok());
    PFF_CHECK(accepted.value().type == MsgType::status_rsp);
    // The same sequence again is a regression.
    PFF_REQUIRE(replay.send(MsgType::status_req, 3,
                            std::span<const std::byte>(replay_payload.data(), replay_payload.size())));
    auto rejected = replay.receive();
    PFF_REQUIRE(rejected.ok());
    PFF_CHECK(rejected.value().type == MsgType::error_rsp);
    auto error = decode_error_response(
        std::span<const std::byte>(rejected.value().payload.data(), rejected.value().payload.size()));
    PFF_REQUIRE(error.ok());
    PFF_CHECK_EQ(error.value().reason, Reason::sequence_regression);
  }

  {
    // A stale boot identity from another incarnation is refused.
    RawConnection raw;
    PFF_REQUIRE(raw.connect(daemon.port));
    auto hello = raw.handshake();
    PFF_REQUIRE(hello.ok());
    SessionHeader header;
    header.session = hello.value().session;
    header.epoch = hello.value().epoch;
    header.boot = BootId::from_value(hello.value().boot.value() + 1);
    header.request_sequence = 1;
    const std::vector<std::byte> payload = encode_session_request(header, {});
    PFF_REQUIRE(raw.send(MsgType::status_req, 2,
                         std::span<const std::byte>(payload.data(), payload.size())));
    auto response = raw.receive();
    PFF_REQUIRE(response.ok());
    PFF_CHECK(response.value().type == MsgType::error_rsp);
    auto error = decode_error_response(
        std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
    PFF_REQUIRE(error.ok());
    PFF_CHECK_EQ(error.value().reason, Reason::session_stale);
  }

  {
    // A non-handshake first frame is refused.
    RawConnection raw;
    PFF_REQUIRE(raw.connect(daemon.port));
    SessionHeader header;
    header.session = SessionId::from_value(1);
    header.epoch = Epoch::from_value(1);
    header.boot = BootId::from_value(1);
    header.request_sequence = 1;
    const std::vector<std::byte> payload = encode_session_request(header, {});
    PFF_REQUIRE(raw.send(MsgType::status_req, 1,
                         std::span<const std::byte>(payload.data(), payload.size())));
    auto response = raw.receive();
    PFF_REQUIRE(response.ok());
    PFF_CHECK(response.value().type == MsgType::error_rsp);
    auto error = decode_error_response(
        std::span<const std::byte>(response.value().payload.data(), response.value().payload.size()));
    PFF_REQUIRE(error.ok());
    PFF_CHECK_EQ(error.value().reason, Reason::message_unsupported);
  }

  daemon.process.terminate();
}
#endif

PFF_TEST_MAIN()
