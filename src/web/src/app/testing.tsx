// SPDX-License-Identifier: AGPL-3.0-or-later
import { render } from '@testing-library/react';
import { createMemoryRouter, RouterProvider } from 'react-router-dom';
import { AppProviders } from './providers';
import { createAppRoutes } from './router';
import type { ConnectionState } from './connection';
import type { CurrentUser } from './session';

export interface RenderAppOptions {
  /** Starting URL; defaults to the dashboard. */
  path?: string;
  /** The session `auth/whoami` would return; `null` means signed out. */
  user?: CurrentUser | null;
  /** Make the session loader reject instead of resolving. */
  sessionError?: Error;
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
  const { path = '/', user = null, sessionError, connection = 'live' } = options;

  const router = createMemoryRouter(createAppRoutes(), { initialEntries: [path] });

  const result = render(
    <AppProviders
      connectionStatus={connection}
      loadSession={() => (sessionError ? Promise.reject(sessionError) : Promise.resolve(user))}
    >
      <RouterProvider router={router} />
    </AppProviders>,
  );

  return { ...result, router };
}
