// SPDX-License-Identifier: AGPL-3.0-or-later
import { Suspense, type ReactNode } from 'react';
import { createBrowserRouter, type RouteObject } from 'react-router-dom';
import { AppErrorBoundary, RouteErrorBoundary } from './ErrorBoundary';
import { RequireSession, RouteGuard } from './guards';
import { NotFound } from './pages/NotFound';
import { ScreenLoading } from './pages/ScreenLoading';
import { ROUTES, type AppRoute } from './route-map';
import { AppShell } from './shell/AppShell';

/** The relative path a nested child route needs (React Router forbids a leading slash there). */
function childPath(route: AppRoute): string {
  return route.path.replace(/^\//, '');
}

/**
 * One route's screen: the permission guard, then the screen's own chunk.
 *
 * The two wrappers are in this order for a reason. `RouteGuard` is what
 * redirects a signed-out user or explains a denial, and it must be able to do
 * that while the screen's chunk is still in flight — putting it inside the
 * `Suspense` boundary would show a spinner first and the explanation second.
 *
 * The boundary itself sits inside the shell's `<main>` (for shell routes) and
 * around the whole page (for `bare` routes like login), so what suspends is
 * always the screen and never the frame. `route.element` is a lazy element for
 * every screen in `route-map.tsx`; see issue #64 for why.
 */
function screenElement(route: AppRoute): ReactNode {
  return (
    <RouteGuard route={route}>
      <Suspense fallback={<ScreenLoading />}>{route.element}</Suspense>
    </RouteGuard>
  );
}

/** The children of the shell route: every screen that renders inside the frame. */
function shellChildren(): RouteObject[] {
  return ROUTES.filter((route) => route.layout === 'shell').map((route) =>
    route.path === '/'
      ? {
          index: true,
          element: screenElement(route),
        }
      : {
          path: childPath(route),
          element: screenElement(route),
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
    // Login and MFA are lazy too, and have no shell to fall back into, so their
    // boundary is the whole page. There is no `RouteGuard` on them by design:
    // the point of the route is to render while there is no session to check.
    ...ROUTES.filter((route) => route.layout === 'bare').map((route) => ({
      path: route.path,
      element: <Suspense fallback={<ScreenLoading />}>{route.element}</Suspense>,
    })),
  ];
}

/** The browser router `App` mounts. Tests use `createMemoryRouter` instead. */
export function createAppRouter() {
  return createBrowserRouter(createAppRoutes());
}

export { AppErrorBoundary };
