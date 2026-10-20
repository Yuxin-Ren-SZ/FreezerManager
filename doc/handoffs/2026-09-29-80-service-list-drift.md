# Handoff note — 2026-10-02, one served-service list instead of two (#80, worker-3)

`FreezerServer` kept the gRPC services it serves in **two** hand-maintained parallel
lists: `k_served_service_full_names` (a `constexpr` array of full names, the set
`served_rpc_names()` enumerated for #74's registry-coverage check) and, in
`build()`, an array of `{full_name, impl}` rows declared with
`k_served_service_full_names.size()` (the set actually registered with gRPC). The
only link between them was that size, and the comment claimed it was enough
("adding a service without listing it here does not compile"). It was not. Both
halves are now one list, so the set that is registered and the set the coverage
check inspects cannot drift.

**Measured, on the pre-change code** (plants made in a scratch tree and reverted;
the numbers are from this machine, not from reasoning about the code):

| edit | pre-change behaviour |
|---|---|
| an extra row, no matching name | `error: excess elements in array initializer` — caught, as claimed |
| a row left out | **compiles**, `cmake --build --preset dev` exit 0 |
| …then at runtime | the missing row is value-initialized to `{full_name: "", impl: nullptr}`, `build()` passes `nullptr` to `RegisterService`, and the process dies: `EXC_BAD_ACCESS` at address `0x10`, dereferencing a null `grpc::Service`'s method vector (lldb, `ServerIntegrationTest.ListSessions`) |
| a row whose `full_name` disagrees with its `impl` | invisible — the row's `full_name` was never read |

So the guard was one-directional, and the direction it missed did not surface per
RPC as #80's description assumed: it crashed the server inside `build()`, with
nothing naming the cause. CI saw it as a bare `SEGFAULT` in `SetUp`. The issue
comment with the correction is
[#80 (STATUS)](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/80#issuecomment-5888287216).

**Changed:** `src/server/FreezerServer.h` — the nine `k_*_service` name constants
and `k_served_service_full_names` are gone; `served_service_full_names()` is gone
(nothing called it — grep found only its declaration and definition);
`served_services()` is the single list, returning
`std::array<ServedService, k_served_service_count>` of
`ServedService { std::string_view full_name; grpc::Service& impl; }`;
`served_rpc_names()` is now a member function. `src/server/FreezerServer.cc` —
`served_services()` builds its rows from
`fmgr::v1::XService::service_full_name()` (the generated proto class's own
constant) and the implementation members, `build()` registers `&row.impl` for
every row, `served_rpc_names()` enumerates the same rows through the generated
descriptors, and the nine generated `<fmgr/v1/*.grpc.pb.h>` headers are included
explicitly now that the file names those types.
`tests/integration/server_integration_test.cpp` — the `RpcRegistryHoldsExactlyTheServedRpcs`
call site becomes `server_->served_rpc_names()` (the served set is the instance's
implementations), and its comment records where the set now comes from. No test
file added; see Decisions.

**Decisions:**

- **Delete the name array, do not unify the two lists around it.** #74's
  `served_rpc_names()` did *not* make either array redundant — it consumed the name
  array. What makes it redundant is `XService::service_full_name()`, a
  `constexpr` constant the generated gRPC class already declares for every
  service, which is also what the descriptor lookup needs. So the names are no
  longer written by hand anywhere, and the row list (which has to exist, because
  implementations need an instance) became the only list. This is the outcome the
  issue's ASSIGN comment preferred over unifying the two.
- **`impl` is a reference, and that is the enforcement mechanism.** A row left out
  of a `std::array` aggregate is value-initialized, which used to mean a null
  implementation; a reference member cannot be value-initialized, so a count that
  disagrees with the rows fails the build in both directions. `k_served_service_count`
  is the one number to keep in step, and keeping it wrong does not build.
- **`served_rpc_names()` became non-static rather than staying a static
  enumerator of a static list.** The served set *is* the server's own
  implementations, and reaching them needs the instance; keeping a static name
  list alive just to preserve the old signature would have kept the second list.
  The integration fixture already holds a built server, so the call site changed
  by one character.
- **No new committed test.** This is a structural defect: the pre-change code had
  no wrong *behaviour* to write a red test against, only a missing check, so the
  red step is the planted-edit transcript below — a compile that should not
  compile, and a segfault that becomes a named failure. A test cannot distinguish
  the two designs, because the property is "there is no second list".
  `RpcRegistryHoldsExactlyTheServedRpcs` remains the enforcement point for the
  other direction (an implementation constructed, so its RPCs are registered, but
  not listed) and kept its meaning.
- **The startup coverage check is untouched.** `build()` still calls
  `verify_registry_covers(served_rpc_names())` with the same semantics; only the
  source of the names changed (#74's fail-closed behaviour is what makes a service
  added *properly* to the list but without `register_rpc` calls refuse to start
  instead of failing per RPC — and that now covers a new service, because a new
  service can no longer be registered without being enumerated).

**Tests:** `tests/integration/server_integration_test.cpp` (call site and comment
only). Planted-edit evidence, post-change, each reverted:

```
# keep the count, drop a row
$ cmake --build --preset dev
src/server/FreezerServer.cc:95:5: error: reference member of type 'grpc::Service &' uninitialized
# exit 1

# add a row for an implementation that does not exist (count bumped to match)
$ cmake --build --preset dev
src/server/FreezerServer.cc:96:20: error: no member named 'GhostService' in namespace 'fmgr::v1'
src/server/FreezerServer.cc:96:55: error: use of undeclared identifier 'ghost_svc_'
# exit 1

# drop a served service AND adjust the count: compiles, then names itself
$ cmake --build --preset dev                # exit 0
$ ctest --preset dev -R 'ServerIntegrationTest.RpcRegistryHoldsExactlyTheServedRpcs'
***Failed
registered RPC(s) the server does not serve: /fmgr.v1.SessionService/ListSessions /fmgr.v1.SessionService/RevokeSession
0% tests passed, 1 tests failed out of 1
```

The third is the edit that used to segfault with no message; it is now a named
test failure. Green state, with the tree clean:

```
cmake --build --preset dev                              # exit 0
ctest --preset dev -R 'ServerIntegrationTest'           # 100% tests passed out of 11
ctest --preset dev                                      # 100% tests passed out of 1464, exit 0,
                                                        #   222 skipped (Postgres; FMGR_TEST_POSTGRES_URL unset)
clang-format --dry-run --Werror src/server/FreezerServer.cc src/server/FreezerServer.h \
    tests/integration/server_integration_test.cpp       # exit 0 (clang-format 17.0.6)
tools/check-spdx-headers.sh                             # exit 0
```

`clang-tidy` could **not** be used as a verdict locally, and the PR says so rather
than implying otherwise. `clang-tidy` 17.0.1 from the shared `.venv` cannot parse
AppleClang's libc++ (`__builtin_clzg`, `__GCC_DESTRUCTIVE_SIZE` → 27
`clang-diagnostic-error`s, "too many errors emitted"), and the checks that still
run on the failed parse emit cascading nonsense — on this file,
`readability-convert-member-functions-to-static` for `wait()`, `shutdown()` and
`in_process_channel()`, which are unchanged and which `main`'s green CI passes. An
A/B against `origin/main`'s version of the same file produced the same artifact
classes (9 × `convert-member-functions-to-static`, 1 ×
`make-member-function-const`, 1 × `unnecessary-value-param`), so the run
discriminates nothing. The one finding on a line this PR wrote
(`served_rpc_names` can be made static) was disproved as an artifact on a clean
TU: a minimal repro of the same shape — a member function whose body only calls
another member function that does use `this` — is *not* flagged, while a
genuinely static-able sibling in the same file is. The CI `clang-tidy` job
(Clang 17 against Ubuntu's libstdc++, its own cached job) is the verdict.

**Known limitations / follow-ups:**

- **The proto is not pinned to the served set.** `served_services()` says what the
  server serves; `proto/fmgr/v1/` says what the API declares. Nothing compares
  them, so a proto service with no implementation is silently unserved
  (UNIMPLEMENTED to a client) unless a test happens to call it. That is a
  different pair from the two lists #80 names, so no invariant for it was added
  here; it is raised as a QUESTION on #80 for the lead to file or drop.
- `served_rpc_names()` still throws `std::logic_error` when a served service has
  no generated descriptor. With the names coming from the generated classes that
  branch is unreachable; it is kept as a defensive check rather than turned into
  an assert, because the throw is the fail-closed path if the two ever disagree.
- A new service still takes two edits — its row, and `k_served_service_count` —
  but both are compiler-checked: forgetting the count is `excess elements in array
  initializer`, adding the count without the row is `reference member of type
  'grpc::Service &' uninitialized`. Neither message names the count, so the
  comment on `k_served_service_count` is what an author has to read.
- The integration suite's "the server serves exactly what it enumerates" property
  is still asserted through the permission registry, not by calling the RPCs, so a
  service registered with gRPC but missing from the list would be caught by
  `RpcRegistryHoldsExactlyTheServedRpcs` only because its implementation also
  registered its RPCs. That is the ordinary case; an implementation that
  registers nothing would be caught by #74's startup check instead, since it is
  now always enumerated.
