# Authority model

## 1. The eight separations

The runtime is built so that each of the following can never be mistaken for the
next:

| # | Separation | Where it is enforced |
| --- | --- | --- |
| 1 | Matching identity is not matching generation | `AuthorityVector` is carried by every candidate, observation, attempt, grant and decision; `classify()` compares generations, never identities |
| 2 | Observation is not authority | `PathEvidence` is an input to gating; it authorises nothing |
| 3 | Eligibility is not authorisation | `Eligibility::eligible` is a gate input; a grant is issued only by `Fabric::begin_promotion` |
| 4 | Recommendation is not authorisation | `evaluate()` returns `AuthorityKind::recommendation`, allocates no attempt and changes no durable state |
| 5 | Authorisation is not application | A `Grant` authorises exactly one transition; `record_effect` is the only path to `committed` |
| 6 | Acknowledgement is not verified effect | An `ok_rsp` frame means the request was accepted; it carries no effect claim |
| 7 | Persistence is not liveness | Restart restores definitions, lineage and fences only; evidence, freshness, leases and in-flight authority are dropped and re-established |
| 8 | Absence is not indeterminate | `REFUSED_PROVEN_NO_ALTERNATE` requires a complete, fully examined, non-indeterminate, non-stale surface |

## 2. Authority vector

    struct AuthorityVector {
      Generation topology;
      Generation path_authority;
      Generation policy;
      Generation obligations;
      Epoch      epoch;   // coordinator term, advanced on every restart
      BootId     boot;    // process incarnation, fresh on every restart
    };

An absent generation (value zero) is `UNKNOWN`, never generation zero.

Per-field classification of a supplied vector against the current one:

| Relation | Class |
| --- | --- |
| both present and equal | CURRENT |
| either absent | UNKNOWN |
| supplied < current | STALE |
| supplied > current | CONFLICT |
| boot differs | STALE (a different incarnation is never "newer") |
| epoch differs and is lower | STALE |
| epoch differs and is higher | CONFLICT |

Overall class folds the per-field classes with severity
`CONFLICT > UNKNOWN > STALE > CURRENT` and names the first blocking field.

## 3. What each class means for a candidate

| Class | Candidate gate | Consequence |
| --- | --- | --- |
| CURRENT | passes | the candidate may continue through the remaining gates |
| STALE | definite exclusion | the member was declared under generations that are gone; it is not promotable and it **voids any proof about the surface** |
| UNKNOWN | indeterminate block | promotability cannot be established; fails closed unless the policy explicitly allows skipping |
| CONFLICT | fatal | caller and runtime disagree about which generation exists; the whole decision is refused |

`REFUSED_STALE_AUTHORITY` rather than
`REFUSED_PROVEN_NO_ALTERNATE` is returned whenever at least one member was
excluded for staleness, because a re-supplied set might have made that member
promotable.

## 4. Positive authority: the grant

`Fabric::begin_promotion` performs, under one lock:

1. admission control (at most one attempt owns the transition);
2. selection;
3. allocation of a monotonic attempt identity;
4. a durable `attempt_begin` record, then an `attempt_state` record
   carrying `authorized`, both flushed before the grant is returned;
5. storing the grant in memory.

The grant carries the attempt identity, the selected path and the authority
vector it was issued under. Anything that changes any generation in that vector
fences the attempt, and the grant is revoked.

## 5. Revocation

| Trigger | Effect |
| --- | --- |
| policy generation advance | in-flight attempts fenced with `authority_fenced` |
| obligation generation advance | same |
| topology or path authority advance | same |
| `fence_path` for the selected path | that attempt fenced with `path_fenced` |
| `fence_all` | every in-flight attempt fenced |
| restart | every pre-restart active attempt becomes `INTERRUPTED` with a `restart_fenced` fence record |

Fences are durable, authority-bearing records. The fence table is bounded and
refuses new fences when full instead of evicting existing authority.

## 6. Verified effect

`record_effect` accepts an `EffectEvidence` only when all of the
following hold:

* the attempt exists, is not terminal, and still owns the transition;
* the evidence is marked verified by the external observer;
* the evidence's authority vector is CURRENT;
* the evidence names the path the grant authorised;
* the observation sequence is strictly newer than the last observation for that
  path.

Only then is the attempt durably committed and a lineage entry written. An
acknowledgement, a matching identifier, or a plausible-looking report is never
sufficient.

## 7. Restart

A restart produces a new incarnation and a new boot identity, advances the
coordinator epoch, and fences everything that was in flight. Definitions
(policy, obligations, alternate sets), durable lineage and fences survive.
Evidence, freshness, leases, ownership and effects do not. The first promotion
after a restart is therefore refused until the adjacent systems re-supply their
inputs under the new authority — which is exactly what the multiprocess suite
demonstrates on real processes.
