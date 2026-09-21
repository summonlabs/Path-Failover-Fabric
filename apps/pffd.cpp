// pffd - Path Failover Fabric coordinator daemon.
//
// Opens the durable state directory, advances the incarnation boundary and
// serves the framed protocol on a loopback socket. The bound port is printed on
// stdout as a single line so that supervisors and tests can discover it without
// guessing or polling.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "args.hpp"
#include "pff/crc32c.hpp"
#include "pff/fabric.hpp"
#include "pff/server.hpp"

int run_main(int argc, char** argv) {
  using namespace pff;
  using namespace pff::tools;

  const auto directory = arg_value(argc, argv, "--dir");
  if (!directory.has_value()) {
    std::fprintf(stderr,
                 "usage: pffd --dir <state-dir> [--port N] [--bind 127.0.0.1] "
                 "[--no-shutdown] [--provenance real|synthetic]\n");
    return 2;
  }

  FabricOptions fabric_options;
  fabric_options.directory = *directory;
  const std::string provenance = arg_or(argc, argv, "--provenance", "synthetic");
  fabric_options.provenance = provenance == "real" ? Provenance::real : Provenance::synthetic;

  auto fabric = Fabric::open(fabric_options);
  if (!fabric.ok()) {
    std::fprintf(stderr, "pffd: cannot open state directory: %s\n",
                 fabric.status().describe().c_str());
    return 3;
  }

  ServerOptions server_options;
  server_options.bind_address = arg_or(argc, argv, "--bind", "127.0.0.1");
  server_options.port = static_cast<std::uint16_t>(arg_u64(argc, argv, "--port", 0));
  server_options.allow_shutdown = !flag_present(argc, argv, "--no-shutdown");

  auto server = Server::start(server_options, *fabric.value());
  if (!server.ok()) {
    std::fprintf(stderr, "pffd: cannot start service: %s\n", server.status().describe().c_str());
    return 4;
  }

  const FabricIdentity identity = fabric.value()->identity();
  std::printf("LISTENING %u\n", static_cast<unsigned>(server.value()->port()));
  std::printf("IDENTITY epoch=%llu boot=%s incarnation=%s restart=%llu\n",
              static_cast<unsigned long long>(identity.epoch.value()),
              id_hex(identity.boot).c_str(), id_hex(identity.incarnation).c_str(),
              static_cast<unsigned long long>(identity.restart_count));
  std::fflush(stdout);

  const Status status = server.value()->run();
  if (!status.ok()) {
    std::fprintf(stderr, "pffd: service stopped with error: %s\n", status.describe().c_str());
    return 5;
  }
  std::printf("STOPPED\n");
  std::fflush(stdout);
  return 0;
}
int main(int argc, char** argv) {
  try {
    return run_main(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "pffd: fatal: %s\n", error.what());
    return 70;
  } catch (...) {
    std::fprintf(stderr, "pffd: fatal: unknown error\n");
    return 70;
  }
}

