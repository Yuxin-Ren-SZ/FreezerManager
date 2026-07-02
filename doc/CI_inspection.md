# CI Inspection — main failed, dev passed (2026-07-02)

## Summary

The `Build` workflow on **main** (run
[28570365740](https://github.com/Yuxin-Ren-SZ/FreezerManager/actions/runs/28570365740),
commit `b386406`, "Merge pull request #23 from Yuxin-Ren-SZ/dev") **failed**,
while the same content on **dev** (run 28567821988, commit `e018c9c`) **passed**
one hour earlier.

**No source commit difference caused the failure.** `git diff origin/main
origin/dev` is empty — the merge commit `b386406` on main has a byte-identical
tree to `e018c9c` on dev. The failure was a **transient external dependency
download error**, not a regression.

## Failed jobs

| Job | Result |
|-----|--------|
| clang-17 tsan / asan / ubsan / dev | failure |
| gcc-13 dev | failure |
| clang-17 release, gcc-13 release | cancelled (concurrency / fail-fast) |

## Root cause

During the Conan dependency build step, the transitive dependency
`util-linux-libuuid/2.39.2` (pulled in via `libpqxx` → `libpq` → libuuid) failed
to fetch its source tarball:

```
ERROR: util-linux-libuuid/2.39.2: Error in source() method, line 81
	get(self, **self.conan_data["sources"][self.version], strip_root=True)
	NotFoundException: Not found: https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.39/util-linux-2.39.2.tar.xz
##[error]Process completed with exit code 1.
```

The kernel.org mirror returned **404** for `util-linux-2.39.2.tar.xz` at
~06:34 UTC. This is upstream mirror flakiness, unrelated to any code change.

## Why dev passed but main did not

- dev's passing run (05:32 UTC) ran an hour before the mirror hiccup and/or hit a
  warm Conan cache (`~/.conan2` restored via `actions/cache`), so it never
  re-downloaded the tarball.
- main's run (06:32 UTC) had a cache miss for that package on a fresh runner and
  re-fetched the source exactly during the mirror's 404 window.

## Verification

As of 2026-07-02 16:38 UTC the tarball is reachable again:

```
$ curl -sI https://mirrors.edge.kernel.org/pub/linux/utils/util-linux/v2.39/util-linux-2.39.2.tar.xz
HTTP/2 200
content-length: 8362220
```

Version `2.39.2` is present in the mirror's `v2.39/` directory listing alongside
2.39.1/.3/.4.

## Fix

No code fix required — the failure is transient infra. **Re-running the failed
jobs** clears it now that the mirror is back. This was done for run
28570365740.

### Hardening options (optional, not applied)

If this class of flake recurs, consider:
- Adding a persistent Conan **cache warm/restore** keyed so main reuses dev's
  downloaded sources, reducing fresh re-downloads.
- Configuring a Conan **remote/backup source mirror** (e.g. an internal artifact
  cache) so a single upstream mirror 404 does not fail CI.
