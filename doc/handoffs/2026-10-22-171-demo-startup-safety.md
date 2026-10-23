# #171 demo startup safety

`demo-setup.sh` now checks for an executable `freezerctl` and `lsof` before it
can reset or migrate the demo database. It passes the resolved CLI path to the
seed. After the seed and TOTP update finish, the script writes a versioned
sidecar completion marker. Marker-backed databases remain reusable through
ordinary sample edits. A markerless legacy database is marked only when the
fixed demo admin/lab IDs and expected per-lab sample coverage prove the old seed
completed; partial files are preserved with an explicit `--reset` recovery
instruction. Readiness probes
check that the process started by the script is alive both before and after
curl, and early exits point to the process log.

The stdlib-only startup harness uses real temporary SQLite databases for the
legacy, partial and marker checks. It also covers missing dependencies before
reset, occupied ports, a dead server hidden by a successful health response,
stale-marker removal on failed reset, and the normal ready path. Process and
command stubs run in temporary directories; no real server or shared database
is started.

Validation:

- `python3 scripts/test_demo_setup.py` — 10 tests passed.
- `bash -n scripts/demo-setup.sh` — passed.
- `git diff --check` — passed.

No full C++ build was run; the lead handles that check against the existing main
build. Implemented by worker-3 (Codex).
