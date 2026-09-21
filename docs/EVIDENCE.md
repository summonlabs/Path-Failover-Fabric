# Evidence matrix and release validation

Everything in this file was produced by commands that were actually run on the
release host. Nothing here is projected, inferred or aspirational.

## 1. Host and toolchain

| Item | Value |
| --- | --- |
| Operating system | Windows (x86-64) |
| Compiler | Microsoft C/C++ 19.44.35222 (Visual Studio 2022 Build Tools) |
| Build system | CMake 4.3.2 with Ninja 1.13.2 |
| Sanitizer runtime | MSVC `clang_rt.asan_dynamic-x86_64.dll` present |
| Static analysis | LLVM 19.1.5 `clang-tidy` |
| C++ standard | C++20 |

## 2. REAL / SYNTHETIC / UNSUPPORTED

| Claim | Label | How it was produced |
| --- | --- | --- |
| Selector, coordinator, persistence, codec, tools | **REAL** | compiled and exercised by the suites below |
| Multiprocess behaviour | **REAL** | `pff_test_multiprocess` spawns real `pffd@@, `pff_host@@ and @@BFF@@pffctl` child processes and talks to them over real loopback sockets |
| Hard process kill | **REAL** | worker exits with code 9 through @@B__Exit` at a named durable boundary; the parent also uses `TerminateProcess` for the directory-lock proof |
| Restart fencing | **REAL** | new process, new boot identity, advanced epoch, pre-restart attempt `INTERRUPTED` |
| Durable commit surviving a kill | **REAL** | kill after the commit record is flushed and before the acknowledgement |
| Alternate sets, policy, obligations, observations, effects | **SYNTHETIC** | deterministic fixtures built in `tests/support.hpp` and labelled @@B__@@Provenance::synthetic` |
| Physical switch, NIC, RDMA, RoCE, DPU, NVLink, multi-node | **UNSUPPORTED** | not exercised; not claimed anywhere |
| POSIX socket and file paths | **UNSUPPORTED on this host** | implemented, but only the Windows path has been run |
| UndefinedBehaviorSanitizer | **UNSUPPORTED on this host** | MSVC has no UBSan; no clang or GCC toolchain is installed (only `clang-tidy.exe` ships with the Build Tools) |

## 3. Build and validation matrix

| Configuration | Command | Result |
| --- | --- | --- |
| Release, `/W4 /WX /permissive-` | `cmake -DCMAKE_BUILD_TYPE=Release` then `cmake --build` | clean, zero warnings |
| Debug, `/W4 /WX /permissive-` | `cmake -DCMAKE_BUILD_TYPE=Debug` | clean, zero warnings; debug assertions active |
| AddressSanitizer | `-DPFF_ENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo` | built; 9/9 suites pass with the runtime on @@B_PATH@@ |
| UBSan | n/a | UNSUPPORTED on this host |
| Static analysis | `clang-tidy -p build/release --checks="-*,bugprone-*,performance-*,clang-analyzer-*,portability-*"` | 2 remaining findings, both accepted and justified in section 7 |

Test results:

| Configuration | Suites | Result |
| --- | --- | --- |
| Release | 9 | 100% passed |
| Debug | 9 | 100% passed |
| AddressSanitizer (RelWithDebInfo) | 9 | 100% passed |

## 4. Suite inventory

| Suite | Tests | Focus |
| --- | --- | --- |
| `pff_test_core` | 16 | identities, status vocabulary, CRC-32C reference vectors, checked arithmetic, canonical codec bounds, authority classification, model validation, objective exactness, wire round trips, enum-driven bounds |
| `pff_test_selector` | 16 | 720 permutations byte-identical, 40x25 sampled permutations, ordering versus authority, every gate, unknown-member blocking and explicit skip, fatal conflict, proof-carrying refusal, bounded search, stale-surface reporting, bounded document, 1200-case differential against the reference solver |
| `pff_test_persistence` | 19 | journal and snapshot round trips, compaction cycles, torn header and torn payload recovery, complete-but-corrupt header refusal, payload bit flip, sequence regression and gaps, unsupported version, unknown record type, impossible length, snapshot field corruption (magic, version, flags, length, header CRC, total CRC, footer magic, truncation), record count mismatch, every truncated payload prefix, oversized append, inspection mode, directory lock, missing directory |
| `pff_test_protocol` | 12 | frame round trip, every truncated prefix, header field corruption (magic, version, type, flags, header CRC, payload CRC) with sticky failure, oversized declared length before allocation, buffer bound, pipelined frames versus trailing garbage, zero-length payload, message classification, payload round trips, truncation and trailing bytes, bounds, detail truncation |
| `pff_test_fabric` | 13 | promotion lifecycle, recommendation versus grant, wrong path and unverified and stale effects, double completion, single ownership, superseded late completion, path fencing revocation, restart epoch and fencing, durability of definitions and lineage, generation reuse conflict, rollback fresh validation, reversion fresh validation, resource bounds, compaction, exclusive directory, provenance labelling |
| `pff_test_property` | 3 | 60 seeds x 24 operations with invariants after every operation and a live reference-solver comparison; 40 seeds proving every promoted path is in the supplied eligible unfenced set; attempt monotonicity and terminal-state stability |
| `pff_test_concurrency` | 4 | eight threads on a barrier produce exactly one winner; six threads produce exactly one completion; fence during completion; eight workers x 40 mixed operations |
| `pff_test_multiprocess` | 10 | full promotion over loopback with the real daemon, directory lock across processes, second daemon refused, hard kill between authorization and completion, hard kill after durable commit before acknowledgement, three consecutive crashes with distinct attempt identities, worker killed while holding the directory, daemon restart epoch advance, command line client, session authority borrowing and replay and stale boot rejection |
| `pff_test_scale` | 3 | work counters at four scales, promotion throughput, retained state bounds, document bound at the candidate limit |

## 5. Deterministic scale evidence

Selection work counters, measured on this host (Release):

| Candidates | Ordering comparisons | Gate evaluations | Score evaluations |
| --- | --- | --- | --- |
| 64 | 304 | 896 | 64 |
| 256 | 1713 | 3584 | 256 |
| 1024 | 8937 | 14336 | 1024 |
| 4096 | 43961 | 57344 | 4096 |

A four-fold increase in candidates multiplies the comparison count by about 4.9,
which is `n log n` behaviour, not quadratic. The suite asserts the
comparison bound `n * (ceil(log2 n) + 1)`, the gate bound `16 * n` and
the ratio bound `work_ratio < 2 * size_ratio`, all from deterministic
counters rather than wall-clock time.

Fabric throughput: 200 promotions (authorise + verify + re-supply) in about
1.1 s on this host, with retained attempts bounded at 64, lineage bounded at
4096 and the fence table bounded at 4096.

## 6. Install, downstream and closure evidence

| Step | Result |
| --- | --- |
| `cmake --install` | installs headers, static library, `pffTargets.cmake`, `pffConfig.cmake`, `pffConfigVersion.cmake`, the three tools, LICENSE and README |
| Independent consumer | `examples/consumer` configures with `find_package(pff 1.0 CONFIG REQUIRED)`, links `pff::pff@@ and runs: promoted a path end to end |
| Example | `pff_example_quickstart` demonstrates recommendation, grant, verified effect, lineage, unknown-member blocking, explicit skip policy and fencing |
| Fresh clone | @@B__@@scripts/fresh-clone-closure.ps1` clone, configure, build, ctest, install, consumer build and run, example run |

## 7. Accepted static analysis findings

Two `bugprone-easily-swappable-parameters` diagnostics remain, both on
internal helpers whose neighbouring parameters are always literal constants at
every call site:

1. @@B__@@ByteWriter(std::size_t reserve_hint, std::size_t max_size)` — swapped
   arguments would make the writer refuse its first byte, which
   @@B__@@pff_test_core.byte_writer_bounds_are_enforced` already asserts.
2. @@B__@@encode_frame(MsgType, std::uint64_t sequence, std::uint32_t flags, ...)@@B__
   — a swap would put a non-zero value into the flags field, which
   @@B__@@pff_test_protocol.header_field_corruption_is_sticky` already asserts is
   refused.

Both are recorded rather than silenced, and both are covered by tests.

## 8. Material defects found and fixed during hardening

| # | Defect | Where found | Fix |
| --- | --- | --- | --- |
| 1 | `Result<T>` could be constructed from a success status, producing a value-less result that reported @@B__@@ok:none@@B__@@ | @@B__@@Fabric::open@@B__@@ failed on an empty directory during the first example run | @@B__@@Store::load_snapshot@@B__@@ now returns @@B__@@Status@@B__@@; the constructor asserts in debug builds |
| 2 | The canonical decision document contained order-dependent work counters, so permutations produced different bytes | permutation test in @@B__@@pff_test_selector@@B__@@ | counters removed from the canonical encoding and documented as diagnostics |
| 3 | @@B__@@FrameDecoder::has_frame@@B__@@ reported true for a partially received frame | truncated-prefix test | it now validates the header and requires the whole frame |
| 4 | The reason-range bound was stale, rejecting valid durable records on replay | persistence round trip | bounds derived from the enumerators in one place |
| 5 | @@B__@@begin_promotion@@B__@@ allocated an attempt and wrote lineage for structural refusals that were never decisions about a path | fabric lifecycle test | structural refusals allocate nothing and write no lineage |
| 6 | Contention refused with a bare error instead of a decision document | single-ownership test | @@B__@@REFUSED_ALREADY_OWNED@@B__@@ is returned as a full decision |
| 7 | A stale member anywhere made the surface proof vacuous | adversarial differential cases | any stale exclusion now yields @@B__@@REFUSED_STALE_AUTHORITY@@B__@@ instead of a proof of absence |
| 8 | The expected-set-generation mismatch classified staleness and conflict backwards | malformed-input test | the caller being ahead of the runtime is now a conflict |
| 9 | A successful protocol mutation was answered as an error | multiprocess daemon test | @@B__@@from_status@@B__@@ maps an ok status to an empty acknowledgement |
| 10 | @@B__@@Server::listener_@@B__@@ was read and written without synchronisation | concurrency and ownership audit | it is now atomic, exchanged and closed by the accept loop alone |
| 11 | A blocking send held the session mutex, so a stuck peer could block teardown | concurrency and ownership audit | sends hold no lock; sockets are shared by ownership and released with @@B__@@shutdown()@@B__@@ |
| 12 | The session handle was read and written under different mutexes, allowing shutdown to act on a closed or recycled handle | concurrency and ownership audit | @@B__@@SocketHandle@@B__@@ closes exactly once, under shared ownership |
| 13 | The server destructor could allocate and throw | static analysis | allocation removed; the destructor is guarded and its last resort is @@B__@@noexcept@@B__@@ |
| 14 | @@B__@@PFF_CHECK_EQ@@B__@@ bound references into temporaries, producing a stack-use-after-scope | AddressSanitizer capability check | the macro copies its operands |
| 15 | @@B__@@Store::load_snapshot@@B__@@ was called twice on the failure path | code review while fixing 1 | single call |

## 9. Genuine remaining limitations

* No authentication or encryption on the framed protocol. See the trust boundary
  section of the README.
* The runtime cannot verify the physical fabric; it validates an externally
  reported effect.
* @@B__@@FabricStats@@B__@@ counters are per incarnation and are not restored on
  restart by design.
* Fence and lineage tables are bounded and refuse rather than evict.
* One state directory is owned by one process; the runtime is not replicated.
* POSIX code paths are implemented but unexercised on this host.
* No UBSan coverage on this host.
