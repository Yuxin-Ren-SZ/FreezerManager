# CI debugging

Two GitHub Actions behaviours that look like failures but are not. Neither is a
defect; both have already cost someone time, which is the only reason they are
written down. The workflow and the gates it runs are described in
`.github/workflows/build.yml` and `doc/dev/clang-tidy.md`.

## A skipped matrix job shows an unexpanded name

On a web-only or docs-only PR the C++ jobs are gated off, and GitHub displays
them in the check list as

```
${{ matrix.compiler }} ${{ matrix.preset }}
```

— the literal template, because the matrix never expanded. Nothing is wrong: it
is a job that was correctly skipped, and **skipped counts as passing**. PR #99
(#95, merged) is the example.

## `pending 0` means queued, not stuck

With several C++ PRs open at once, jobs wait for a runner. Until one starts, the
check's duration reads `0`, which can look stuck. Check the job's `started_at`
before concluding anything is wrong: PR #53 had simply been queued behind a
wave.

**Never re-push to "unstick" a queued run.** A push adds another run and
lengthens the queue for everyone.

## Local arm64 macOS ASan does not check leaks

LeakSanitizer does not run on arm64 Darwin. A passing local ASan run checks
the enabled memory diagnostics, but provides no leak evidence. For Drogon/HTTP
changes, require the Linux CI ASan leg for leak detection and the Linux TSan
leg for race detection. Record any excluded tests explicitly.

PR #168 (#44) illustrates the distinction: local ASan passed while Linux
LeakSanitizer found a c-ares allocation leaked by URL-based HTTP client
construction. A local pass must not be used to dismiss a Linux sanitizer failure.
