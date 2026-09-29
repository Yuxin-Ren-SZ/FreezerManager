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
 * This lives in `src/app/` rather than `src/test/` on purpose: until G1.2
 * (#42) merges, `src/test/` belongs to another agent's task, and this helper
 * is temporary scaffolding that should move there with the rest of the shared
 * fakes (see the G1.3 handoff note).
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
