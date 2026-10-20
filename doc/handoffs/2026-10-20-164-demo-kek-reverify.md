# Handoff note — 2026-10-20, demo `FMGR_MASTER_KEK` + wave-A re-verify (#164, demo-verifier)

Issue **#164**: after #143's seed, `scripts/seed_demo.py` requires
`FMGR_MASTER_KEK` and encrypts the CSF `diagnosis` field under it, but
`scripts/demo-setup.sh` exported no key. A fresh run therefore died in the seed
before writing anything (and, with a key supplied by hand, the server started
with no KMS, so `admin` saw no PHI). This slice adds the key and closes the
wave-A gate left open by #159/#162.

**Changed:**

- `scripts/demo-setup.sh` — exports a **fixed, demo-only** `FMGR_MASTER_KEK`
  (base64 of 32 bytes) before the migration, seed and server steps, so the seed
  and `freezerd` always use the same key. The comment marks it demo-only and
  says a real deployment generates its own. Fixed on purpose, like the TOTP
  secret: the demo DB survives `--reset` and restarts.
- `doc/dev/web.md` — runbook: the key note (demo-only, why fixed), the now-real
  9×9/10×10 box split, the non-empty History panel, and the #137/#143 re-verify
  note replacing the two "keep honest" dates. Presenter steps unchanged.

**Verified (run, not read)** on 2026-10-20 in `.worktrees/164-demo-verify`,
`AGENT_SLOT=0 source scripts/agent/env.sh` (slot 0 → gRPC 50051, REST 18080,
SPA 5173), then the bare path with no `env.sh` (defaults 18090/5190/50090):

- `bash scripts/demo-setup.sh --reset` → migrations, seed `32 samples, 32 audit
  events`, `PHI: … 4 encrypted sample(s)`, `2 box types (9x9, 10x10) x 2 labs`,
  `TOTP enabled for admin@freezer.local`, both servers up, proxy health `ok`.
- Admin browser login → `mfa_required:true`; `whoami` before the code → 401
  `MFA_REQUIRED`; `--totp-code` → `submit-mfa` 200; a fresh `whoami` with only
  the cookie → `admin@freezer.local`, `phi.read` in the Neuro permissions.
- `sample/list?query=P001` → 6 matches; admin `sample/get` shows
  `"diagnosis":"Hydrocephalus"`; ReadOnly `zhang` and Member `li` see the same
  sample without `diagnosis` (gate OK).
- `sample/checkout` → `SAMPLE_STATUS_CHECKED_OUT`; `box/list` shows both grid
  types (9×9 = 81 positions, 10×10 = 100); `CSF Bank - Box 1` A1 is the
  checked-out sample.
- `audit/list` (Neuro) → 27 events (`insert=20 phi.read=7`); sample-scoped
  history → 5; `audit/verify` → `chain_intact=true events_verified=45`.
- SPA: `GET /` and a deep sample link → 200 `text/html` + `<div id="root">`.
- Restart without `--reset`: seed skipped, checkout preserved, and the admin
  still decrypts `diagnosis` — i.e. the fixed key is exported on every start.
- Bare default-port run: proxy health, `GET /`, TOTP login and
  `diagnosis=Hydrocephalus` all pass on 18090/5190/50090.

**Boundary (not fixed here):** an unknown `/api/*` path currently returns
Drogon's HTML 404, not JSON. That is #44's acceptance criterion (`Unknown
/api/* paths still get a JSON 404`) and it needs `src/rest` — out of this
chore's scope and overlapping worker-1's branch. The SPA's own routes are
unaffected; the JSON-404 spot check should be repeated after #44 merges.

**Checks not run:** `ctest --preset dev` (no C++ change, and three agents were
compiling — the issue asked to keep load light), `npm run check` (no
`src/web/**` change), clang-format (no C/C++ files). Run:
`bash -n scripts/demo-setup.sh` and `tools/check-spdx-headers.sh` → exit 0.

PR: `Closes #164`.
