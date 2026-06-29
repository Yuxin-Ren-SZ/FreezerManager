# PRD Review: Institutional IT Manager Perspective

**Reviewer persona:** IT infrastructure manager at a large research university
or teaching hospital.  Responsible for 200+ servers, 50+ research groups,
regulatory compliance (HIPAA / GDPR / local data protection), identity
management, backup/DR, and security operations.  Approves or rejects every piece
of software that touches PHI or PII on institutional infrastructure.

**Document reviewed:** `doc/PRD.md` (design baseline 2026-05-06, last synced
2026-06-15).

**Disclosure:** I am reading this because a PI just emailed me asking whether
they can deploy FreezerManager instead of renewing the $18k/year Freezerworks
license.  I need to decide whether this project is a net reduction or net
increase in my team's operational risk.

---

## Features I Like

### Data safety as the #1 design priority (§1.1)

This is the correct ordering.  Every other priority — security, extensibility,
concurrency, usability — is meaningless if the system drops rows silently.  I
have seen production databases with corrupt rows that went undetected for
months.  The fact that this PRD opens with "no silent loss; every mutation is
auditable; recoverable" tells me the author has been burned before and learned
the right lesson.

### Append-only hash-chained audit (§7.3)

This is not a checklist item; it is the only audit model that can survive a
regulatory review.  Each row carries `prev_hash` + `this_hash = SHA-256(prev ‖
canonical_json(row))`.  A nightly HMAC checkpoint signed by a KMS key outside
the database.  `freezerctl audit verify` walks the chain and reports the first
divergence.  For HIPAA technical safeguards (§164.312), for 21 CFR Part 11
audit trail requirements, for GDPR Article 30 record-keeping — this is exactly
what auditors want to see.  I can hand them the verification output and the
chain of custody is self-proving.

I particularly note that audit snapshots (`before_json`/`after_json`) are
**repository-derived, never caller-supplied** (§7.3 / Audit section).  This
closes the most common audit bypass: an attacker who compromises the application
layer cannot forge a mutation to look like it came from a legitimate internal
call, because the snapshot is produced from the authoritative entity state
inside the transaction, not from the RPC arguments.

### Separate backup KEK from master KEK (§8, §14)

This is the kind of decision that separates a system designed by someone who
understands key compromise scenarios from one designed by someone who reads
NIST SP 800-53 checklists.  Master key stolen?  Backups are still encrypted
under a different key.  Backup key leaked?  Live data is still under a
different key.  `KmsFactory::make_backup_kms()` is independent of
`make_default_kms()`.  This is correct.

The weekly restore-drill (`freezerctl backup verify` — never throws on a bad
backup, reports PASS/FAIL) is equally important.  A backup that hasn't been
test-restored recently is not a backup; it's a superstition.  That the drill
runs automatically inside `BackupScheduler` and picks a random backup each
week means I don't need to write a cron job for this.

### Pluggable storage, both backends pass the same conformance suite (§5)

SQLite for dev and small labs; PostgreSQL for production with Row-Level
Security.  Both run through the same parameterized GoogleTest conformance suite.
This means I can start a pilot lab on a single SQLite file (zero operational
overhead) and migrate to a managed PostgreSQL cluster when the deployment
graduates to production — the domain code doesn't change.  The conformance
suite is my assurance that the migration won't silently break semantics.

### PHI field-level encryption with per-record DEK envelope (§8, §5 M5)

`crypto_secretbox` per record, DEK wrapped by master KEK, `is_phi ∧ indexed`
rejected at definition time, `phi.read` as a distinct audit event kind.  This
is the correct design: PHI is never in plaintext at rest, and every access is
logged as a separate audit event with higher severity.  For HIPAA minimum
necessary standard, for GDPR data minimization — this delivers.

The `IKmsProvider` abstraction with `EnvVarKms` (dev), `OsKeyringKms`
(systemd-creds, production), and planned `VaultKms` means I can integrate with
our existing HashiCorp Vault deployment without changing the encryption layer.

### Multi-lab isolation at both application layer and database layer (§3, §5)

Per-lab RLS policies in PostgreSQL inject `app.current_lab_id` as a session
variable.  The application layer enforces the same isolation, so even a
successful SQL injection cannot bypass lab boundaries without also injecting a
PostgreSQL `SET` command — and that would be caught by query logging.  This is
defense in depth.

### Five built-in roles + custom roles with scope filters (§3)

`SystemAdmin`, `LabAdmin`, `Member`, `ReadOnly`, `ApiClient`.  Lab admins can
define custom roles with scoped permissions (e.g., "Member, but only freezers
F1 & F2").  For a deployment serving 50 labs with different risk profiles (some
have PHI, some don't), this RBAC model is granular enough without being a
full ABAC engine that nobody can configure correctly.

### `authz_version` cache invalidation (§3)

The permission cache (5-min TTL) is invalidated on both `revoke` AND on
`users.authz_version` bump.  This means a role downgrade or membership removal
takes effect at the next request, not at the next cache expiry window.  I have
seen production incidents where a terminated employee retained access for 5+
minutes because the permission cache hadn't expired.  This PRD closes that gap.

### First-run wizard (§16)

A CLI wizard that creates the system admin, the first lab, and generates the
master key.  This is what I hand to a junior sysadmin (or a PI who insists on
self-hosting) and say "follow the prompts."  No 50-page deployment guide
required.

### GFS backup retention (§14, README)

30 daily, 12 monthly, 7 yearly.  This matches our institutional retention
policy without me having to write a wrapper script.  Configurable, so I can
adjust for labs with different regulatory requirements.

### Build reproducibility & sanitizer CI (§16, README)

CMake 3.25+ with Conan 2 lockfile.  ASan + UBSan + TSan builds in CI.  This
means dependency resolution is deterministic, and memory errors, undefined
behavior, and data races are caught before merge — not discovered in production
at 3am.

### Dual license: AGPLv3 + commercial (§18)

AGPLv3 covers academic use.  The commercial license path exists for industry
partners who need different terms.  This is a cleaner model than "open source
but the good parts are proprietary."  I know exactly what I'm getting.

---

## Features Missing From the PRD That I Need

### 1. SLO/SLA definition for the server

The PRD describes what the server _does_ but not what it _promises_.  Before I
approve this for production, I need documented SLOs: RPC latency (p50/p95/p99),
availability target, backup window, recovery time objective (RTO), recovery
point objective (RPO).  Even if v1 sets aspirational targets, I need the
framework.  Without SLOs, I cannot design monitoring, cannot size hardware, and
cannot answer "what happens if it's slow?"

### 2. Prometheus metrics and OpenTelemetry tracing are deferred (§17)

The PRD lists Prometheus `/metrics` and OTel tracing under Operations (§17)
with 🔲 Planned status.  This is in the wrong milestone.  Observability is not
a polish item for v1.0 — it is a Day 0 requirement for any service I am
expected to operate.  I need: RPC latency histograms, error rate by endpoint,
audit-append latency, database connection pool utilization, backup job
status/success/failure.  Without these, the server is a black box.  If
something degrades at 3am, I learn about it when the PI emails me at 9am.

Recommendation: promote Prometheus metrics from M7 polish to M3 (gRPC server
milestone).  They can ship as a single `/metrics` endpoint with 5–10 core
gauges/histograms; more can be added later.  OTel can stay at M7.

### 3. OIDC/LDAP auth is deferred past v1.0 (§7.1, M7)

This is a blocking gap for institutional adoption.  Every research university
and hospital has a central identity provider — typically Azure AD, Shibboleth,
or FreeIPA.  I cannot, and will not, provision local accounts for every lab
member in a standalone database.  The password-reset flow alone would consume
my helpdesk.

The PRD lists `OidcAuthProvider` and `LdapAuthProvider` as planned (🔲), but
places them at M7 (Polish & 1.0) and M2 (Auth & Audit) — the milestones are
contradictory.  At minimum, OIDC must be in the first production-tagged
release.  Without it, the deployment model is "one admin provisions every user
manually," which doesn't scale past one lab.

Recommendation: make OIDC (with auto-provisioning: first login → create user +
assign default role in their lab) an M3/M4 deliverable, not M7.

### 4. TLS enforcement is underspecified (§6, §8)

The PRD states "TLS 1.3 only; HSTS; modern cipher suites only. Self-signed cert
OK for dev, not for production."  This is directionally correct but incomplete.
I need to know:
- Does the gRPC server and the REST gateway share the same TLS configuration?
- Is client certificate authentication (mTLS) supported for the machine-client
  use case?  (`MtlsAuthProvider` is 🔲 Planned — but TLS itself must be
  configurable to request/require client certs.)
- Does `freezerd` reload certificates on `SIGHUP` without dropping connections?
- What is the cipher suite list?  "Modern cipher suites only" is not specific
  enough for a security review.

### 5. No documented deployment model for high availability

The PRD assumes a single server process (§2: "Single self-hosted server").  For
a core facility that 20 labs depend on, a single point of failure is
unacceptable during business hours, let alone 24/7.  I need to know:
- Can I run two `freezerd` instances behind a load balancer?  (If the audit
  chain relies on a single-writer advisory lock, probably not.)
- What is the failover story for PostgreSQL?  (Streaming replication is
  mentioned in §14 for backup, but not as an HA strategy.)
- If the answer is "v1 is single-server by design," that's acceptable as an
  explicit limitation — but it must be stated, so I can plan accordingly.

### 6. No structured log schema

§17 says "JSON logs to stdout, journald-friendly."  I need a documented log
schema: what fields are guaranteed, what levels are used (and when), what
constitutes an ERROR vs WARNING vs INFO.  Without this, my log aggregation
pipeline (Elasticsearch / Loki / Splunk) cannot parse, index, or alert on
FreezerManager logs.  The `redact()` wrapper for PHI is good — now define the
log contract.

### 7. No integration test for the full stack in CI

§15 describes the test strategy — and it is thorough — but the "end-to-end
smoke" test ("start server in a container, run a Python client script...") is
still listed as a verification plan item (§21) rather than an active CI gate.
For institutional adoption, I need to know that every merged commit passes a
real end-to-end test: start `freezerd`, create a lab via gRPC, add samples,
export CSV, verify the audit chain, restore from backup.  Promoted to CI gate
before 1.0.

### 8. No documented upgrade/migration path between versions

The PRD has migration tests (§15: "every schema migration has an up+down test")
and a `migrate_to_latest()` method on `IStorageBackend`.  But there is no
documented procedure for upgrading a running deployment from version N to N+1:
- Is the server backward-compatible with the previous version's client?
- Does the migration block writes?
- Can I roll back a failed migration?
- Are migration durations documented (so I can schedule a maintenance window)?

### 9. No rate-limiting or DoS protection specification

§11 mentions rate limits for the public API (default 60 req/min for Member
tokens).  But what about the gRPC server?  What about authentication endpoints
(brute-force protection)?  The `LocalAuthProvider` has account lockout (§7.1),
which is good — but what prevents an unauthenticated attacker from saturating
the gRPC thread pool with connection attempts?  Rate limiting is not a Polish
item; it's a security boundary.

### 10. No data residency / geo-replication story

For GDPR compliance, some institutions require that PHI data never leaves a
specific jurisdiction.  The PRD assumes a single server, which implicitly keeps
data local — but this should be stated as a design property, not an accident of
architecture.  For institutions with multi-site deployments, is there any path
to geo-replication?  (Even if the answer is "not in scope for v1," I need to
know.)

### 11. No documented support model

I am evaluating this against a commercial vendor who provides a support
contract with a 4-hour response SLA.  As an open-source project, FreezerManager
doesn't have that — and I don't expect it to.  But the PRD should document what
support _does_ look like: GitHub Issues?  Community forum?  Is there a paid
support tier?  What is the expected response time for a security vulnerability
report (and is there a `SECURITY.md` — yes, README references it, good)?

---

## Features I Don't Like / Design Concerns

### 1. AGPLv3 is flagged by our legal review process

§18 and §20 acknowledge this risk.  I can navigate it — AGPLv3 for internal
academic deployment is legally unproblematic — but it will trigger a mandatory
legal review that adds 4–8 weeks to the approval timeline.  Some institutions
have blanket AGPL bans written by lawyers who don't distinguish between
"running AGPL software internally" (fine) and "distributing a modified AGPL
binary to customers" (the trigger condition).  A clear legal FAQ — ideally
reviewed by an attorney — would accelerate this substantially.

The CLA requirement (§18) will also raise questions in legal review: "who owns
the copyright, and what happens if the project is abandoned?"  These are
standard concerns for any single-maintainer open-source project with a CLA, but
they should be addressed in the FAQ.

### 2. The design priorities create a tension with production readiness

§1.1 ranks: Data Safety > Security > Extensibility > Concurrency > Usability.
These are correct for a domain model.  But for a _production service_, I would
rank them differently: Data Safety > Security > **Operability** > Concurrency >
Extensibility > Usability.  "Operability" means: metrics, logging, health
checks, graceful shutdown, configuration validation, backup monitoring,
migration runbooks.  The PRD defers most operability items to M7 (Polish &
1.0).  This means the server is built and functionally complete before anyone
asks "how do I operate this in production without waking up at 3am."

Recommendation: add an explicit operability milestone between M3 (gRPC+Qt) and
M4 (Web UI) that covers metrics, structured logging, a `/health` endpoint with
dependency checks, and a documented runbook.

### 3. SQLite in production with concurrent writers

§20 acknowledges "SQLite single-writer surprise in production" as a risk, with
mitigation: "Docs + first-run wizard warn if concurrent users > 5 on SQLite and
recommend Postgres."  A doc warning is not a mitigation for a production system.
SQLite's WAL mode allows concurrent readers but serializes writers.  For 5
concurrent users doing check-outs, this could manifest as intermittent
`SQLITE_BUSY` errors that are confusing to end users and hard to diagnose.

I would prefer that the first-run wizard _refuse_ to start in production mode
with SQLite if it detects no PostgreSQL URL configured, and require an explicit
`--i-understand-sqlite-is-not-for-production` flag to override.  Or, at
minimum, that the dashboard displays a persistent amber warning banner when
running on SQLite with > 3 active sessions.

### 4. Cross-lab sharing approval chain is rigid

`source_admin + target_admin + system_admin` three-signature approval for every
share.  For a formal biospecimen transfer between hospitals, this is perfect.
For two labs in the same department sharing cell lines, this is bureaucratic
overhead that will drive users to bypass the system entirely — they'll hand
over the tube and track it on a whiteboard.

The system should support at minimum a LabAdmin-configurable policy: "shares
within the same department require only source_admin approval" or "shares of
non-PHI samples require no approval, just logging."  The three-signature
default can remain as the conservative baseline.

### 5. The test count (1437) is impressive, but I need to know what's _not_ tested

Every engineer who reports a test count should also report a coverage report
with uncovered branches.  The PRD mentions `gcovr` in the README, which is
good — but the coverage percentage and uncovered modules should be part of the
verification plan (§21), not just a development tool.

Specifically, I would flag these areas for coverage review:
- Network error handling in gRPC service handlers (client disconnects,
  timeouts, partial messages)
- Database connection pool exhaustion and recovery
- Concurrent session expiry and cleanup
- Memory pressure during large CSV imports

### 6. No mention of capacity planning or resource limits

How much memory does `freezerd` use per concurrent connection?  What is the
maximum CSV import size before the server OOMs?  What is the recommended disk
I/O throughput for the audit log under peak write load?  These are sizing
questions I need to answer before I provision a VM or container.

Even rough numbers ("memory: ~50 MB baseline + ~10 MB per connection; tested
with 100 concurrent connections on a 4 GB VM") would be valuable.

### 7. Single-maintainer risk

The PRD lists one owner (Yuxin Ren) and requires a CLA from all contributors.
For an institutional deployment with a 5–10 year expected lifespan, the bus
factor of 1 is a material risk.  If the maintainer moves on, do I have the
source code?  (Yes, AGPLv3.)  Can I hire someone to maintain it?  (Yes, C++20
with standard tooling.)  Can I contribute fixes back?  (Yes, but need CLA.)

This is not a criticism of the project — every project starts with one
maintainer — but it should be stated honestly in an "Institutional Adoption
Considerations" section.  A governance model that grows to include other
maintainers over time would substantially reduce this risk.

---

## Verdict

**Conditional approval, pending resolution of four blocking items.**

This is a well-engineered project with a domain model that matches laboratory
reality and an audit architecture that would survive a regulatory review.  The
encryption design (separate backup KEK, per-record DEK envelope, `IKmsProvider`
abstraction) is correct.  The test discipline (TDD, 1437 tests, sanitizer CI,
conformance suite) is unusually rigorous for a pre-alpha project.

However, there are four gaps that block institutional adoption in its current
form:

| # | Gap | Impact | Recommended milestone |
|---|-----|--------|----------------------|
| 1 | No OIDC/OAuth2 auth provider | Cannot integrate with institutional IdP; manual account provisioning doesn't scale past one lab | M3/M4 (not M7) |
| 2 | No Prometheus metrics or health-check endpoint | Cannot monitor, cannot alert, cannot operate | M3 (gRPC server milestone) |
| 3 | No documented upgrade procedure | Cannot plan maintenance windows or assess migration risk | M3 (before first adopter deployment) |
| 4 | No rate-limiting specification for gRPC/auth endpoints | DoS surface is unquantified | M3 |

If these four items are resolved before the first production-tagged release, I
would recommend FreezerManager over renewing a commercial LIMS license for
academic research labs.  The cost savings (~$18k/year/lab) and data sovereignty
benefits outweigh the operational overhead of self-hosting a well-architected
single-server C++ application.

If they are not resolved, I recommend waiting for a subsequent release and
continuing with the commercial vendor for one more license cycle.
