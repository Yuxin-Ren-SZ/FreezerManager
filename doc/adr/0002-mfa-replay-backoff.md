# ADR 0002: Atomic MFA replay protection, durable backoff and administrative unlock

- Status: Proposed; requires lead acceptance after independent security review.
- Date: 2026-10-27.
- Design issue: [#208](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/208).
- Parent: [#179](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/179), still
  open and still gates non-loopback deployment. This document implements nothing.
- Source baseline: `d527c0c312554d5485c020355e6bd5c6891eb9a4`.
- Scope: local TOTP authentication. No WebAuthn, password-lockout redesign or
  credential-recovery bypass. Migration 15 is free, **not reserved** by this ADR.

## Context and source evidence

The decisions below are proposals, not descriptions of available interfaces.
Paths are relative to this document; function names identify the inspected code.

| Current behavior / constraint | Repository evidence |
| --- | --- |
| Six digits, 30-second steps, ±1 window; verification returns only bool and stops at the first match | [Totp.h](../../src/auth/Totp.h), [Totp.cc](../../src/auth/Totp.cc), `totp_verify` |
| `verify_totp(SessionId, code)` loads a session/user, verifies, updates only `mfa_complete`, commits; completed sessions are already rejected | [IAuthProvider.h](../../src/auth/IAuthProvider.h), [LocalAuthProvider.cc](../../src/auth/LocalAuthProvider.cc), `verify_totp` |
| User has a TOTP-secret field and authz epoch, but no replay or MFA-backoff state | [identity.h](../../src/core/identity.h), `User` |
| SubmitMfa uses TokenOnly then an in-memory per-user bucket: capacity 5, refill 1/minute | [AuthServiceImpl.cc](../../src/server/AuthServiceImpl.cc), `SubmitMfa`; [AuthServiceImpl.h](../../src/server/AuthServiceImpl.h) |
| TokenOnly skips data-tier limiting; permission gates require completed MFA and global permission when no lab is given | [AuthMiddleware.cc](../../src/rpc/AuthMiddleware.cc), `authenticate_impl`, `authorize_impl` |
| Session validation checks live user, revocation and idle/absolute expiry; normal validation may write activity; revalidation preserves activity | [LocalAuthProvider.cc](../../src/auth/LocalAuthProvider.cc), `validate_session_token`, `check_session_expiry`, `update_last_seen_if_needed` |
| Cached grants use live authz_version; MFA comes from the live session, not that cache | [LocalAuthProvider.h](../../src/auth/LocalAuthProvider.h), `CachedContext`; `lookup_or_build_context` in LocalAuthProvider.cc |
| No conditional update, row-lock or early-write-transaction API exists | [IStorageBackend.h](../../src/storage/IStorageBackend.h), `IRepository`, `ITransaction`, `IStorageBackend` |
| SQLite `begin(Serializable)` does not start a database transaction; reads use a connection; repository updates stage full rows; `commit()` starts BEGIN IMMEDIATE and flushes hooks | [SqliteBackend.cc](../../src/storage/sqlite/SqliteBackend.cc), `SqliteTransaction::Impl`, `commit`, `rollback`; [SessionRepositories.cc](../../src/storage/sqlite/SessionRepositories.cc), `stage_update`, `update_pending` |
| PostgreSQL starts a real transaction and sets requested isolation; updates execute immediately; commit appends audit under a database advisory lock | [PostgresBackend.cc](../../src/storage/postgres/PostgresBackend.cc), `PostgresTransaction`, `commit`; [SessionRepositories.cc](../../src/storage/postgres/SessionRepositories.cc), `write_update` |
| Both backends append queued mutation snapshots in the domain transaction; append failure aborts; snapshots are not automatically redacted | The two Backend.cc `commit` methods above; `ITransaction::note_mutation` |
| Global-only permission catalog and SystemAdmin grants exist, but no MFA-unlock permission | [permissions.h](../../src/core/permissions.h), `is_global_only_permission`, `builtin_role_permissions`; [AuthServiceImpl.cc](../../src/server/AuthServiceImpl.cc), `is_system_admin` |
| No unlock RPC or CLI command exists; freezerctl user commands currently open a backend directly | [auth.proto](../../proto/fmgr/v1/auth.proto), [CliApp.cc](../../src/cli/CliApp.cc), [UserCommands.cc](../../src/cli/UserCommands.cc), [BackendFactory.h](../../src/cli/BackendFactory.h) |
| Structured obs has request/user/lab/event fields; logs are asynchronous, nonblocking, not a durable audit sink | [Log.h](../../src/obs/Log.h), [Log.cc](../../src/obs/Log.cc) |
| Password-only and API-token flows can set mfa_complete=true without TOTP; legacy schema defaults completed; generic User updates can replace secrets, and demo setup currently uses direct SQL | [LocalAuthProvider.cc](../../src/auth/LocalAuthProvider.cc), `do_password_auth`, `do_api_token_auth`; [session.h](../../src/core/session.h); both IdentityRepositories.cc update paths; [demo-setup.sh](../../scripts/demo-setup.sh) |
| Audit verifier already detects duplicate predecessors, orphan rows and hash mismatches; current PostgreSQL schema makes this_hash unique but not prev_hash | [AuditChainVerifier.cc](../../src/audit/AuditChainVerifier.cc), `verify_audit_chain`; PostgresBackend.cc migration 1 and `commit` |
| Every mutation must audit atomically; SQLite contention returns Unavailable without an application writer mutex | [AGENTS.md](../../AGENTS.md), §§3–6; [PRD.md](../PRD.md), §§6, 7, 17 |

A provider mutex or a boolean verifier followed by a normal User update cannot
solve cross-process replay. In particular, SQLite's present isolation enum does
not protect the reads that decide acceptance. Its full-row Session updates also
allow a previously read activity/revocation snapshot to overwrite a later MFA
change. This source-level finding is tracked separately as
[#211](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/211); its deterministic
runtime reproduction is pending. This ADR claims no reproduced interleaving.
These are explicit prerequisites, not capabilities assumed below.

The current provider feeds `totp_secret_enc` directly to the base32 verifier.
This ADR does not invent a KMS decrypt operation or claim that enrollment/secret
storage is repaired. Do not copy that field into the new security state or its
audit records. Secret replacement is a separate audited lifecycle, described
below; it is never an unlock side effect.

## Decision: return all matching counters and consume a monotonic floor

Add a pure matcher in `auth/Totp.*` returning the set of matching nonnegative
64-bit counters, rather than just bool. Keep the generic generation helper; the
local MFA security profile is exactly step=30 seconds, window=1, digits=6.
Validate this profile at startup, fail closed on unsupported configuration,
negative time or counter arithmetic overflow. Validate codes as exactly six
ASCII digits with no trimming. Malformed submitted codes count as failures once
admitted; malformed stored secrets or crypto errors are server failures, not
user mistakes. Compare fixed-length generated codes with constant-time equality
and evaluate the whole window, without first-match short circuiting.

At authoritative wall time `now`, `T = floor(now / 30)`. Evaluate all valid
counters in `[T-1, T, T+1]` using the locked user's current secret. Let `M` be
all matching counters and `F` the stored last accepted step (NULL = no step
consumed). Accept only when `M` is nonempty and **every** member is greater than
`F`; consume `max(M)`. Reject empty M or any member at/below F with the same
wrong-code outcome. Never discard old matches first and then accept a newer
match of the same digits. Consuming the largest coincident match prevents a
second session using the same observed digits at another counter in that window.

The floor is per user, shared by every session and server process using the DB.
It never decreases on failure, restart, backoff expiry, success or unlock. A
future-window match advances the floor to T+1, so the user may have to wait until
a genuinely newer counter is in the window. Clock rollback never lowers F:
older/current codes remain rejected until time catches up. A later six-digit
collision outside the original window is indistinguishable from a newly generated
code; this is counter replay protection, not a global ban on previously seen
numeric strings. Do not store codes or their hashes to imply stronger protection.

A repeated call on an already-complete session remains InvalidCredentials, even
with a fresh code; it consumes no step and changes no failure state. This retains
the current contract. A lost success response can be reconciled with WhoAmI using
the same credential; SubmitMfa is not changed into a success-idempotent endpoint.

## Decision: durable, bounded per-user backoff

Add a private typed `MfaSecurityState` entity/table keyed by `UserId`, foreign-keyed
to users, with no lab scope and no public query endpoint:

| Field | Meaning / initial value |
| --- | --- |
| `last_accepted_step` | Nullable signed 64-bit floor F, initially NULL |
| `failure_count` | Saturating integer n in 0..17, initially 0 |
| `next_allowed_at` | Nullable UTC microsecond timestamp, initially NULL |
| `observed_at` | Last committed evaluation/unlock UTC time high-water mark, initially NULL |
| `totp_generation` | Private random UUID identifying the current enrollment, NULL if no secret; never a hash of the secret |

Use database constraints for counts/counter ranges. The migration creates an
**empty** state table, with no existing-user backfill. All users, including those
predating the migration, get state lazily under the same user lock when an
eligible operation first needs it: enrolled-user login, an admitted pending MFA
evaluation, authorized unlock, or the secret lifecycle below. Read the locked
User to assign a fresh random generation if it already has a secret, otherwise
NULL. Insert plus exactly one `auth.mfa_state_initialized` audit event commits
in the operation's transaction. No migration/runtime insert bypasses that event;
rollback removes both. Missing state is an initialization requirement, not a
reset or permission bypass. A blocked/rejected request must not create state
before its credential/authority checks; a newly initialized state has no prior
backoff to refuse.

One row per real user bounds durable storage; arbitrary email, IP, token or
session values cannot create extra failure rows. Repository update/soft-delete
must not erase an existing state to recover the initial allowance. Retain F and
backoff across secret replacement; initialization is not repeated. The generation
and lifecycle contract below prevents a pre-enrollment or legacy completed
session from becoming a TOTP-proven administrator. A future counter-profile
change requires its own migration/lifecycle decision.

Default policy is fixed for this implementation; no environment variable silently
disables it. For an admitted incorrect/malformed/replayed code, increment n once,
saturating at 17, then set the next deadline as follows:

| New n | Delay from this committed evaluation |
| --- | --- |
| 1..4 | 0 seconds (next_allowed_at = NULL) |
| 5..16 | `30 * 2^(n-5)` seconds: 30 seconds through 61,440 seconds |
| 17, and every later failure at 17 | 86,400 seconds (24-hour cap) |

No inactivity decay and no daily/reset-on-login allowance exist. At the cap,
there is at most one evaluated guess per 24 hours per user, regardless of process
restarts or new sessions. Unlike permanent lockout, eventual retry does not
require a surviving administrator. A successful eligible MFA resets n/deadline;
it retains F (advancing it) and observed_at. Only explicit admin unlock can reset
the budget without a valid fresh code. Login/password success does not reset it.

Read wall time after acquiring locks. For backoff and in-transaction session
expiry use `effective_now = max(wall_now, observed_at)`; update observed_at only
on committed evaluated attempts or unlock. TOTP matching uses wall_now, never
an invented future time. A backwards clock cannot shorten a persisted deadline
or extend expiry relative to this recorded high-water mark; it can make users
wait longer and trigger replay rejection. A forward clock jump can expire a
deadline, but cannot reset n: each failed evaluation still increases/saturates
it. Correct time on hosts sharing the DB is an operational prerequisite; this
is not protection against an administrator repeatedly manipulating the clock.
Use checked/saturating timestamp addition and reject unrepresentable time as a
server error; integer overflow must never produce an immediately eligible retry.

When effective_now is strictly before next_allowed_at, refuse without evaluating
the code or updating any state/audit. Equality permits one evaluation. Repeated
blocked submissions neither spend a guess nor extend the delay. Eligible valid
codes cannot bypass backoff. Admission limiter refusal similarly never evaluates
a code or mutates durable guessing state. No lockout state is exposed to anonymous
requests; SubmitMfa derives the subject only from a valid session credential and
accepts no user/email argument. TokenOnly failures retain generic credential
errors; user-keyed events are emitted only after a trusted user is resolved.

## Decision: add an early, database-backed auth transaction mode

Proposed storage API (names are part of the proposal, absent today):

```cpp
// Attempts all requested user locks in sorted UUID order before repo reads.
// Missing rows are recorded, not thrown; expose Locked/Missing status per ID.
// Returns the usual typed repositories and audit hooks, in this locked mode.
virtual std::unique_ptr<ITransaction>
begin_auth_transaction(const std::vector<core::UserId>& users) = 0;
```

The vector is nonempty, deduplicated and sorted by the backend. Add
`ITransaction::auth_user_lock_status(UserId)` returning Locked or Missing for
each requested ID. The backend records which rows it actually locked and does
not throw NotFound for missing IDs or synthesize rows. A later concurrent insert
cannot convert a recorded Missing status into a locked row for this operation.
An actor missing at this point is an invalid credential; a missing target is
not reported until the actor's locked authorization succeeds. Auth code may prelookup a bearer solely to learn its candidate user ID, then must resolve/hash-check that same credential
again inside this transaction. No caller-supplied user ID authenticates a session.
Add typed repository/traits/column support for MfaSecurityState to both backends.
MfaSecurityState updates also require this locked mode; repository validation
rejects lowering a non-NULL floor or replacing it with NULL. The provider uses
find_by_id so it sees staged state, with explicit read-your-writes conformance.
Only initialization and replace_totp_secret may assign/change totp_generation;
ordinary state updates preserve it and cannot erase a state row.
Higher layers use those interfaces/DSL only; all lock/SQL mechanics stay inside
Backend implementations. There is no fallback to `begin(Serializable)` for a
backend lacking this capability; its implementation must fail closed.

**SQLite:** begin this mode with BEGIN IMMEDIATE before returning the transaction
or allowing repository reads. Retain the database writer lock through all reads,
staged writes, audit append and COMMIT. Modify commit to avoid a second BEGIN;
rollback/destructor must execute a real rollback in this mode, unlike today's
flag-only rollback. On constructor/flush/audit/commit exceptions release the
transaction/connection safely. Retain ordinary transaction behavior outside this
mode. Map SQLITE_BUSY/LOCKED, including relevant extended busy/locked variants,
to Unavailable, using the existing bounded busy timeout (default 5 seconds).
No writer mutex, retry loop or sleep is added at application level; return
UNAVAILABLE so callers can retry the entire request with backoff. Never classify
contention as a wrong code or consume guessing state without commit.

**PostgreSQL:** use READ COMMITTED plus SELECT FOR UPDATE on each users row in
sorted UUID order, before security/session/permission reads. All MFA operations
for a user take that same existing row lock, including lazy state creation and
unlock, so there is no absent-row race. This is a dedicated locking contract,
not a claim that ordinary ReadCommitted serializes auth. Row locks last through
commit; use a bounded 5-second local lock timeout mapped to Unavailable. Preserve
serialization/deadlock mapping to SerializationFailure (gRPC ABORTED). Do not
retry automatically inside the provider. Retain the existing database advisory
lock for audit append, **plus the common predecessor-uniqueness protocol below**.
READ COMMITTED refreshes only this auth transaction's statement snapshot; an
ordinary SERIALIZABLE or REPEATABLE READ appender may still have an older
snapshot after acquiring the same advisory lock. The lock alone is not a
cross-isolation chain guarantee.

**Session writer prerequisite:** migrate every production Session update and
soft-delete path to acquire its owning user lock and reread the row inside this
mode: MFA, `update_last_seen_if_needed`, `revoke_session`, `revoke_all_sessions`.
Session creation in password login also locks/rereads the user before deciding
whether MFA is required. Refresh activity from that row, never the snapshot
passed to the helper; preserve live MFA/revocation. For revoke-all, query the
sessions only after locking. Reject repository Session update/soft-delete outside
this locked mode (fixtures must use it too); ordinary read-only validation stays
read-only. Inserts may only create fresh IDs and must not upsert existing rows.
This closes the stale full-row activity update that otherwise could clear MFA
or resurrect a revoked session after the atomic acceptance. No cache is an
exception: later validation reads authoritative MFA/status as today. Coordinate
this prerequisite with #211 rather than independently landing conflicting
activity fixes; reproduce its source finding with barriers before implementation.

User status/secret writes on PostgreSQL already acquire the same users row lock
when executed; require security-sensitive read-modify-write enrollment paths to
enter this mode before reading. SQLite's immediate transaction excludes concurrent
DB writers. Do not store new MFA fields on User, where an unrelated stale full-row
User write could replace them. Permission/role mutations must keep their current
in-transaction users.authz_version bump; unlock re-resolves current grants under
the actor user lock instead of trusting a cached opening context.

## Decision: one predecessor can have only one committed audit successor

Add a nondeferrable PostgreSQL constraint named
`audit_events_prev_hash_unique`, UNIQUE(prev_hash), and the equivalent named
unique index on SQLite. Include **all** values, including the zero-hash genesis
predecessor; a partial non-genesis index would still allow two first rows. Keep
the existing this_hash uniqueness and immutable-row rules. Apply this to every
audit writer, not only MFA: ordinary and auth transaction commit share the same
append path, with no alternate unguarded INSERT. This adds storage migration
work; no constraint currently provides this guarantee.

PostgreSQL retains its advisory transaction lock and existing tail walk. Replace
each audit INSERT in the common commit loop with a targeted conflict action:
`ON CONFLICT ON CONSTRAINT audit_events_prev_hash_unique DO NOTHING RETURNING id`.
Require exactly one returned ID. Zero rows means another committed append already
claimed that predecessor: immediately throw `storage::SerializationFailure`
with fixed safe text. Abort the entire transaction, including every earlier
audit row in this batch and every domain write. Native SQLSTATE 40001/40P01 also
remains SerializationFailure/ABORTED. Other unique constraints keep their existing
mapping; do not relabel every 23505 as a retry. The named conflict target avoids
parsing localized exception text or assuming libpqxx exposes a constraint-name
diagnostic (the current repository only examines sqlstate). No provider-internal
retry, second connection, separately committed audit or audit-only retry exists.
A caller retries the complete RPC in a fresh transaction, rechecking credentials,
expiry, replay and durable backoff; its first failed commit grants no authority.

The database's uniqueness check arbitrates even when a transaction's tail query
cannot see the winning row. Thus an old snapshot can choose an obsolete tail,
but cannot commit a fork; it must abort or encounter the named no-insert outcome.
The auth operation's success/failure event is emitted only after a successful
commit, as elsewhere in this ADR. In SQLite, BEGIN IMMEDIATE already excludes a
competing appender; predecessor uniqueness is the same final integrity guard,
and an unexpected collision is a server invariant failure with full rollback.
Never suppress that collision and continue to the next audit row.

For migration, stop old writers and acquire an exclusive audit-table write lock
(PostgreSQL ACCESS EXCLUSIVE; SQLite BEGIN IMMEDIATE) in the same migration
transaction. Extend the backend migrator with a preflight hook for this migration:
read the full existing chain through backend-owned row mapping and call the pure
`audit::verify_audit_chain` before creating the constraint, state table, proof
columns or recording the migration version. Empty chain is valid. Any existing
fork (including genesis), duplicate, orphan or hash mismatch fails migration
closed, rolls back all changes/version metadata and prevents startup. Never
choose a branch, delete/rewrite audit rows or accept an arbitrary tail. Creating
the unique constraint is also inside this transaction; failure is MigrationFailure,
not a transient RPC conflict. Existing migrations lack this C++ verification hook;
adding it is explicit implementation work. Recovery of a pre-existing bad chain
requires a separately reviewed operator plan, not this migration or an unlock.

A deterministic live PostgreSQL test must begin an ordinary SERIALIZABLE writer,
establish its snapshot and stage a domain mutation, pause it before audit append,
then commit an auth READ COMMITTED writer against the same chain on a separate
connection. Resume the ordinary writer: it must abort with SerializationFailure,
its domain/audit changes must be absent, and one verified chain must remain.
Retry the whole ordinary operation freshly and require success. Repeat with
REPEATABLE READ, an initially empty chain, a multi-event audit batch, reversed
writer order and native 40001/40P01 injection. A named-predecessor no-insert test
pins the explicit result-count translation separately from native serialization
failure. Unrelated uniqueness violations must retain their normal statuses.
This is a required regression plan, not a reproduced runtime result.

The snapshot limitation is documented by PostgreSQL 16's
[explicit locking guidance](https://www.postgresql.org/docs/16/applevel-consistency.html#NON-SERIALIZABLE-CONSISTENCY).
Its [INSERT reference](https://www.postgresql.org/docs/16/sql-insert.html#SQL-ON-CONFLICT)
specifies named conflict targets and RETURNING for inserted rows. The uniqueness
protocol above is this ADR's inference/design using those semantics and current
source; no mixed-mode runtime reproduction is claimed here.

## Decision: persist TOTP proof separately from generic session completion

Add private nullable Session fields `totp_verified_generation` (UUID) and
`totp_verified_at` (UTC timestamp), both NULL on every new session insert and on
all legacy rows at migration. Do not backfill proof from mfa_complete, enrollment,
role or creation time. Database constraints require both proof fields to be NULL
or both present; present proof requires mfa_complete=true. Generic Session
insert/update may not supply/alter proof. Add a backend-owned typed
`ITransaction::complete_totp_session(session_id, generation, verified_at, ctx)`
operation usable only in locked auth mode; it requires a live pending session
owned by the locked user and the generation currently in that user's state.
Only the provider's successfully matched, freshly consumed step invokes it.
It sets mfa_complete and proof in the same audited transaction as consumption.
Touch/revoke preserve existing proof, and generic mfa_complete assignment cannot
manufacture proof. Keep proof out of public protobuf/WhoAmI/log output.

Unlock requires all existing credential/permission checks **and** non-NULL session
proof matching the locked MfaSecurityState.totp_generation of an enrolled user.
Missing state, NULL/legacy proof, or a generation mismatch denies with a fixed
PERMISSION_DENIED outcome and no target detail/mutation. No lazy creation or
proof assignment occurs to make the actor pass. A password-only session can
still be mfa_complete under today's general session semantics, but cannot unlock.
A genuine completed session at the current generation can unlock self/others;
its proof is not a cached flag or inferred from the existence of today's secret.

Add one typed backend lifecycle operation for secret enrollment/replacement/
removal, `ITransaction::replace_totp_secret(user_id, optional_secret, ctx)`,
requiring that user's auth lock. Generic User updates must exclude the secret
column entirely at the actual SQL write; reject attempts to change it through
that interface. Generic User insertion requires secret=NULL; provisioning must
subsequently invoke this operation. This prevents an unrelated stale User copy
from reverting a secret after the lifecycle commits. The lifecycle lazily
initializes state if missing (one initialization audit), writes the new secret,
sets a freshly generated UUID for every explicit enrollment/replacement (including
same-value replacement), or NULL for removal, and revokes **all** active user
sessions, pending and completed. Preserve F, count and deadline. Commit secret,
generation, revocations and sanitized audit effects together; failure rolls all
back. A replay floor cannot be cleared even if the secret is removed/re-enrolled.
Only explicit admin unlock or valid MFA can replenish guessing allowance.

This is required dependency work, not an already available enrollment API. Both
IdentityRepositories and SessionRepositories need the write guards/typed lifecycle.
Queue one `auth.totp_secret_changed` action containing only old/new enrolled
booleans and generation IDs, one `auth.mfa_generation_changed` state action,
and one `auth.session_revoked` action per actually revoked session. A lazy
initialization adds its own event. No generic User snapshot or duplicate update
audit is queued; audit/log/status never contain either secret or auth bindings.
The existing direct-SQL secret update in scripts/demo-setup.sh must be removed
or refused before enabling this feature; demo/bootstrap must invoke the same
backend-backed audited lifecycle through a trusted provisioning adapter. Fixtures,
seeding, imports, CLI password/profile writes and future enrollment providers
must obey these guards. UserImport already ignores secret fields; preserve that.
Explicitly test both backend repositories, the demo adapter and any provisioning
command rather than treating a source search as a complete enforcement boundary.
Direct privileged DBA writes remain outside application guarantees.

Cutover revokes all pre-cutover sessions in an audited operation, and legacy
proof remains NULL regardless. Restore similarly revokes restored sessions. An
operator obtains recovery authority by a fresh password login for an enrolled
SystemAdmin followed by a real accepted TOTP through SubmitMfa; only that new
session carries proof. If the admin's durable backoff is active, use another
TOTP-proven admin session to unlock it, or wait until the bounded deadline and
complete a fresh valid code. Secret setup or flipping mfa_complete cannot mint
recovery authority. Keep the existing rejection of SubmitMfa on a completed
session; legacy/password-only users must sign out and log in again to obtain a
pending session, not silently upgrade a completed one.

## Decision: one committed outcome before returning an auth result

Extend IAuthProvider's C++ verification operation to take the session bearer and
MutationContext (instead of authenticating solely by SessionId). Update its sole
runtime implementation and all fakes/compile mocks. SubmitMfa still gates through
`authenticate(TokenOnly)` and the same registry entry; pass the bearer only in
memory. Proposed provider flow:

1. Resolve the candidate session owner without recording activity; reject API
   tokens (which have no real pending Session). Acquire that user's auth
   transaction, then recheck bearer hash, ownership, active user, non-revoked
   session, current secret and idle/absolute expiry. Recheck time immediately
   before staging a transition; if it has expired, do not elevate it.
2. Reject completed sessions before backoff or matching, with no durable write.
   A missing secret is a generic credential failure without budget change; an
   invalid stored secret is a masked server error. Neither is an incorrect code.
3. Load/create security state, check durable eligibility. If blocked, leave state
   unchanged and return a typed RateLimited outcome after releasing the lock.
4. Compute matches. On failure, stage n/deadline/observed_at, queue a sanitized
   `auth.mfa_failed` audit mutation with reason category `invalid_or_replayed`,
   and commit. **Only after commit succeeds**, emit failed-code obs and throw
   InvalidCredentials. Do not throw inside a scope that rolls back the failure.
5. On success, stage F=max(M), reset n/deadline, update observed_at, set only this
   session's MFA complete, current-generation proof and activity time using
   complete_totp_session from the locked row; queue
   `auth.mfa_step_consumed` and `auth.mfa_completed` audit effects; commit once.
   Return success only after commit. No other pending session is elevated.

Existing repo auto-auditing must not accidentally duplicate or leak the semantic
audit. Add backend-derived allowlisted snapshots for these auth writes, not
caller-supplied JSON. MfaSecurityState snapshots contain only its five fields
and user ID; fixed event reason categories may be added as server-derived
allowlisted metadata (not merely MutationContext.reason, which today's commit
format does not persist). Session snapshots for these operations contain ID,
user ID, mfa_complete, TOTP proof generation/time and activity/revocation timestamps as
needed, excluding token hash, prefix, IP and user-agent. Do not mutate a User merely to consume a step: its
current JSON snapshot contains auth bindings and the TOTP field. All failure
state writes, initialization and session writes still receive their own audit
effects in the same transaction; transition events may be the repo action rather
than an extra generic update event. Use exactly one
`auth.mfa_state_initialized` event for a new row, one `auth.mfa_failed` event per committed failed evaluation, two events for an
existing-state success (state consumption plus session completion), and one
`auth.mfa_unlocked` per unlock including no-op. Initialization adds one event to
that operation's count. No duplicate generic update event is queued. Pin these
counts and the before/after snapshots in tests.

If audit/flush fails, roll back floor, counters, deadlines, session and all audit
rows together; return a masked server/storage error, not wrong-code or success.
An ambiguous network commit is never retried internally: a fresh request reads
DB truth. A committed failure remains spent; a committed success remains elevated
and consumed. Keep UNAVAILABLE and ABORTED for backend availability/conflict
failures. Add an auth-operation internal-failure translation for crypto, invariant and audit-append
failures: fixed INTERNAL text and allowlisted obs category even in debug mode.
In particular, today's injected audit failure is a ConstraintViolation that the
generic translator maps to INVALID_ARGUMENT; do not misreport this server fault
as a client mistake or log its raw exception text. An actually rolled-back
correct attempt can succeed on retry if still fresh/eligible; this is not a second committed consumption. Concurrent
requests serialize by user: losers reread F/backoff/completed-session status,
so at most one transaction consumes the matching step. Revocation/disable ordered
before acceptance wins; ordered after acceptance revokes/disables that session
normally. Storage errors never grant authority from in-memory provisional state.

## Decision: authenticated, deployment-wide administrative unlock

Add global-only `Permission::MfaUnlock`, key `auth.mfa_unlock`, seeded only to the
built-in SystemAdmin role in both migrations. Add it to permission parsing,
catalog/global-only tests and scope validation. LabAdmin, custom lab roles and
`user.manage_roles`/`session.revoke` alone confer no unlock authority.

Add additive `AuthService.UnlockMfa(UnlockMfaRequest) -> UnlockMfaResponse`;
request has target `user_id` UUID and a required fixed reason enum
`OPERATOR_VERIFIED` (reject unspecified/unknown values), response is empty.
Use no free-text reason field that could collect PHI. Register
`/fmgr.v1.AuthService/UnlockMfa` as `RpcGate(Permission::MfaUnlock)`; handler calls
`middleware.authorize(extract_bearer(ctx), Permission::MfaUnlock)` with **no lab**.
This enforces token + completed MFA + current global permission and charges the
data-tier limiter. Do not register TokenAndMfa while manually asserting a
permission; keep registry and enforcement identical.

Additionally require a real, non-revoked/unexpired session of an active user
with a configured TOTP secret, completed MFA and the durable current-generation
proof defined above; mfa_complete plus today's secret alone is insufficient.
Reject PAT/machine credentials for this human recovery action even if their scope includes the permission.
The provider's new `unlock_mfa(actor_bearer, target_user, MutationContext)`
re-resolves/hash-checks that session and its current global grants inside the
same auth transaction, locking actor and target users in sorted order. Admission
is not transactional authority: a permission revoke/disable completed before this
locked recheck must deny the mutation. Verify MutationContext actor/session are
those authenticated, and use server-derived request identity. The sorted lock
attempt may record an absent target before rechecking the actor, but must not
throw or return a target error. First check the actor's Locked status, bearer,
expiry, active status, current grants and proof; deny those failures even if the
target was Missing. Only an authorized actor may then translate target Missing
to NOT_FOUND. Never query/mutate a subsequently inserted row that was not locked;
retry through a fresh operation if the target is later created. Missing actor
returns a generic credential failure, never the target's existence status.

A SystemAdmin can unlock any active TOTP-enrolled user deployment-wide, including
another SystemAdmin or themselves **using an existing completed session**. A
pending session cannot unlock itself or others. For authorized callers only,
unknown target returns NOT_FOUND; inactive/non-enrolled target returns
FAILED_PRECONDITION with fixed text. Unauthorized callers fail before target
validation/disclosure and receive no existence/backoff detail; the lock layer may
already have recorded presence/absence as specified above.

In the locked transaction reset only failure_count=0 and next_allowed_at=NULL,
advance observed_at to max(old, wall_now), retain last_accepted_step exactly,
and append `auth.mfa_unlocked` with actual admin actor, target entity ID, request
ID and allowlisted before/after state. An already-clear target is a successful
no-op but still records the admin action. Audit failure rolls back the reset.
No secret, role, MFA requirement, current session MFA flag or consumed floor is
changed; no session is minted and no code is consumed. Concurrent unlock/attempt
follows the serialized order; an attempt after unlock can spend the fresh budget.
Repeated unlocks intentionally reset it again and remain attributable, not a
client retry token granting a permanent exemption.

Add `freezerctl auth mfa-unlock --server <endpoint> --user <uuid>` as a new gRPC
client command; obtain the admin session from a protected token file or stdin,
never argv, logs or output. It invokes this RPC over the same production TLS
policy, does not take --sqlite/--postgres, and prints a fixed success/failure
message plus target UUID. Current freezerctl has no such authenticated command
plumbing: implement client/channel, credential input, errors and tests rather
than implying it already exists. No new REST route/web UI is required for the
admin path; existing SubmitMfa REST/browser routes retain their status behavior.

Operators verify the requester out of band before choosing OPERATOR_VERIFIED,
then ask the user to retry after any local admission limiter refills (at most a
minute for one token under the current bucket). Unlock does not clear per-process
DoS buckets. A completed admin session can perform self-unlock; otherwise use a
second enrolled SystemAdmin or wait out the bounded 24-hour backoff. If no admin
credential is available, this protocol provides no unauthenticated/DB-direct
bypass. It does not justify lowering F, clearing secrets or restoring an older
backup as an unlock technique. Database restore can inherently roll back security
state: recovery must revoke sessions and allow the old TOTP window to pass before
reopening auth; backup restore is not equivalent to a process restart guarantee.

## Decision: admission layers and observable failures

Keep Login's per-IP limiter and existing password lockout separate. SubmitMfa
remains TokenOnly and outside the data-tier limiter; retain its per-user
in-memory 5/1-minute bucket as an additional DoS boundary, before the provider's
durable transaction. Its eviction/restart/reset can increase admission but never
reset durable n/F. Authenticated API tokens and already-completed sessions do
not get an MFA guess; any admission token they spent is not refunded.

Change SubmitMfa's credential admission to a named non-renewing middleware path
that checks the same TokenOnly registry rule using `revalidate_token`, **while
still charging its MFA admission bucket once**. The existing `reauthenticate`
can supply that read-only credential gate; its use here must be explicit and
tested so the handler's own admission limiter is not omitted. Attempts refused
by backoff/DoS and wrong codes do not refresh idle time. Only committed successful
MFA records activity; absolute expiry never moves. Preserve existing 12-hour idle,
7-day absolute defaults and boundary comparisons; the locked provider recheck
prevents expiry/revocation races after admission. Expired/revoked sessions return
UNAUTHENTICATED before evaluating a code, preserving durable state. They can
log in again, but inherit that user's durable budget and floor.

| Outcome | Client status | Obs event | Durable audit |
| --- | --- | --- | --- |
| Evaluated wrong/malformed/replayed code, committed | UNAUTHENTICATED, fixed invalid-code text without MFA_REQUIRED marker | `auth.mfa_failed` | Yes, atomic with failure state |
| Per-user admission bucket refused | RESOURCE_EXHAUSTED, fixed slow-down text | `auth.mfa_admission_refused` | No: no durable mutation/evaluation |
| Durable backoff refused | RESOURCE_EXHAUSTED, fixed slow-down text | `auth.mfa_backoff_refused` | No: read-only refusal |
| Successful consumption/elevation | OK | Optional `auth.mfa_completed` after commit | Yes, both effects atomic |
| Storage/crypto/audit failure | UNAVAILABLE / ABORTED / fixed INTERNAL as specified above | `auth.mfa_storage_error` with fixed category | No partial audit/state survives |
| Admin unlock | OK | `auth.mfa_unlocked` after commit | Yes, including no-op admin action |

Emit one obs call for every evaluated failure and every limiter refusal after
user resolution, using `obs::log_event` with `actor_user_id` = trusted user UUID,
request_id from server context, lab_id=NULL and fixed event/message strings.
Never log submitted codes, generated codes, secrets, secret hashes, bearer/token
prefix/hash, email, display name, IP, user-agent, entity snapshots or exception
payloads. Pass only whitelisted server-error categories, including when internal
error masking is off. Unknown/invalid credentials cannot be assigned a user ID:
use a separate anonymous fixed event if needed, never a request-supplied identity.
Metrics may aggregate event counts but must not label by user or token.

Obs is best effort: the current async sink can drop under pressure and does not
survive a process crash. There is no sampling before these calls in the initial
implementation. External collectors may aggregate/rate-limit deliveries, but
this cannot suppress durable failure-mutation audit. Auditing every read-only
refusal would turn floods into DB/audit writes and worsen SQLite contention;
those refusals use obs only. Audited eligible failures are bounded by the durable
policy per user; persist them even if obs delivery fails. A failed storage attempt
may have no durable audit, but cannot elevate MFA or grant an unaudited guess
result. Alert on repeated user-keyed failures/refusals and storage errors using
these event tags; logging is not part of the authentication commit path.

## Alternatives and accepted costs

- Boolean verification + User read/update or a provider mutex: rejected for
  counter ambiguity, SQLite deferred reads and multi-process races.
- Conditional floor update alone: rejected; it omits durable failure serialization,
  atomic elevation and stale Session writers. A future general compare-and-swap
  storage interface is larger than this dedicated transaction mode.
- Persist only the existing token bucket: rejected; its sustained one guess per
  minute remains too permissive. Durable exponential backoff has no idle decay.
- Permanent lock after N failures: rejected for unattended small-lab recovery;
  the one-day cap limits slow guessing while permitting recovery with a valid
  factor. Password holders can still deny a user MFA by spending their budget;
  notify operators and provide audited admin unlock.
- Clear replay floor on unlock: rejected. Secret replacement must be independently
  audited and cannot be used to roll the counter backwards.
- Audit every refusal: rejected for unbounded write load; obs-only refusals are
  deliberately less durable than actual failure-state mutations.

## Dependent implementation phases and acceptance gate

Lead creates separate assigned issues; these are dependencies, not completed
checklists. No runtime piece may be described as closing #179 before all phases
and independent review pass. Acquire locks only on actual implementation
assignment; choose the then-current migration number for both backends.

At cutover stop all old server processes: mixed-version auth writers cannot
honor the new locking/budget/append contract. Preflight must reject a pre-existing
bad audit chain before schema/version changes. After migration revoke **all**
pre-cutover sessions through an audited operation and wait a full 90 seconds
before reopening auth, so pre-hardening codes are outside the accepted window. New floors start
NULL only for this clean cutover. Do not downgrade to a server that ignores the
state while exposing auth. Subsequent restarts need neither reset nor warmup.

1. **Pure policy and typed contracts:** red matcher/collision/backoff/time tests;
   define MfaSecurityState, durable session proof, startup profile validation,
   clock injection and auth transaction/provider contracts. Update fakes with error
   injection. No feature enablement until storage implementations exist; unsupported paths fail closed.
2. **Storage foundation (lock:migration):** both-backend schema/state registration,
   early auth transactions, session writer guard/rereads coordinated with #211,
   predecessor uniqueness/common append conflict handling, audit-chain migration
   preflight, empty-table lazy initialization and nullable legacy proof fields.
   Add secret lifecycle/proof write guards, safe snapshots and failure injection;
   adapt demo/provisioning and all Session/User writers. Add MfaUnlock catalog and
   seeded SystemAdmin grant. Conformance proves cross-process locking, mixed-mode
   audit safety, real proof provenance and atomic rollback.
   Never ship a migration-only claim that replay is solved.
3. **Provider and SubmitMfa:** consume matches, committed failure backoff, live
   credential/time checks, non-renewing admission, obs and status mapping. Wire
   policy through both backends; preserve rejection of completed sessions and
   existing REST/browser wrong-code versus 429 behavior from #181.
4. **Admin recovery (lock:proto):** additive messages/RPC, exact permission gate,
   transactional current-authority/current-generation proof check, absent-target
   error ordering, gRPC freezerctl command + TLS/credential plumbing and operator instructions. Update descriptor registry, all permission
   catalogs/clients and fakes; generate clients from proto, never edit outputs.
   Coordinate any build-graph edits with lock:ci; no new dependency is assumed.
5. **Adversarial completion:** deterministic backend/provider/integration tests,
   full dev and required storage/auth sanitizer runs, live PostgreSQL CI and
   operator runbook. Lead verifies every #179 criterion and reviews residual
   risks before closing it or changing the non-loopback deployment gate.

Phases 2–4 edit shared auth/storage interfaces; serialize them or define narrow
nonoverlapping follow-up assignments. Required storage/proto/CLI changes are
implementation scope, not permission to edit them in this docs PR.

## Deterministic regression and review matrix

Inject a clock into the provider/policy, fixed test-only secrets and controlled
matcher collisions. Use barriers/failpoints rather than sleeps or lucky codes.
Never use a real user's identity/secret in fixtures. A genuinely failing test
precedes each implementation change; the following are planned, not executed.

| Area / likely test files | Required executable cases and assertions |
| --- | --- |
| [totp_test.cpp](../../tests/unit/totp_test.cpp) | Fixed T-1/T/T+1 boundaries; invalid length/non-ASCII/whitespace; whole-window matching; counter zero, negative/overflow time, bad config/secret; collision M={T,T+1} consumes T+1; any old match rejects even with a newer match; later independent numeric collision semantics |
| Pure backoff tests beside local auth tests | n=1..17 exact delays, saturation and 24-hour cap; deadline-1us blocked / equality eligible; no inactivity reset; backward/forward clock and timestamp overflow; success/unlock reset only allowed fields |
| [storage_backend_conformance_test.cpp](../../tests/backend_conformance/storage_backend_conformance_test.cpp), SQLite/Postgres wrappers | Missing/new user state race; rollback/destructor release early locks; Session mutations outside auth mode fail closed; staged read-your-writes; unknown users; count constraints; fail_next_audit_append on initial state, failure, success and unlock leaves no partial state/audit; safe snapshot allowlist |
| Migration and common audit append tests | Empty table migration creates zero state entities/events; first old/new-user use initializes exactly once with its own audit; initialization/operation rollback removes both; forked genesis/non-genesis, orphan and hash mismatch preflight fail atomically with version unchanged; uniqueness covers genesis; mixed READ COMMITTED auth vs ordinary SERIALIZABLE/REPEATABLE READ paused-snapshot tests described above roll back losing domain/audit and allow fresh whole-operation retry; unrelated unique failure stays nontransient |
| Same conformance, separate backend objects/processes sharing one DB | Two pending sessions submit same counter under a barrier: exactly one committed elevation; losers reject/retry transient conflict then reject replay; different fresh counters serialize monotonically; concurrent failures accumulate without lost updates; unlock versus evaluation follows serial order; different-user concurrent commits retain one verified audit chain |
| [local_auth_provider_test.cpp](../../tests/unit/local_auth_provider_test.cpp) | Same code in second session rejected; next fresh step succeeds; completed session still rejected; future match/clock rollback/restart retain floor; wrong/malformed/replayed code commits failure before throw; backoff refuses even valid code; failed-code rollback/crypto/storage errors do not spend a durable attempt or elevate |
| Same provider tests, fresh process and reopened SQLite / live Postgres | Fail 5+ times, destroy provider/backend/process, restart at injected time: same deadline/count/F; process-local limiter reset and many new sessions never reset durable state; at cap slow guessing stays one/day; success resets n only while F increases |
| Session races / provider tests | Interleave activity read, MFA commit, delayed activity write: MFA stays true; interleave revoke and MFA/touch: no resurrection; active-user disable and secret removal with a correct code reject when ordered first; expiry between admission and transaction rejects; backoff/failed codes do not renew idle time; successful MFA does; absolute limit fixed; cache TTL cannot restore provisional MFA |
| [server_integration_test.cpp](../../tests/integration/server_integration_test.cpp), [auth_middleware_test.cpp](../../tests/unit/auth_middleware_test.cpp) | Missing/invalid bearer and PAT cannot guess/enumerate; TokenOnly registry mismatch fails INTERNAL; data bucket does not block SubmitMfa; MFA bucket refusal emits trusted-user obs; durable refusal produces RESOURCE_EXHAUSTED; storage conflict UNAVAILABLE/ABORTED never wrong-code; lost response reconciles committed success |
| Proof/lifecycle conformance and provider tests | Legacy complete session with NULL proof denied; password-only completed session followed by enrollment cannot unlock; generic writes cannot set proof/change secret; every new session has NULL proof; real SubmitMfa atomically creates generation/time proof and allows recovery; replacement/removal/same-value replacement revoke all sessions and change/clear generation while preserving F/backoff; old-generation proof denied even if revocation is bypassed in an adversarial fixture; audit/commit failures roll back secret/generation/revocations/proof together; demo/provisioning/fixture paths use guarded lifecycle |
| New UnlockMfa integration/CLI tests | Anonymous/pending/PAT/Member/LabAdmin/scoped grant denied without target disclosure; exact MfaUnlock registry/authorize agreement planted mismatches; real completed SystemAdmin succeeds cross-lab and self; revoked/expired/disabled actor or permission downgrade before lock denied; target unknown/inactive/no secret; actor revoked/permission downgraded after admission with Missing target still gets actor denial, authorized Missing target gets NOT_FOUND with no state/audit; missing actor and concurrently created target cannot bypass lock status; proven admin self/other recovery after capped wait or another admin unlock; preserved F replay rejection after unlock; repeated/no-op audit; append failure retains backoff; fixed reason validation; token never in argv/output; CLI status/TLS failures injectable |
| SQLite contention | Hold real BEGIN IMMEDIATE in another connection/process: bounded UNAVAILABLE, no mutex queue, no state/elevation/audit; release then full request retry succeeds or honors committed backoff; constructor/commit failures leave connection reusable |
| Obs/audit capture tests | Every evaluated failure and both limiter refusals call correct user-keyed event; no code/secret/token/hash/PHI substring in serialized audit/log/status even on injected errors; audit append failure never emits committed-failure/success events; refusal has zero DB mutation/audit rows; audit chain verifies across races |
| REST/web regression | Existing browser MFA pending state survives 429; wrong code differs from throttling; genuine expiry ends session; no new REST admin endpoint assumed; run existing MFA tests and npm check if web code changes |

Later implementation validation: focused `ctest --preset dev -R
'Totp|SubmitMfa|LocalAuthProvider|AuthMiddleware|BackendConformance'` (confirm actual
test names), full `ctest --preset dev`, and ASan/UBSan/TSan appropriate to touched
storage/concurrency/parser paths, excluding `grpc_integration|e2e` under ASan/TSan
as CI requires. Record PostgreSQL skips separately and obtain live PostgreSQL CI;
macOS runs do not substitute for Linux sanitizer/concurrency evidence. Run format,
SPDX, actual applicable clang-tidy, registry descriptor and any changed CLI/client
checks. This prose-only issue instead uses its explicit TDD/C++/npm waiver and
reference/diff/SPDX/docs CI validation, recorded in the handoff.

Adversarial reviewer should try to falsify these invariants: Can a stale writer
undo elevation/revocation? Can two processes consume one counter, or consume an
old coincident match? Can a thrown auth error accidentally roll back the failure
budget? Can restart/login/clock rollback/limiter eviction/unlock lower protection?
Can a credential or global permission revoked before the locked check still
unlock? Can legacy/password-only flags masquerade as TOTP proof after secret
replacement? Can mixed-isolation appenders or two genesis inserts commit a fork?
Does an audit error leave authority without a chain entry? Can refusal
traffic force unbounded audit writes, expose identities, or preserve a session
forever? Are all recovery paths real code/API work, and does #179 stay open until
they and durable guessing protection are implemented?
