// SPDX-License-Identifier: AGPL-3.0-or-later
import { useState } from 'react';
import { RouterProvider } from 'react-router-dom';
import { AppErrorBoundary } from './ErrorBoundary';
import { AppProviders } from './providers';
import { createAppRouter } from './router';
import { stubSessionLoader } from './stubSession';

/**
 * The application root: providers outside the router, so a route error still has
 * the toast host and the session it needs to explain itself.
 *
 * TODO(G2.1): two stubs are wired here and nowhere else, and neither is
 * finished by the G1.2 merge — `src/api/sse.ts` and the `auth/whoami` client
 * exist now, but nothing calls them from the shell yet.
 *   - `stubSessionLoader` (see `stubSession.ts`) becomes the `auth/whoami`
 *     call; the RPC itself is G0.2.
 *   - `connectionStatus` becomes the state of the SSE wrapper in
 *     `src/api/sse.ts`. It is `'offline'` rather than `'live'` today because
 *     nothing has opened a stream, and an indicator that always says "Live" is
 *     worse than no indicator at all.
 */
export function App() {
  const [router] = useState(createAppRouter);

  return (
    <AppErrorBoundary>
      <AppProviders loadSession={stubSessionLoader} connectionStatus="offline">
        <RouterProvider router={router} />
      </AppProviders>
    </AppErrorBoundary>
  );
}
