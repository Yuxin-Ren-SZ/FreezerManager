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
 * The live-connection state is not passed here: it is derived from the real
 * streams by `app/connection.tsx` (#141), so the indicator cannot be a
 * hardcoded literal again.
 */
export function App() {
  const [router] = useState(createAppRouter);

  return (
    <AppErrorBoundary>
      <AppProviders loadSession={loadSession}>
        <RouterProvider router={router} />
      </AppProviders>
    </AppErrorBoundary>
  );
}
