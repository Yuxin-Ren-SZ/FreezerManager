# Browser-storage notification documentation (#187)

The merged #169 cross-tab fix writes an opaque nonce to
`fmgr-session-change-v1` so a peer without BroadcastChannel can still discard
its stale session and cached data. TODO G-arch 7/G5.1 and the web runbook now
name that exception to the UI-preferences-only wording. Credentials,
user/session identities and API payloads remain prohibited in browser storage.

No runtime behavior changes. Compared the wording against `sessionChange.ts`
and its mixed-transport/deduplication tests; `git diff --check` passes. The
merged implementation's combined web check passed 52 files / 825 tests.

Implemented by lead (Codex), with AI assistance.
