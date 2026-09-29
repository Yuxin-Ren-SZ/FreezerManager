// SPDX-License-Identifier: AGPL-3.0-or-later
import { createContext, useContext } from 'react';
import type { ReactNode } from 'react';

/**
 * Health of the live-update stream (G-arch 5: `EventSource` on the `…/watch`
 * SSE routes).
 *
 * The shell only needs the three states — a spinner, a green "live" dot and a
 * grey "offline" dot — so that is the whole interface. G1.2's `src/api/sse.ts`
 * knows about reconnects, `Last-Event-ID` and backoff; none of that belongs in
 * a top-bar indicator.
 */
export type ConnectionState = 'connecting' | 'live' | 'offline';

const ConnectionContext = createContext<ConnectionState>('offline');

export interface ConnectionProviderProps {
  status: ConnectionState;
  children: ReactNode;
}

export function ConnectionProvider({ status, children }: ConnectionProviderProps) {
  return <ConnectionContext.Provider value={status}>{children}</ConnectionContext.Provider>;
}

export function useConnectionState(): ConnectionState {
  return useContext(ConnectionContext);
}
