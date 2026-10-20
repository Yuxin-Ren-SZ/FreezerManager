// SPDX-License-Identifier: AGPL-3.0-or-later
import { useSyncExternalStore } from 'react';

/**
 * The app-wide health of the live-update streams (G-arch 5, issue #141).
 *
 * `subscribeSse()` registers every stream it opens here, reports the state of
 * its connection, and removes it on unsubscribe. The shell's connection
 * indicator and the screens' "live updates" copy both read the aggregate, so
 * they cannot disagree about whether live updates are working — which is what a
 * hardcoded `connectionStatus` prop and a per-screen copy did before #141.
 *
 * Four states, because "not live" is not one condition:
 *
 *  - `connecting` — a stream was opened and has not come up yet.
 *  - `live` — a stream is open (one working feed is enough).
 *  - `reconnecting` — the feed dropped and the wrapper is waiting out its
 *    backoff. The state must not stay `live` while nothing is connected, and
 *    must leave this state when a retry opens.
 *  - `offline` — no stream is open, or the only ones were refused permanently
 *    (`UNAUTHENTICATED`/`PERMISSION_DENIED` are terminal in `sse.ts`).
 *
 * The state is module-level, not a React provider, because `subscribeSse()` is
 * transport code below the component tree. `useLiveConnectionState()` is the
 * React adapter; `src/app/connection.tsx` re-exports it under the app's name.
 */
export type LiveConnectionState = 'connecting' | 'live' | 'reconnecting' | 'offline';

/**
 * One entry per open `subscribeSse()` subscription, keyed by a symbol: two
 * screens can watch the same route, and the aggregate must count both.
 */
const streams = new Map<symbol, LiveConnectionState>();
const listeners = new Set<() => void>();
let aggregate: LiveConnectionState = 'offline';

function computeAggregate(): LiveConnectionState {
  const states = [...streams.values()];
  // "Live" wins: one working feed is enough for the shell to say live updates
  // are on. A pending retry is the next most informative state, then a stream
  // that has not opened yet.
  if (states.includes('live')) return 'live';
  if (states.includes('reconnecting')) return 'reconnecting';
  if (states.includes('connecting')) return 'connecting';
  return 'offline';
}

function publish(): void {
  const next = computeAggregate();
  if (next === aggregate) return;
  aggregate = next;
  for (const listener of [...listeners]) {
    listener();
  }
}

/** Start tracking one stream; returns the handle `sse.ts` reports against. */
export function beginLiveStream(): symbol {
  const id = Symbol('live-stream');
  streams.set(id, 'connecting');
  publish();
  return id;
}

/** Move one stream's state, e.g. to `live` on open or `reconnecting` on a drop. */
export function setLiveStreamState(id: symbol, state: LiveConnectionState): void {
  if (!streams.has(id)) return;
  streams.set(id, state);
  publish();
}

/** Stop tracking one stream: its subscription was cleaned up. */
export function endLiveStream(id: symbol): void {
  streams.delete(id);
  publish();
}

/**
 * Test-only: forget every tracked stream.
 *
 * Module state outlives a test file's `beforeEach`; the older `subscribeSse`
 * tests open a stream and never unsubscribe, so without this the next test
 * starts from `live`. Not exported through the app wrapper, so production code
 * has only one way to reach the store.
 */
export function resetLiveConnectionForTests(): void {
  streams.clear();
  publish();
}

/** The aggregate state; the `useSyncExternalStore` snapshot. */
export function getLiveConnectionState(): LiveConnectionState {
  return aggregate;
}

/** Subscribe to aggregate changes; returns the unsubscribe function. */
export function subscribeLiveConnection(listener: () => void): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

/** The aggregate state as a React value. */
export function useLiveConnectionState(): LiveConnectionState {
  return useSyncExternalStore(
    subscribeLiveConnection,
    getLiveConnectionState,
    getLiveConnectionState,
  );
}
