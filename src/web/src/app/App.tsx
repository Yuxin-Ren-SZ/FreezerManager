// SPDX-License-Identifier: AGPL-3.0-or-later
import { useState } from 'react';
import { RouterProvider } from 'react-router-dom';
import { AppErrorBoundary } from './ErrorBoundary';
import { AppProviders } from './providers';
import { createAppRouter } from './router';
import type { SessionLoader } from './session';

/**
 * TODO(G0.2 / #140): this becomes
 * `createWhoAmISessionLoader(() => call('auth/whoami', {}))`.
 *
 * `AuthService.WhoAmI` does not exist yet — not in `proto/fmgr/v1/auth.proto`,
 * not in `RestGateway.cc`, not in `src/api/routes.ts` — so the SPA has no way to
 * learn who is signed in, and the honest answer is "nobody". That keeps the app
 * on the real sign-in screen rather than making one up: the hardcoded session
 * G2.1 deleted invented a user called `Stub User` in a lab id of `lab-1`, which
 * is not a UUID, so every lab-scoped screen answered
 * `internal server error: invalid UUID length` and the SPA could never be
 * logged into at all.
 *
 * Everything around this line is finished and tested — the sign-in and
 * second-factor screens, the four session states, the 401-to-login path, and
 * `createWhoAmISessionLoader` in `src/app/whoamiSession.ts`, which maps the
 * response this loader will return.
 */
const loadSession: SessionLoader = () => Promise.resolve(null);

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
