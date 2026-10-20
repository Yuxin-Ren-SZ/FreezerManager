# Handoff note — 2026-10-20, demo entry point (#142, worker-3)

The demo could not be started: `scripts/demo-setup.sh` targeted the Qt client,
never started the SPA, hardcoded `127.0.0.1:8080` while `vite.config.ts`
proxied `/api` to `18080`, died at the seed step on the system `python3`
(3.9.6, no argon2), and — on a machine shared with two other agents — ran
`pkill -f freezerd` and deleted `data/freezer.db`. This slice turns it into one
command that seeds a demo database and starts `freezerd` **and** the SPA on a
matching loopback port pair, and writes the click path into
`doc/dev/web.md`. No C++, proto or schema change.

**Changed:**

- `scripts/demo-setup.sh` — rewritten:
  - exports `FMGR_REST_LISTEN` / `FMGR_WEB_DEV_PORT` for both children so the
    Vite proxy target and the server always agree; defaults 18090 / 5190 /
    50090, or the slot ports when `scripts/agent/env.sh` was sourced; refuses a
    non-loopback listener.
  - the demo DB is `out/demo/freezer.db`, never `data/freezer.db`; nothing is
    deleted without `--reset`, which logs the three files first; a busy port is
    reported with `lsof`, never killed.
  - no `pkill`; the EXIT/INT trap stops only the PIDs the script started
    (`freezerd`, Vite, and Qt only if its binary exists).
  - first-run seeding is fixed: the old check ran *after* the migration server
    had created the file, so a fresh database was never seeded. A `FRESH` flag
    is decided before migrations.
  - the seed python is the main checkout's `.venv/bin/python3` (via
    `FMGR_MAIN_CHECKOUT`, or derived from `git rev-parse --git-common-dir`), and
    the script exits 1 with the `pip install argon2-cffi` fix if argon2 is
    missing. Node 22 comes from `out/tools/node22/bin` by the same lookup.
  - the demo admin gets the fixed RFC 6238 TOTP secret `JBSWY3DPEHPK3PXP`
    (idempotently, on every start) so the demo shows the real MFA flow;
    `--totp-code` prints the current code.
  - Vite runs directly (`npm run gen` then `node_modules/.bin/vite`) so the
    recorded PID owns the port; `npm run dev` can leave an orphan behind.
- `doc/dev/web.md` — the demo runbook: port table, prerequisites, `--reset`,
  the click path (TOTP sign-in → reload keeps the session → lookup → detail →
  check-out → box view), which screens are real and which are placeholders.

**Decisions:**

- **Defaults, not slot 0's ports.** 18090/5190/50090 keep a bare `bash
  scripts/demo-setup.sh` off the lead's slot-0 ports while still being one
  memorable pair; agents pass their slot environment and get 18110/5203/50081
  for slot 3. The important property is that both sides read the same exported
  variables, not that a particular number is hardcoded.
- **Raw SQL for the TOTP secret.** Seeding runs before the server starts and the
  users table stores the base32 secret in plaintext (`LocalAuthProvider` reads
  `totp_secret_enc` directly), so the script sets it in the demo DB and fails if
  it did not update exactly one row. Routing it through an RPC would need an
  admin token before the demo has a user.
- **No unit test for a shell entry point.** This repo has no shell test harness;
  the issue's acceptance criterion is a full run, and that is what the PR
  records. `ctest` was not run because no C++ file changed.

**Tests / verification:**

- `bash scripts/demo-setup.sh --reset` (slot 3) on 2026-10-20: fresh DB seeded
  (5 users, 2 labs, 32 samples), migrations applied, `freezerd` healthy at
  `http://127.0.0.1:18110`, Vite at `http://127.0.0.1:5203`, and
  `GET http://127.0.0.1:5203/api/v1/health` → `"status":"ok"` through the proxy.
- Cookie-jar flow through the Vite proxy: `browser/login` → `mfa_required:true`;
  `browser/submit-mfa` with a `--totp-code` code → HTTP 200; a **fresh** curl
  with only the cookie `whoami` → `admin@freezer.local`; `sample/list` with
  `query=P001` → 6 samples; `sample/get` → volume/box/position/custom fields;
  `sample/checkout` → `SAMPLE_STATUS_CHECKED_OUT`; `box/get` +
  `box-type/list` + `sample/list?box_id=` → 100-position 10×10 grid with `A1`
  checked out.
- Reuse run without `--reset`: "reusing the existing demo database", seed
  skipped, TOTP re-applied, the checked-out sample preserved.
- Guards: `FMGR_REST_LISTEN=0.0.0.0:18090` → exit 1; an occupied web port →
  exit 1 with the `lsof` line; Ctrl-C stopped both children and left the ports
  free.

**Known limitations / follow-ups:**

- **Adjacent footgun, deliberately not fixed here:** `scripts/demo-clean.sh`
  still runs `pkill -f freezerd` and `rm -f data/freezer.db`. It is outside
  #142's file scope (the issue names `demo-setup.sh`), but on this shared box
  it can take down every agent's server — worth its own issue before anyone
  follows it.
- **Browser sign-in is G2.1 (#137, PR #145), not merged at verification time.**
  `/login` and `/login/mfa` still render placeholders until it lands, so the
  TOTP/reload part of the click path was verified at the HTTP layer (the same
  browser routes the SPA calls) with a cookie jar. Re-check the browser path
  once #145 is on `main`.
- **Re-verify against the final seed after #143 merges** (audit rows, PHI field,
  both box sizes). The click path above does not change; the History panel and
  the box grid's shape do.
