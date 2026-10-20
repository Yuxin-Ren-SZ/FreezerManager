# Handoff note — 2026-10-20, web live-connection indicator (#141, worker-1)

`App.tsx` passed `connectionStatus="offline"` to the providers, so the top bar
read **Offline** in every state while the audit streamed real frames, and the
Box and Samples screens each apologised for a feature that was working
(`Live updates are unavailable`). This slice makes the live-connection state
real: `subscribeSse()` reports every stream into a small store, and the shell
indicator plus both screens read that one value. PR **#163**.

**Changed:**

- `src/api/liveConnection.ts` (new) — the module store and
  `useLiveConnectionState()`. `LiveConnectionState = 'connecting' | 'live' |
  'reconnecting' | 'offline'`. `subscribeSse()` registers a stream handle
  (`beginLiveStream`), reports transitions (`setLiveStreamState`) and removes it
  on cleanup (`endLiveStream`). Concurrent streams aggregate: any live → live,
  else any reconnecting → reconnecting, else any connecting → connecting, else
  offline.
- `src/api/sse.ts` — opens the handle, reports `live` on `open`, `reconnecting`
  after a transient transport/`event: error` that schedules the retry, and
  `offline` on a terminal code (`UNAUTHENTICATED`/`PERMISSION_DENIED`); it
  removes the handle on unsubscribe. A malformed frame is an `ApiError` but does
  not change the state — the stream is still up.
- `src/app/connection.tsx` — now a React adapter over the store
  (`useConnectionState`), no context/provider. The `connectionStatus` prop is
  gone from `AppProviders`, `App` and `renderApp`.
- `src/app/shell/ConnectionIndicator.tsx` — four states; `Reconnecting` added
  with a warning tone.
- `src/api/hooks/sampleLive.ts` — reports the shared state as `status` (same
  type name, four members) and keeps only the last `error`; it no longer owns a
  local status.
- `src/features/box/BoxScreen.tsx`, `src/features/samples/SampleBrowserScreen.tsx`
  — the live label maps the shared state, including `live.reconnecting`;
  `locales/en/{shell,box,samples}.json` gained the `reconnecting` strings.

**Decisions:**

- **The store lives in the API layer, not in a React provider.** `subscribeSse`
  is transport code below the component tree; a provider would only be a prop
  with extra steps, which is how the literal survived. `app/connection.tsx` is
  the app's name for the same value, and the screens and the shell both use it.
- **Four states, and `reconnecting` is not `offline`.** A drop with a pending
  retry is "we are working on it"; only a terminal refusal or no open stream is
  "unavailable". `open` after a retry writes `live` again — that is the line
  that keeps the indicator from getting stuck.
- **A malformed frame does not drop the state.** The stream is still open; the
  parse failure goes to `error` for the screen, but `status` stays `live`. The
  old code set an `error` status there, which made the copy say live updates
  were unavailable while the feed was running.

**Tests:**

- Red first: **5 files failed, 7 tests failed** (plus `sse.test.ts` failed to
  load the not-yet-existing `liveConnection` module): the four-state
  transitions, the App indicator, the `sampleLive` status expectations and both
  screens' copy tests.
- Green after the fix, focused run over the five files: **153 passed (153)**.
  The behavior is pinned by:
  - `src/api/sse.test.ts` — connecting → live → reconnecting → live on the
    retry; offline on a terminal code with no reconnecting after it; notifies
    subscribers; aggregates two streams (one live wins; one dropped plus one
    connecting reads reconnecting); offline after the last unsubscribe.
  - `src/app/App.test.tsx` — the shell indicator through the real Samples
    stream: Offline with no stream open, Connecting, Live, Reconnecting after a
    transport drop, Live again after the retry opens, Offline after
    `PERMISSION_DENIED`.
  - `src/api/hooks/sampleLive.test.tsx` — the shared status through the same
    transitions; a malformed frame keeps `live` while recording the error.
  - `src/features/box/BoxScreen.test.tsx`,
    `src/features/samples/SampleBrowserScreen.test.tsx` — both screens' copy
    follows connecting/live/reconnecting/unavailable.
- Full check on the final tree, Node **v22.23.3** from the repo toolchain,
  `NODE_ENV=production` exported and unset — both exit 0, identical output:
  **50 files / 802 tests passed**, `check-routes: 74 unary + 2 SSE routes`,
  eslint 0 errors (1 pre-existing `src/ui/Table.tsx` warning),
  `prettier --check` clean, initial JS **203.0 KiB gzipped** (budget 250).
- One existing test changed timing:
  `SampleBrowserScreen > live updates > merges a new sample…` now `await
  waitFor`s for the row. TanStack Query batches observer notifications with
  `setTimeout(0)` by design; the old hook's local `status` state happened to
  force a synchronous re-render that read the new cache. Removing that state
  exposed the real timing, and the assertion now waits for the DOM the user
  actually sees. No behavior change.

**Known limitations / follow-ups:**

- With no watch open (Home, Lookup, …) the indicator reads `offline`. That is
  truthful ("live updates: offline") and is one of the four states the issue
  asked for; a separate "not in use" state would be a fifth, deliberately not
  invented here.
- The store is module-level, so `resetLiveConnectionForTests()` exists: the
  older `subscribeSse` unit tests open streams without unsubscribing, and
  module state outlives a test's `beforeEach`.
- No `TODO.md` or board edits.
