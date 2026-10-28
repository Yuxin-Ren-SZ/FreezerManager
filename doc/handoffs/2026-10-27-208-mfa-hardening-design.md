# #208: MFA replay/backoff/unlock design handoff

- Worker: worker-3 (Codex), slot 3.
- PR: [#212](https://github.com/Yuxin-Ren-SZ/FreezerManager/pull/212), closes only
  [#208](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/208).
- Source base: `d527c0c312554d5485c020355e6bd5c6891eb9a4`.
- Deliverable: [proposed ADR0002](../adr/0002-mfa-replay-backoff.md).
- Status: proposed design; independent adversarial review and lead acceptance
  remain required. No runtime implementation or deployment approval is claimed.

## Decisions for implementation review

Consume the greatest matching TOTP step once per user, reject any match at/below
its durable floor, and commit consumption, one-session MFA elevation and audit
together. Completed sessions remain rejected. Introduce an auth transaction mode
that locks before reads: SQLite BEGIN IMMEDIATE; PostgreSQL READ COMMITTED with
sorted users-row locks. Existing ordinary transaction APIs do not provide this.
All audit appenders additionally require predecessor uniqueness, including genesis,
and targeted transient conflict handling across isolation modes.

Persist per-user failures, exponentially backing off from failure 5 (30 seconds)
to failure 17 (24 hours, thereafter one evaluated guess/day); no inactivity,
restart or login reset. Commit failure state with an audit row before returning
wrong-code. Admin unlock resets guessing state with audit, retains the replay
floor, and requires a new global auth.mfa_unlock permission plus an existing
completed real TOTP admin session with durable proof for the current enrollment
generation. Legacy/password-only mfa_complete flags are insufficient. Additive RPC and authenticated gRPC CLI work
are identified explicitly. Read-only limiter refusals emit user-keyed obs without
new audit writes; failed-state mutations always audit.

The stale full-row activity/session overwrite is a source finding tracked as
[#211](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/211), with deterministic
runtime reproduction pending. Coordinate that prerequisite before implementing
MFA atomicity; this docs slice neither reproduces nor fixes it.

## Response to independent review R1–R3

- R1: the common PostgreSQL append loop targets the named nondeferrable
  audit_events_prev_hash_unique constraint via ON CONFLICT ... DO NOTHING
  RETURNING id; zero returned rows becomes SerializationFailure, as do native
  40001/40P01. Other uniqueness errors retain their mappings. Full domain/audit
  rollback, fresh whole-RPC retries, genesis coverage, atomic fail-closed chain
  preflight and deterministic mixed-isolation tests are specified. Advisory lock
  alone is explicitly insufficient. Source reasoning and PostgreSQL 16 primary
  documentation were checked; no runtime reproduction is claimed.
- R2: new private nullable session generation/time proof can only be assigned by
  the atomic TOTP consumption/elevation operation; legacy/new sessions start
  without proof. Unlock checks the current locked enrollment generation. A guarded
  secret lifecycle rotates/clears generation, revokes all sessions and preserves
  replay/backoff, including same-value replacement. Generic User/Session writes,
  demo/provisioning and cutover paths must honor the contract. Actual fresh TOTP
  admin recovery and password-only/legacy/old-generation denials are tested in
  the future matrix. This is added implementation scope, not an existing feature.
- R3: the migration creates an empty security-state table; old and new users
  initialize lazily under the same lock with one separate transactional audit
  event. No unaudited migration backfill remains. State cannot be deleted/reset
  to replenish guesses. Lock status records Missing without throwing target
  errors: locked actor credential/permission/proof recheck wins before an
  authorized caller may observe target NOT_FOUND.

The ADR remains Proposed pending re-review. #179 and #211 remain runtime work.

## Validation and scope limits

Only this handoff and ADR0002 are changed. No source, test, schema, proto,
dependency, CI, TODO, README, board body or owner architecture edits. No migration
version or resource lock is reserved; version 15 remains free.
[#179](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/179) remains open and
continues to gate non-loopback deployment. The ADR test matrix is planned
regression work, not passing runtime evidence.

The explicit prose-only waiver in #208 covers TDD, C++/sanitizer/full-CTest and npm
reruns for this issue. None were run or claimed. Applicable PR CI is reported on
PR #212; a skipped C++/clang-tidy job is not security/concurrency coverage. The
worktree helper configured dev with Conan build=never but no build/test followed.

Local validation commands (run from this worktree with
`AGENT_SLOT=3 source scripts/agent/env.sh`):

```sh
python3 - <<'PY'
import re
from pathlib import Path
paths = [Path('doc/adr/0002-mfa-replay-backoff.md'),
         Path('doc/handoffs/2026-10-27-208-mfa-hardening-design.md')]
count = 0
for path in paths:
    for target in re.findall(r'\[[^\]]*\]\(([^)]+)\)', path.read_text()):
        if re.match(r'[a-z]+://', target):
            continue
        assert (path.parent / target.split('#', 1)[0]).exists(), target
        count += 1
print(f'Local Markdown targets: {count} checked, 0 missing')
checks = {
    'src/auth/Totp.cc': ['totp_verify', 'base_counter'],
    'src/auth/LocalAuthProvider.cc': ['void LocalAuthProvider::verify_totp', 'check_session_expiry', 'update_last_seen_if_needed', 'lookup_or_build_context'],
    'src/storage/IStorageBackend.h': ['class ITransaction', 'note_mutation', 'class IStorageBackend'],
    'src/storage/sqlite/SqliteBackend.cc': ['void SqliteTransaction::commit()', 'BEGIN IMMEDIATE', 'void SqliteTransaction::rollback()'],
    'src/storage/sqlite/SessionRepositories.cc': ['stage_update', 'update_pending', 'mfa_complete = ? WHERE id = ?'],
    'src/storage/postgres/PostgresBackend.cc': ['IsolationLevel::Serializable', 'pg_advisory_xact_lock', 'throw_pqxx_error', 'this_hash        TEXT   NOT NULL UNIQUE'],
    'src/storage/postgres/SessionRepositories.cc': ['write_update', 'mfa_complete = $10'],
    'src/rpc/AuthMiddleware.cc': ['authorize_impl', 'authenticate_impl', 'TokenOnly'],
    'src/core/permissions.h': ['is_global_only_permission', 'builtin_role_permissions'],
    'src/server/AuthServiceImpl.cc': ['SubmitMfa', 'token_only()'],
    'src/obs/Log.h': ['log_event', 'LogFields'],
    'src/audit/AuditChainVerifier.cc': ['verify_audit_chain', 'multiple rows share a prev_hash (fork)'],
    'src/core/session.h': ['bool mfa_complete{true}'],
    'scripts/demo-setup.sh': ['UPDATE users SET totp_secret_enc = ?'],
}
for path, anchors in checks.items():
    data = Path(path).read_text()
    for anchor in anchors:
        assert anchor in data, (path, anchor)
print(f'Source-reference anchors: {sum(map(len, checks.values()))} checked across {len(checks)} files, 0 missing')
PY
git diff --cached --check
git diff origin/main --check
bash tools/check-spdx-headers.sh
git diff origin/main --name-only
```

Results after R1–R3 revision: 35 local Markdown targets resolved, 0 missing;
34 source anchors in 14 files resolved, 0 missing; staged diff whitespace check and SPDX check passed;
changed-path check contains exactly the two approved Markdown files. Final rebase
uses `git fetch origin main` then `git rebase origin/main`; no main checkout edit.

## Review questions and accepted limitations

The ADR gives concrete defaults and APIs rather than leaving a backoff or recovery
choice unmade. Review should challenge storage locking/rollback, coincident-code
semantics, stale session writers, failure-commit-before-throw, audit allowlists,
current admin authority and cross-process tests. The one-day cap deliberately
permits eventual recovery instead of permanent account lockout. A password holder
can still spend a user's budget to deny MFA; the admin path is attributable and
must not erase consumed steps. Obs is best effort; committed guessing-state
audit is durable. Secret lifecycle/provenance enforcement is required dependency
work; existing encryption, clock administration and disaster restore retain their
stated limits. No runtime repair or test is claimed. Matching-head CI is left to
the lead after push; earlier-head green CI is not attributed to the revised head.
