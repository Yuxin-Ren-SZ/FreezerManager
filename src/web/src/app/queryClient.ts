// SPDX-License-Identifier: AGPL-3.0-or-later
import { QueryClient } from '@tanstack/react-query';

/**
 * One `QueryClient` per app instance (see `AppProviders`), never a module-level
 * singleton: a singleton leaks cached API data between tests and between two
 * mounts of the app.
 *
 * G-arch 7 is the constraint that shapes the defaults: the cache holds API
 * payloads, which may contain PHI, so it is memory-only, it is dropped on
 * logout and on any 401 (G2.1 wires that), and nothing here writes to
 * `localStorage`, IndexedDB or the Cache API.
 */
export function createQueryClient(): QueryClient {
  return new QueryClient({
    defaultOptions: {
      queries: {
        // Server state is shared and changes under us; a short stale window is
        // enough to stop a screen from refetching on every focus, and the SSE
        // stream is what actually keeps a screen current.
        staleTime: 30_000,
        gcTime: 5 * 60_000,
        refetchOnWindowFocus: false,
        // Retry a transport failure twice. Once G1.2's `ApiError` lands this
        // becomes a predicate that skips UNAUTHENTICATED, PERMISSION_DENIED and
        // the other codes that waiting cannot fix.
        retry: 2,
      },
      mutations: {
        // A mutation is a write. Retrying one automatically is how a
        // double-submit happens without the user doing anything wrong.
        retry: false,
      },
    },
  });
}
