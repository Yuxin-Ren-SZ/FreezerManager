# #171 demo startup safety

`demo-setup.sh` now checks for an executable `freezerctl` and `lsof` before it
can reset or migrate the demo database. It passes the resolved CLI path to the
seed. Existing databases are reused only when the demo admin, sentinel sample,
sample count and audit count show that seeding completed; incomplete files are
preserved with an explicit `--reset` recovery instruction. Readiness probes
check that the process started by the script is alive both before and after
curl, and early exits point to the process log.

The stdlib-only startup harness covers missing dependencies before reset,
incomplete database recovery, occupied ports, a dead server hidden by a
successful health response, and the normal ready path. All process and command
stubs run in temporary directories; no real server or database is started.

Validation:

- `python3 scripts/test_demo_setup.py` — 6 tests passed.
- `bash -n scripts/demo-setup.sh` — passed.
- `git diff --check` — passed.

No full C++ build was run; the lead handles that check against the existing main
build. Implemented by worker-3 (Codex).
