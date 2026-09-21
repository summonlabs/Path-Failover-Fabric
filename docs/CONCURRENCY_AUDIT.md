# Concurrency and ownership audit

This audit was performed by reading the call paths, not only by running tests.
Each listed hazard class was checked explicitly; the findings and their fixes are
recorded here.

## 1. Hazard checklist

| # | Hazard | Result |
| --- | --- | --- |
| 1 | read-lock then write-lock re-entry on the same lock | **clear** — one non-recursive `std::mutex` in `Fabric`; every internal helper is `_locked` and no public method is called while it is held |
| 2 | write lock held across a callback that re-enters state | **clear** — the library has no callbacks, listeners or event sinks at all |
| 3 | event/log/callback invocation beneath internal locks | **clear** — decisions are returned by value; observability is by explicit query |
| 4 | joining workers while holding state they need | **clear** — session threads are detached and publish completion through an atomic counter plus a condition variable; the waiter releases the mutex inside `wait` |
| 5 | cancellation/shutdown with reversed lock ordering | **finding 2** — fixed |
| 6 | blocked socket/thread teardown | **finding 2** — fixed; teardown is by `shutdown()` and a wakeup connection, never by a timeout |
| 7 | cross-object mutex order inversion | **clear** — the only nesting is `Server::mu_` taken while holding nothing else; `send_frame` no longer holds any lock across a blocking write |
| 8 | moved-from handle ownership | **clear** — `Fabric`, `Client`, `Server` and `Store` are non-copyable and owned by `unique_ptr`; session sockets use shared ownership; the test process helper has an explicit move |
| 9 | close/shutdown races and double-close | **finding 3** — fixed |
| 10 | callbacks retaining references to mutable state beyond lock lifetime | **clear** — no callbacks |
| 11 | data race on a member read by one thread and written by another | **finding 1, finding 3** — fixed |
| 12 | destructor that can throw or allocate | **finding 4** — fixed |

## 2. Findings

### Finding 1 — `Server::listener_` was a plain integer

`run()` (the accept thread) wrote `listener_` while
`request_stop()` read it from other threads, including the session thread
handling a `shutdown_req`. That is a data race.

**Fix.** `listener_` is now `std::atomic<std::intptr_t>`, exchanged
to `-1` exactly once by the accept loop, which is also the only thread that
closes the listener.

### Finding 2 — a blocking send held the session mutex

`send_frame` held `session->mu` across `net::send_all`. A peer
that stopped reading could therefore block a send indefinitely while holding the
mutex, and the shutdown path needed that same mutex to discover the handle. The
result was a teardown that could block behind a stuck write, and a nested lock
order (`session->mu` then `Server::mu_` for statistics).

**Fix.** Sends no longer hold any lock across the blocking write. A session socket
is a `std::shared_ptr<SocketHandle>` taken by value, which keeps the handle
valid for the duration of the call without any lock. `release_blocked_io`
snapshots the shared handles under `Server::mu_` only, copies them into a
fixed-size array (no allocation) and calls `shutdown()` outside every lock,
which releases both a blocked read and a blocked write promptly.

### Finding 3 — session handle read and written under different mutexes

`release_blocked_io` read `session->handle` under
`Server::mu_` while the session thread wrote `session->handle = -1`
and `handle_closed` under `session->mu`. Two different mutexes do not
order those accesses, so this was both a data race and a window in which
`shutdown()` could act on a handle that had just been closed and possibly
recycled by the operating system.

**Fix.** `SocketHandle` owns the native handle and closes it exactly once in
its destructor. Every user holds a `shared_ptr` copy, so the handle cannot be
closed while anyone is using it, and a double close is structurally impossible.
`Session@@ no longer has a mutex at all: the fields it still owns are touched
only by its own thread after the handshake.

### Finding 4 — the server destructor could allocate and throw

`~Server()` called `request_stop()`, which built a
`std::vector` of handles. An allocation failure inside a destructor
terminates the process; clang-tidy also flagged the potential escape.

**Fix.** `release_blocked_io` uses a fixed `std::array` sized by the
compile-time session bound, so it never allocates, and the destructor wraps its
whole body in a guard whose last-resort branch closes the listener through the
`noexcept` close helper.

### Finding 5 — `Result<T>` could be built from a success status

`Result(Status)` accepted an ok status and produced a result that reported
`ok:none` while being treated as a failure. This actually happened during
development in `Store::load_snapshot` and produced a silently failing
`Fabric::open`.

**Fix.** `load_snapshot` now returns `Status`, and the
`Result(Status)` constructor asserts `!status.ok()` in debug builds so
the whole class of bug fails loudly during testing.

### Finding 6 — a stale reason-range bound silently invalidated records

The wire decoder bounded the reason field by `complete_set_exhausted` while
the vocabulary had grown two more enumerators. Valid attempt records carrying
`authority_fenced` were rejected on replay, which surfaced as a crash in a
test that dereferenced a failed result.

**Fix.** `max_reason_value` and `max_code_value` are now derived from
the enumerators themselves in one place and used by every encoder and decoder, so
the bound cannot drift again. The test framework also gained `PFF_REQUIRE`,
which reports and returns instead of dereferencing a value that was not produced.

### Finding 7 (test infrastructure) — reference binding to a member of a temporary

`PFF_CHECK_EQ` bound its operands with `const auto&`. For an operand
such as `some_result().status().reason` the temporary dies at the end of the
declaration and the reference dangles. AddressSanitizer reported this as a
stack-use-after-scope during the sanitizer capability check.

**Fix.** `PFF_CHECK_EQ` copies its operands by value and documents why.

## 3. Confirmed-clear properties

* No public `Fabric` method is called while its mutex is held: every call
  path from a locked region reaches only `_locked` helpers or pure
  functions.
* `Selector::evaluate` is pure and is safe to call under the fabric lock.
* `Fabric` methods are safe to call from any thread, including several at
  once: the concurrency suite starts eight threads on a barrier against one
  coordinator.
* `Client` is single-threaded by contract and documented as such; it owns
  its socket exclusively and closes it exactly once, in `shutdown()` or in
  its destructor.
* Shutdown cannot deadlock: the accept loop is woken by a self-connection, session
  threads are released by `shutdown()` on a shared handle, and the waiter
  releases the mutex while waiting.
