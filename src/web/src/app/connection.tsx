// SPDX-License-Identifier: AGPL-3.0-or-later
import { useLiveConnectionState, type LiveConnectionState } from '../api/liveConnection';

/**
 * Health of the live-update streams (G-arch 5: `EventSource` on the `…/watch`
 * SSE routes).
 *
 * The state is **derived from the streams themselves** — `src/api/sse.ts`
 * reports every subscription's connection into the store behind
 * `useLiveConnectionState()` — not passed down as a prop. The shell's
 * indicator and the screens' "live updates" copy read the same value, so they
 * cannot disagree; before #141 `App.tsx` passed the literal `"offline"`, and
 * the top bar read Offline while streams were delivering frames.
 *
 * There are four states because "not live" is not one condition: a stream that
 * has not opened yet is `connecting`, a dropped one the wrapper is about to
 * retry is `reconnecting`, and only a terminal refusal or no stream at all is
 * `offline`. The reconnect/backoff machinery stays in `src/api/sse.ts`; this
 * module is the React-facing name for its aggregate state.
 */
export type ConnectionState = LiveConnectionState;

/** The aggregate live-connection state, as a React value. */
export function useConnectionState(): ConnectionState {
  return useLiveConnectionState();
}
