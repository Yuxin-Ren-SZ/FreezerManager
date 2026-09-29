// SPDX-License-Identifier: AGPL-3.0-or-later
import { createBrowserRouter, type RouteObject } from 'react-router-dom';
import { AppErrorBoundary, RouteErrorBoundary } from './ErrorBoundary';
import { RequireSession, RouteGuard } from './guards';
import { NotFound } from './pages/NotFound';
import { ROUTES, type AppRoute } from './route-map';
import { AppShell } from './shell/AppShell';

/** The relative path a nested child route needs (React Router forbids a leading slash there). */
function childPath(route: AppRoute): string {
  return route.path.replace(/^\//, '');
}

/** The children of the shell route: every screen that renders inside the frame. */
function shellChildren(): RouteObject[] {
  return ROUTES.filter((route) => route.layout === 'shell').map((route) =>
    route.path === '/'
      ? {
          index: true,
          element: <RouteGuard route={route}>{route.element}</RouteGuard>,
        }
      : {
          path: childPath(route),
          element: <RouteGuard route={route}>{route.element}</RouteGuard>,
        },
  );
}

/**
 * The router's route objects, derived from `ROUTES` so the map stays the single
 * source of truth.
 *
 * `*` is last and inside the shell: a wrong address keeps the nav so the user
 * can walk out of the dead end. Login is a sibling of the shell rather than a
 * child, because it must render while there is no session to gate on.
 */
export function createAppRoutes(): RouteObject[] {
  return [
    {
      path: '/',
      element: (
        <RequireSession>
          <AppShell />
        </RequireSession>
      ),
      errorElement: <RouteErrorBoundary />,
      children: [...shellChildren(), { path: '*', element: <NotFound /> }],
    },
    ...ROUTES.filter((route) => route.layout === 'bare').map((route) => ({
      path: route.path,
      element: route.element,
    })),
  ];
}

/** The browser router `App` mounts. Tests use `createMemoryRouter` instead. */
export function createAppRouter() {
  return createBrowserRouter(createAppRoutes());
}

export { AppErrorBoundary };
