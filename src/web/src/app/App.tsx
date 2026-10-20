// SPDX-License-Identifier: AGPL-3.0-or-later
import { useState } from 'react';
import { RouterProvider } from 'react-router-dom';
import { call } from '../api/client';
import { AppErrorBoundary } from './ErrorBoundary';
import { AppProviders } from './providers';
import { createAppRouter } from './router';
import { createWhoAmISessionLoader } from './whoamiSession';

/**
 * The real session loader (G2.1): `AuthService.WhoAmI` (#140) names the caller
 * from the `HttpOnly` session cookie, so a signed-in user who reloads stays
 * signed in. `createWhoAmISessionLoader` keeps the three answers apart — a
 * user, no session (`UNAUTHENTICATED` → `null` → the sign-in form) and a
 * pending second factor (`MFA_REQUIRED` → `MfaPendingError` → `/login/mfa`).
 */
const loadSession = createWhoAmISessionLoader(() => call('auth/whoami', {}));

/**
 * The application root: providers outside the router, so a route error still has
 * the toast host and the session it needs to explain itself.
 *
 * TODO(G2.1): `connectionStatus` becomes the state of the SSE wrapper in
 * `src/api/sse.ts`. It is `'offline'` rather than `'live'` today because nothing
 * has opened a stream, and an indicator that always says "Live" is worse than no
 * indicator at all.
 */
export function App() {
  const [router] = useState(createAppRouter);

  return (
    <AppErrorBoundary>
      <AppProviders loadSession={loadSession} connectionStatus="offline">
        <RouterProvider router={router} />
      </AppProviders>
    </AppErrorBoundary>
  );
}
