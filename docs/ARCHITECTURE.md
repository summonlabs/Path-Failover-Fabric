# Architecture

## 1. Boundary

`Path Failover Fabric` owns one decision and one transition:

    promoted(incumbent, alternate_set, obligations, policy, generations) -> winner | refusal

It consumes an alternate set that an upstream path authority has already declared
legal and already ordered. It never discovers paths, computes routes, reserves
capacity, installs forwarding state or decides global congestion. Every adjacent
responsibility enters through a typed value with an explicit generation.

    upstream path authority ─┐
    topology authority ──────┤   typed inputs + authority vectors
    policy author ───────────┤
    obligation author ───────┘
                             │
                             v
                    +---------------------+
                    |  Selector (pure)    |  canonical order + gates
                    +---------------------+
                             │  PromotionDecision (recommendation)
                             v
                    +---------------------+
                    |  Fabric (stateful)  |  attempts, grants, fences, lineage
                    +---------------------+
                             │  Grant (positive authority)
                             v
                    effect observer ──> EffectEvidence ──> committed transition
                             │
                             v
                    +---------------------+
                    |  Store (durable)    |  journal + snapshot + lock
                    +---------------------+
                             │
                             v
                    +---------------------+
                    |  Server (framed)    |  bounded loopback service
                    +---------------------+

## 2. Components

| Component | Header | Responsibility |
| --- | --- | --- |
| Identities | `pff/ids.hpp` | strongly typed, non-interchangeable identities; zero means absent |
| Authority | `pff/authority.hpp` | authority vectors and CURRENT/STALE/UNKNOWN/CONFLICT classification |
| Model | `pff/model.hpp` | evidence, obligations, policy, candidates, alternate sets, objective |
| Selector | `pff/selector.hpp` | pure deterministic selection and gating |
| Decision | `pff/decision.hpp` | outcomes, assessment, canonical decision document, digest |
| Records | `pff/records.hpp` | attempt, fence, lineage, durable state, identity, stats |
| Store | @@BFF@@pff/store.hpp` | versioned integrity-checked journal and snapshot |
| Fabric | `pff/fabric.hpp` | transition ownership, grants, fencing, restart, queries |
| Protocol | `pff/protocol.hpp` | bounded framed codec and message payloads |
| Server | `pff/server.hpp` | accepting service and client |

`Selector` is a pure function: no clock, no randomness, no state, no
container-order dependence. `Fabric` is the only stateful component and
holds a single non-recursive mutex.

## 3. Decision pipeline

1. **Structural validation.** Policy, obligations, set and incumbent are
   validated. Any failure is an explicit refusal, not an exception.
2. **Basis binding.** The policy and obligation generations and the expected set
   generation are classified against the authority vector the caller holds. A
   mismatch is `STALE` (behind), `CONFLICT` (ahead) or
   `UNKNOWN` (absent).
3. **Scoring.** The exact integer objective is computed per candidate with
   checked arithmetic.
4. **Canonical ordering.** A deterministic bottom-up merge sort over
   (score desc, rank asc, path asc). The order is total because path identity is
   unique inside a validated set.
5. **Bounded scan.** At most `policy.search_limit` members are examined.
   Reaching the limit is recorded and downgrades any "nothing found" answer to
   `INDETERMINATE_SEARCH_LIMIT`.
6. **Gating.** Thirteen gates in a fixed order, each returning a definite
   exclusion or an indeterminate block, all recorded per candidate.
7. **Selection.** The first promotable member in canonical order wins, unless a
   blocking member outranks it and the policy does not explicitly allow skipping.
8. **Outcome.** A proof-carrying refusal, an indeterminate result, or positive
   authority.

## 4. Durable format

Journal record:

    offset 0   u32 magic 0x31464650 ("PFF1")
    offset 4   u16 format version
    offset 6   u16 record type
    offset 8   u64 sequence (starts at 1, strictly contiguous)
    offset 16  u32 payload length (authenticated by header_crc before use)
    offset 20  u32 header_crc  (CRC-32C over bytes 0..19)
    offset 24  payload
    offset N   u32 payload_crc (CRC-32C over the payload)

Snapshot:

    header 40 bytes: magic, version, flags, sequence, record count,
                     payload length, reserved, header_crc, reserved
    body:           the same framed records, in a fixed order
    footer 8 bytes: total_crc (over header+body), footer magic

Snapshots are written to `fabric.snapshot.tmp`, flushed, atomically
replaced, the directory entry is synced where the platform supports it, and only
then is the journal reset.

## 5. Concurrency model

* One non-recursive mutex in `Fabric` guards all mutable state.
* Public methods take the lock; every internal helper is suffixed `_locked`
  and assumes it.
* No callbacks, no listener interfaces, no event bus: a decision is returned by
  value, which removes an entire class of "callback under lock" hazards.
* `Server` has one accept thread and one detached thread per session,
  bounded by `max_sessions`. Session sockets are shared through
  `std::shared_ptr<SocketHandle>` so close happens exactly once and
  `shutdown()` never acts on a recycled handle.
* Shutdown wakes a blocked accept with a self-connection and releases blocked
  reads and writes with `shutdown()`, never with a timeout.

## 6. Error model

* No exceptions cross the public API. Allocation failures are caught and
  returned as `Code::exhausted`.
* `Result<T>` is either a value or a `Status`. A debug-only
  assertion forbids constructing one from a success status, which would otherwise
  silently produce a failed result that reports "ok".
* Every refusal names a `Code` and a fine-grained `Reason`; the
  reason vocabulary is stable and is persisted and transmitted numerically.
