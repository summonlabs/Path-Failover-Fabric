# Path Failover Fabric

**Version 1.0.0** — a deterministic alternate path promotion runtime for
infrastructure fabrics.

Path Failover Fabric answers exactly one question, and answers it truthfully
under stale, partial, contradictory, restarted, concurrent and adversarial
conditions:

> Given a failed or invalidated incumbent path, an authoritative ordered
> alternate set, service obligations, policy and exact generations, **which
> alternate path may be promoted now, why is it the deterministic winner, and
> when must promotion be fenced, rolled back, revalidated or refused?**

Everything else in this repository exists to make that answer reproducible,
auditable and safe.

---

## 1. Systems boundary

### What this runtime owns

* Promotion of an **already-supplied, already-legality-governed** alternate path
  set after the incumbent becomes unusable.
* The transition lifecycle of that promotion: claim, authorisation, verified
  effect, commit, fencing, rollback, reversion, abandonment.
* The deterministic, canonical, permutation-independent ranking of the supplied
  candidates and the gating that decides which of them may actually be promoted.
* Binding every decision to the exact generations that made it legal, and
  revoking that authority when any of those generations changes.
* Durable lineage of completed outcomes and durable fences.
* A bounded framed service that exposes the same decisions over a socket.

### What this runtime deliberately does not own

* Path discovery. Candidates are supplied; the fabric never invents one.
* Route computation, capacity reservation, forwarding state installation.
* Global congestion decisions, topology computation, path authority, policy
  authorship, service obligation authorship.
* Physical effect verification: the fabric accepts a verified effect report from
  an external observer. It never claims to have observed the fabric itself.

These adjacent responsibilities are integrated through explicit typed inputs,
evidence, references and authority boundaries — never by absorbing them.

---

## 2. Authority model

The full model is in `docs/AUTHORITY_MODEL.md`. The essential rules:

| Statement | Meaning in this runtime |
| --- | --- |
| Matching identity is not matching generation | A matching `PathId` is worthless unless the authority vector matches too |
| Observation is not authority | `PathEvidence` never authorises anything; it is an input to gating |
| Eligibility is not authorisation | `Eligibility::eligible` says "this member may be considered", nothing more |
| Recommendation is not authorisation | `Fabric::evaluate` returns `AuthorityKind::recommendation` and creates no attempt |
| Authorisation is not application | A `Grant` authorises one transition; it is not applied until a verified effect is recorded |
| Acknowledgement is not verified effect | An `ok` protocol response means the request was accepted, not that the fabric changed |
| Persistence is not liveness | Restoring a record never restores freshness, a lease, an in-flight attempt or a backend effect |
| UNKNOWN never becomes affirmative | UNKNOWN, STALE, CONFLICT, INVALID and UNSUPPORTED are first-class and fail closed |

Every decision document carries the **authority vector** it was computed
against:

    AuthorityVector {
      Generation topology;        // fabric topology authority
      Generation path_authority;  // the authority that declared the set legal
      Generation policy;          // failover policy generation
      Generation obligations;     // service obligation generation
      Epoch      epoch;           // coordinator term
      BootId     boot;            // process incarnation
    }

Classification of any supplied vector against the current one yields exactly:
`CURRENT`, `STALE`, `UNKNOWN` or `CONFLICT`, with
the first blocking field named in the reason. Severity order is
`CONFLICT > UNKNOWN > STALE > CURRENT`.

---

## 3. Deterministic selection

The objective is exact integer arithmetic, evaluated with checked operations:

    score = w_health  * clamp(health_ppm, 0, 1_000_000)
          + w_cost    * (1_000_000_000 - clamp(cost_units, 0, 1_000_000_000))
          + w_rank    * (65_535 - clamp(upstream_rank, 0, 65_535))
          + w_capability * popcount(capabilities & required_capabilities)

The canonical total order is:

1. `score` descending
2. `upstream_rank` ascending
3. path identity ascending

Path identity is unique inside a validated set, so this is a **total** order: the
sorted sequence is unique, and the decision document is byte-identical for every
permutation of the same logical input. Candidate ordering is a recommendation
input; it is never authority. The winner is the first member of the canonical
order that passes **every** gate.

Gates, evaluated in this fixed order, each producing a definite exclusion or an
indeterminate block:

| # | Gate | Definite exclusion | Blocking (indeterminate) |
| --- | --- | --- | --- |
| 1 | incumbent itself | `incumbent_is_self` | |
| 2 | withdrawn by upstream | `withdrawn` | |
| 3 | durable fence | `path_fenced` | |
| 4 | generation coverage | `authority_stale` | `authority_unknown`, `generation_conflict` (fatal) |
| 5 | objective overflow | `score_overflow` | |
| 6 | upstream eligibility | `eligibility_ineligible` | `eligibility_unknown` |
| 7 | evidence freshness | `evidence_stale` | `evidence_unknown` |
| 8 | reachability | `reachability_unreachable` | `reachability_unknown` |
| 9 | capacity | `capacity_insufficient` | `capacity_unknown` |
| 10 | required capabilities | `capability_missing` | |
| 11 | health obligation | `health_below_obligation` | |
| 12 | cost obligation | `cost_above_obligation` | |
| 13 | policy score floor | `score_below_policy` | |

**Proof carrying refusal.** `REFUSED_PROVEN_NO_ALTERNATE` is emitted only
when the supplied set is declared `COMPLETE`, the whole surface was
examined (no search limit reached), no member was indeterminate, and no member
was excluded because its generations are stale. Anything else is reported as
`INDETERMINATE_SEARCH_LIMIT`, `INDETERMINATE_INCOMPLETE_SET` or
`INDETERMINATE_UNKNOWN_MEMBER`. A bounded scan that finds nothing is never
reported as proof of absence.

`allow_unknown_skip` (default **false**) is the only way to promote past a
blocking member; when it is enabled the decision records the blocked members in
`indeterminate_above`, so reduced confidence is always visible.

---

## 4. Lifecycle and restart semantics

    claimed -> authorized -> committed
            \-> refused / fenced / superseded / abandoned / rolled_back / interrupted

* **One owner.** At most one attempt owns the transition. Contending callers get
  `REFUSED_ALREADY_OWNED` as a full decision document.
* **Late completion is refused.** A completion from a fenced, superseded or
  interrupted attempt is rejected and counted in
  `stale_completions_rejected`. The only path to `committed` is a
  verified effect bound to the attempt's own path under the **current** authority
  vector.
* **Fencing.** A policy change, an obligation change, a topology or path
  authority advance, a path fence and a restart all fence in-flight attempts.
  Fences are durable and authority-bearing: the fence table refuses new fences
  when full rather than silently evicting one.
* **Rollback** is a pre-commit undo that requires **fresh** validation of the
  incumbent (freshness, eligibility, reachability, capacity, capabilities,
  obligations, generations, not fenced).
* **Reversion** is a post-commit explicit transition back to a displaced path,
  with the same fresh validation.
* **Restart** opens a new incarnation: a new boot identity, an advanced
  coordinator epoch, and every pre-restart active attempt marked
  `INTERRUPTED` with a `restart_fenced` fence record. No
  pre-restart evidence, freshness, lease or in-flight authority is restored.

---

## 5. Persistence

Durable artifacts live in one state directory owned by exactly one runtime
process (exclusive file lock):

* `fabric.snapshot` — transactional full state (write staging, flush,
  atomic replace, directory sync), versioned, header and total CRC-32C checked.
* `fabric.journal` — append-only framed records:
  `magic | version | type | sequence | payload_length | header_crc | payload | payload_crc`.
  The length field is authenticated before it is ever used to size an allocation.
* `fabric.lock` — exclusive ownership lock.

Recovery rules, exactly:

| Situation | Behaviour |
| --- | --- |
| Tail shorter than a full record header | genuine torn tail: truncated and reported |
| Valid header, payload incomplete | genuine torn tail: truncated and reported |
| Fully present header failing integrity | **corrupt**: refuse to open, never repair |
| Complete record failing payload integrity | **corrupt**: refuse to open |
| Non-contiguous sequence | `sequence_regression`: refuse to open |
| Older sequence already covered by the snapshot | skipped and counted |
| Unsupported version, unknown record type, impossible length | refuse to open |
| Trailing bytes after a snapshot | `trailing_garbage`: refuse to open |

Only the two torn-tail cases above are ever repaired, and both are reported in
the recovery report.

---

## 6. Service, tools and examples

`pffd` serves a bounded framed protocol on a loopback socket. Frames carry
magic, version, type, flags, a monotonic per-session sequence, an authenticated
payload length and two integrity checks. The decoder is total and **sticky**:
truncated prefixes wait for more bytes, but a corrupt header, an invalid enum, an
oversized declared payload or an integrity failure permanently fails that
session.

Every request after the handshake is bound to the session authority established
at handshake time (session identity, coordinator epoch, boot identity) and to a
strictly increasing request sequence. A request that borrows another session's
identity, replays a sequence, or carries a stale boot is refused and the session
is closed.

| Tool | Purpose |
| --- | --- |
| `pffd` | coordinator daemon: opens the state directory and serves the framed protocol |
| `pffctl` | client (`status`, `lineage`, `evaluate`, `promote`, `fence`, `shutdown`) and offline inspector (`inspect --dir`) |
| `pff_host` | scenario worker used by the multiprocess proofs |
| `pff_example_quickstart` | end-to-end walkthrough of the product-defining question |

Typical daemon session:

    pffd --dir ./state --port 0
    LISTENING 52344
    IDENTITY epoch=1 boot=5bdaa80eb19395a1 incarnation=ac94d7661744d030 restart=1
    pffctl --port 52344 status
    pffctl --port 52344 shutdown
    pffctl inspect --dir ./state

---

## 7. Build, install and use

Requirements: CMake 3.20+, a C++20 compiler (MSVC 19.30+ or GCC/Clang), no third
party runtime dependencies.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=./stage
    cmake --build build
    ctest --test-dir build --output-on-failure
    cmake --install build

Downstream use, from an independent project outside the source tree:

    find_package(pff 1.0 CONFIG REQUIRED)
    target_link_libraries(my_runtime PRIVATE pff::pff)

    #include <pff/fabric.hpp>
    auto fabric = pff::Fabric::open({.directory = "./state"});
    auto authorization = fabric.value()->begin_promotion(
        incumbent, pff::IncumbentCondition::failed, set_id);
    if (authorization.value().authorized()) {
      // apply the transition, then report the verified effect
      fabric.value()->record_effect(authorization.value().grant.attempt, verified_effect);
    }

`examples/consumer/` is exactly such a project;
`scripts/fresh-clone-closure.ps1` builds, tests, installs and consumes a
fresh clone end to end.

---

## 8. Proof inventory

Nine CTest suites, all deterministic, none using a timeout:

| Suite | What it proves |
| --- | --- |
| `pff_test_core` | identities, status vocabulary, canonical codec, checked arithmetic, authority classification, model validation, wire round trips |
| `pff_test_selector` | all 720 permutations of a colliding-score set produce byte-identical decisions; 40x25 sampled permutations; ordering-versus-authority; every gate; proof-carrying refusal; **1200 seeded differential cases against an independently written reference solver** |
| `pff_test_persistence` | journal and snapshot round trips, compaction, torn-tail recovery versus corruption refusal, every snapshot field corruption, sequence regression, impossible lengths, invalid enums, trailing garbage, inspection mode, directory lock |
| `pff_test_protocol` | every truncated frame prefix, magic/version/type/flags/header-CRC/payload-CRC corruption, oversized declared lengths, buffer bounds, sticky failure, message payload round trips and bounds |
| `pff_test_fabric` | full lifecycle, single ownership, late completion rejection, fencing, rollback and reversion with fresh validation, restart fencing, generation reuse conflicts, resource bounds, compaction |
| `pff_test_property` | 60 seeds x 24 operations with invariants asserted **after every operation**, plus a live comparison against the reference solver; 40 seeds proving every promoted path belongs to the supplied eligible, unfenced set |
| `pff_test_concurrency` | barrier-synchronised races: exactly one winner of eight, exactly one completion of six, fence-during-completion, mixed contention invariants |
| `pff_test_multiprocess` | real daemon and worker processes over loopback sockets, hard kills at durable boundaries, restart fencing, directory lock across processes, session authority borrowing and replay rejection |
| `pff_test_scale` | deterministic work counters at 64/256/1024/4096 candidates proving sub-quadratic selection, promotion throughput, bounded retained state |

Evidence classification and the full result matrix are in
`docs/EVIDENCE.md`.

---

## 9. Evidence classification

| Capability | Label |
| --- | --- |
| Library, selector, coordinator, persistence, protocol, tools | **REAL** — built and exercised on this host |
| Multiprocess, crash, restart, fencing, session binding | **REAL** — independent OS processes over real loopback sockets, real hard kills |
| Alternate set, policy, obligations, evidence, effect reports | **SYNTHETIC** — deterministic fixtures; no physical fabric |
| Physical switch, NIC, RDMA, RoCE, DPU, multi-node behaviour | **UNSUPPORTED** — not exercised, not claimed |
| AddressSanitizer | **REAL** on this host (MSVC `/fsanitize=address`), 9/9 suites clean |
| UndefinedBehaviorSanitizer | **UNSUPPORTED** on this host — MSVC has no UBSan and no clang or GCC toolchain is installed |
| Static analysis | **REAL** on this host (LLVM 19 clang-tidy: bugprone, performance, clang-analyzer, portability) |
| POSIX socket and file paths | Implemented, **UNSUPPORTED** on this host: verified only under Windows |

---

## 10. Trust boundary

The framed protocol is **not** authenticated and **not** encrypted. It binds each
request to the session authority the runtime itself established at handshake, but
any local process that can reach the loopback port can establish its own session.
Integrity checks (CRC-32C) detect accidental corruption and truncation; they are
not a cryptographic authenticator and provide no protection against an active
adversary. Deploy behind an authenticated transport or a process-level trust
boundary. No cryptographic security is claimed.

---

## 11. Genuine limitations

* Authentication and transport confidentiality are out of scope and unimplemented.
* The runtime verifies that an effect report is internally consistent, current
  and about the authorised path. It cannot verify the physical fabric itself.
* `FabricStats` counters are per incarnation and are deliberately not
  restored on restart; durable evidence of a completed promotion is the attempt
  record and the lineage entry, not a counter.
* The policy objective is a weighted sum of four bounded terms. A different
  objective shape requires a new policy generation and a code change, not a
  configuration change.
* The fence and lineage tables are bounded. When the fence table is full the
  runtime refuses new fences rather than evicting authority, which is safe but
  requires operator intervention.
* The state directory is owned by exactly one process at a time; the runtime is
  not a replicated or highly available service.
* POSIX socket and file code paths are implemented but have not been exercised on
  this host.

---

## 12. Repository layout

    include/pff/      public headers (installed)
    src/              runtime implementation
    apps/             pffd, pffctl, pff_host
    examples/         quickstart and the independent downstream consumer
    tests/            the nine proof suites and shared fixtures
    docs/             architecture, authority model, concurrency audit, evidence
    scripts/          fresh-clone closure

## License
Apache License 2.0. Copyright 2026 Summon Software Labs.
