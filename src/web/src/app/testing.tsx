// SPDX-License-Identifier: AGPL-3.0-or-later
import { render } from '@testing-library/react';
import { createMemoryRouter, RouterProvider } from 'react-router-dom';
import { AppProviders } from './providers';
import { createAppRoutes } from './router';
import { MfaPendingError, type CurrentUser, type SessionLoader } from './session';
import type { ConnectionState } from './connection';

export interface RenderAppOptions {
  /** Starting URL; defaults to the dashboard. */
  path?: string;
  /** The session `auth/whoami` would return; `null` means signed out. */
  user?: CurrentUser | null;
  /** Make the session loader reject instead of resolving. */
  sessionError?: Error;
  /**
   * Make the loader answer the way `auth/whoami` does for a session whose
   * second factor is still outstanding: the `UNAUTHENTICATED` refusal that is a
   * resumable state, not a sign-out.
   */
  mfaPending?: boolean;
  /**
   * The loader itself, for a test that needs it to answer differently over
   * time — a sign-in or an MFA submission is exactly that, since the session it
   * establishes is what the *next* `auth/whoami` reports.
   */
  loadSession?: SessionLoader;
  connection?: ConnectionState;
}

/**
 * Renders the real app — providers and the real route table — into a memory
 * router, so a test can assert what a user at a given URL would see.
 *
 * `src/test/render.tsx` (G1.2) and this file are not duplicates: that one
 * mounts a single component in the provider stack, which is what a component
 * test wants. This one boots the whole application — the real route table, the
 * real providers, a real navigation history — which is the only way to assert
 * "a user at this URL sees this screen". It lives beside the router because it
 * is the router's test entry point.
 */
export function renderApp(options: RenderAppOptions = {}) {
  const {
    path = '/',
    user = null,
    sessionError,
    mfaPending = false,
    loadSession: loadSessionOverride,
    connection = 'live',
  } = options;

  const router = createMemoryRouter(createAppRoutes(), { initialEntries: [path] });

  // One loader identity for the whole mount: a fresh function on every render
  // would restart `SessionProvider`'s effect on every commit.
  const loadSession: SessionLoader =
    loadSessionOverride ??
    (mfaPending
      ? () => Promise.reject(new MfaPendingError())
      : () => (sessionError ? Promise.reject(sessionError) : Promise.resolve(user)));

  const result = render(
    <AppProviders connectionStatus={connection} loadSession={loadSession}>
      <RouterProvider router={router} />
    </AppProviders>,
  );

  return { ...result, router };
}
