// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen, waitFor, within } from '@testing-library/react';
import { MemoryRouter, Route, Routes } from 'react-router-dom';
import userEvent from '@testing-library/user-event';
import { HttpResponse, http } from 'msw';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { AppErrorBoundary, ErrorBoundary } from './ErrorBoundary';
import { AppProviders } from './providers';
import { RouteGuard } from './guards';
import { ALL_PERMISSIONS, type PermissionKey } from './permissions';
import { ROUTES, type AppRoute } from './route-map';
import type { CurrentUser, LabMembership } from './session';
import { renderApp } from './testing';
import {
  DEMO_MFA_EMAIL,
  DEMO_PASSWORD,
  DEMO_TOTP_CODE,
  DEMO_USER_EMAIL,
  fakeApi,
} from '../test/fakeApi';
import { server } from '../test/server';
import appShellCss from './shell/AppShell.module.css?raw';
import sideNavCss from './shell/SideNav.module.css?raw';
import accountCopy from '../../locales/en/account.json';
import auditCopy from '../../locales/en/audit.json';
import authCopy from '../../locales/en/auth.json';
import boxCopy from '../../locales/en/box.json';
import homeCopy from '../../locales/en/home.json';
import csvImportCopy from '../../locales/en/import.json';
import itemTypesCopy from '../../locales/en/itemTypes.json';
import layoutCopy from '../../locales/en/layout.json';
import lookupCopy from '../../locales/en/lookup.json';
import membersCopy from '../../locales/en/members.json';
import sampleDetailCopy from '../../locales/en/sample-detail.json';
import samplesCopy from '../../locales/en/samples.json';
import scanCopy from '../../locales/en/scan.json';
import sharesCopy from '../../locales/en/shares.json';

/**
 * What each route in the map must render: the title declared by the namespace
 * that screen is supposed to own, and the TODO id that will replace it.
 *
 * This is an independent restatement of the mapping, not a copy of it — which
 * is the whole point. A route wired to the wrong namespace, or to a namespace
 * belonging to another feature task, disagrees with this table.
 *
 * `task: null` means the real screen has replaced the placeholder (G3.1's
 * layout tree, G3.2's sample browser and G3.3's two sample screens so far): it
 * no longer renders its TODO id, so the assertion below flips to "the
 * placeholder sentence is gone", which is what catches a route quietly reverted
 * to `PlaceholderScreen`.
 */
const EXPECTED_SCREEN: Record<string, { title: string; task: string | null }> = {
  // G2.1 replaced both auth placeholders: the sign-in form and the second-factor
  // screen own their headings now, which is why their `task` is null here.
  login: { title: authCopy.title, task: null },
  'login-mfa': { title: authCopy.mfa.title, task: null },
  home: { title: homeCopy.title, task: 'G4.2' },
  lookup: { title: lookupCopy.title, task: null },
  samples: { title: samplesCopy.title, task: null },
  // G3.3 replaced both of its placeholders. The create screen's heading is its
  // own namespace's form title; the detail screen's heading is the sample's
  // *name*, which for the route map's `:sampleId` is the fake's seeded
  // `sample-1`.
  'sample-new': { title: sampleDetailCopy.form.createTitle, task: null },
  'sample-detail': { title: 'Serum A', task: null },
  layout: { title: layoutCopy.title, task: null },
  // G3.4 replaced the box placeholder too. Its heading names the box being
  // viewed, and the fake seeds its boxes in `lab-demo` while `concretePath`
  // visits `lab-1` — so the screen has no box to name and falls back to the id
  // in the path, which is exactly the state the title has to survive.
  box: { title: boxCopy.title.replace('{{label}}', 'box-1'), task: null },
  scan: { title: scanCopy.title, task: null },
  'csv-import': { title: csvImportCopy.title, task: 'G3.7' },
  shares: { title: sharesCopy.title, task: 'G3.13' },
  'admin-layout': { title: layoutCopy.title, task: 'G3.8' },
  // G3.9 replaced the item-types placeholder. Its heading is the namespace's
  // title, and for `lab-1` there are no item types in the fake, so the screen
  // renders the tree's empty state and still names itself.
  'item-types': { title: itemTypesCopy.title, task: null },
  members: { title: membersCopy.title, task: 'G3.10' },
  audit: { title: auditCopy.title, task: 'G3.12' },
  account: { title: accountCopy.title, task: 'G3.11' },
};

const LAB_ID = 'lab-1';

// Every route in the map is rendered below, and a real screen fetches: G3.1's
// layout tree is the first one that does. `fakeApi()` answers every route in
// `routes.ts`, so the app test does not need to know which screens fetch what —
// and an unimplemented route answering with protobuf defaults is exactly what
// the real server does for an empty lab.
beforeEach(() => {
  server.use(...fakeApi());
});

function lab(permissions: readonly PermissionKey[], labId = LAB_ID): LabMembership {
  return {
    labId,
    labName: labId === LAB_ID ? 'Demo Lab' : 'Other Lab',
    roleId: 'role-1',
    roleName: 'Member',
    permissions,
    isPhiEnabled: false,
  };
}

function user(
  permissions: readonly PermissionKey[],
  labs: readonly LabMembership[] = [lab(permissions)],
): CurrentUser {
  return {
    userId: 'u-1',
    email: 'ada@example.invalid',
    displayName: 'Ada Lovelace',
    isSystemAdmin: false,
    permissions: [],
    labs,
  };
}

/** Substitutes every `:param` in the route map, so each path can be visited. */
function concretePath(route: AppRoute): string {
  return route.path
    .replace(':labId', LAB_ID)
    .replace(':sampleId', 'sample-1')
    .replace(':boxId', 'box-1');
}

describe('app shell', () => {
  it('renders the top bar, the side nav and the main landmark', async () => {
    renderApp({ path: '/', user: user(ALL_PERMISSIONS) });

    expect(await screen.findByRole('banner')).toBeInTheDocument();
    expect(screen.getByRole('navigation', { name: 'Main navigation' })).toBeInTheDocument();
    expect(screen.getByRole('main')).toBeInTheDocument();
  });

  it('links home from the brand', async () => {
    renderApp({ path: '/lookup', user: user(ALL_PERMISSIONS) });

    expect(await screen.findByRole('link', { name: 'FreezerManager' })).toHaveAttribute(
      'href',
      '/',
    );
  });

  it.each(ROUTES.map((route) => [route.id, route] as const))(
    'renders the %s screen from the route map, and only that screen',
    async (id, route) => {
      const expected = EXPECTED_SCREEN[id];

      // The two `bare` routes are the signed-*out* half of the app — `/login`
      // exists precisely for a visitor with no session, and the route loop used
      // to render it with a signed-in user, which the real screen answers with a
      // redirect. Every other route is visited signed in.
      renderApp({
        path: concretePath(route),
        user: route.layout === 'bare' ? null : user(ALL_PERMISSIONS),
      });

      // Asserting that *a* level-1 heading exists would not catch a screen
      // wired to the wrong path: a copy-paste in the route map — `/labs/:labId/
      // audit` rendering the samples screen — would still pass. So the heading
      // has to be the one this route's namespace declares, and the body has to
      // name this route's TODO id.
      //
      // The expected title is read from the namespace's own JSON rather than
      // written out here, so renaming a screen is not a test failure; wiring a
      // route to the wrong namespace is.
      expect(
        await screen.findByRole('heading', { level: 1, name: expected.title }),
      ).toBeInTheDocument();
      if (expected.task === null) {
        // An implemented screen no longer names the TODO id that replaced it;
        // the title above still proves the namespace, and this proves the
        // placeholder is gone.
        expect(screen.queryByText(/This screen is a placeholder/)).not.toBeInTheDocument();
      } else {
        // The id is interpolated into the placeholder sentence, so match on
        // containment rather than on a text node that is exactly the id.
        expect(screen.getByText(expected.task, { exact: false })).toBeInTheDocument();
      }
      expect(screen.queryByRole('heading', { name: 'Page not found' })).not.toBeInTheDocument();
    },
  );

  it('has an explicit expectation for every route in the map', () => {
    // Adding a route without deciding what it renders is the failure this
    // catches, rather than the test above quietly skipping it.
    expect(Object.keys(EXPECTED_SCREEN).sort()).toEqual(ROUTES.map((route) => route.id).sort());
  });

  it('renders the 404 page inside the shell for an address that is not in the map', async () => {
    renderApp({ path: '/no/such/place', user: user(ALL_PERMISSIONS) });

    expect(await screen.findByRole('heading', { name: 'Page not found' })).toBeInTheDocument();
    expect(screen.getByRole('navigation', { name: 'Main navigation' })).toBeInTheDocument();
  });

  it('sends a signed-out visitor to a full-page route with no shell around it', async () => {
    renderApp({ path: '/lookup', user: null });

    expect(await screen.findByRole('heading', { level: 1 })).toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'Main navigation' })).not.toBeInTheDocument();
  });

  it('shows a retry instead of the login screen when the session check itself fails', async () => {
    renderApp({ path: '/', user: null, sessionError: new Error('network down') });

    expect(
      await screen.findByRole('heading', { name: 'Could not check your session' }),
    ).toBeInTheDocument();
    expect(screen.getByText('network down')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Try again' })).toBeInTheDocument();
  });

  it('keeps the nav usable while a screen is denied', async () => {
    renderApp({ path: `/labs/${LAB_ID}/audit`, user: user(['sample.read']) });

    expect(
      await screen.findByRole('heading', { name: 'You do not have access to this screen' }),
    ).toBeInTheDocument();
    expect(screen.getByRole('navigation', { name: 'Main navigation' })).toBeInTheDocument();
  });

  it('tells a user with no lab that there is no lab, not that they lack a permission', async () => {
    renderApp({ path: `/labs/${LAB_ID}/samples`, user: user([], []) });

    expect(
      await screen.findByText(
        'You are not a member of any lab yet, so there is nothing to show at this address.',
      ),
    ).toBeInTheDocument();
  });

  it('still applies the lab check to a route that needs no permission', async () => {
    // No route is scoped *and* permission-free today, so the ordering inside
    // `RouteGuard` makes no difference to the real map — which is exactly why
    // it needs a synthetic route to be tested at all. With the "no permission
    // required" shortcut first, this screen would render against a `:labId`
    // that nobody checked the user belongs to.
    const scopedButOpen: AppRoute = {
      id: 'scoped-but-open',
      path: '/labs/:labId/open',
      task: 'G0.0',
      element: <p>open screen</p>,
      permissions: null,
      scoped: true,
      layout: 'shell',
      nav: null,
    };

    render(
      <AppProviders connectionStatus="live" loadSession={() => Promise.resolve(user([], []))}>
        <MemoryRouter initialEntries={[`/labs/${LAB_ID}/open`]}>
          <Routes>
            <Route
              path="/labs/:labId/open"
              element={<RouteGuard route={scopedButOpen}>{scopedButOpen.element}</RouteGuard>}
            />
          </Routes>
        </MemoryRouter>
      </AppProviders>,
    );

    expect(await screen.findByText(/not a member of any lab/)).toBeInTheDocument();
    expect(screen.queryByText('open screen')).not.toBeInTheDocument();
  });
});

describe('side nav', () => {
  it('shows an entry behind a permission the user holds', async () => {
    renderApp({ path: '/', user: user(['sample.read', 'audit.read']) });

    const nav = await screen.findByRole('navigation', { name: 'Main navigation' });
    expect(within(nav).getByRole('link', { name: 'Audit' })).toHaveAttribute(
      'href',
      `/labs/${LAB_ID}/audit`,
    );
  });

  it('omits entries behind permissions the user lacks', async () => {
    renderApp({ path: '/', user: user(['sample.read']) });

    const nav = await screen.findByRole('navigation', { name: 'Main navigation' });
    expect(within(nav).queryByRole('link', { name: 'Audit' })).not.toBeInTheDocument();
    expect(within(nav).queryByRole('link', { name: 'Members' })).not.toBeInTheDocument();
  });

  it('shows an entry that needs only one of its two permissions', async () => {
    renderApp({ path: '/', user: user(['user.invite']) });

    const nav = await screen.findByRole('navigation', { name: 'Main navigation' });
    expect(within(nav).getByRole('link', { name: 'Members' })).toBeInTheDocument();
  });

  it('omits lab-scoped entries when the user belongs to no lab', async () => {
    renderApp({ path: '/', user: user([], []) });

    const nav = await screen.findByRole('navigation', { name: 'Main navigation' });
    expect(within(nav).getByRole('link', { name: 'Home' })).toBeInTheDocument();
    expect(within(nav).queryByRole('link', { name: 'Samples' })).not.toBeInTheDocument();
  });

  it('lays out as a horizontal strip below 48rem, down to 360px', () => {
    // jsdom applies no stylesheets, so the responsive behaviour is asserted
    // against the rules that produce it — the same technique the sticky table
    // header uses. Both files have to agree: the nav strips and the shell body
    // collapses to one column.
    expect(sideNavCss).toMatch(/@media\s*\(max-width:\s*48rem\)/);
    expect(sideNavCss).toMatch(/\.nav\s*\{[^}]*flex-direction:\s*row/s);
    expect(sideNavCss).toMatch(/\.nav\s*\{[^}]*overflow-x:\s*auto/s);
    expect(appShellCss).toMatch(/@media\s*\(max-width:\s*48rem\)/);
    expect(appShellCss).toMatch(/\.body\s*\{[^}]*grid-template-columns:\s*minmax\(0,\s*1fr\)/s);
  });
});

describe('global lookup', () => {
  it('focuses the lookup box when / is pressed', async () => {
    renderApp({ path: '/', user: user(ALL_PERMISSIONS) });
    const box = await screen.findByRole('searchbox', {
      name: 'Search samples by name or barcode',
    });

    expect(box).not.toHaveFocus();
    await userEvent.keyboard('/');

    expect(box).toHaveFocus();
  });

  it('does not steal the keystroke while the user is typing', async () => {
    renderApp({ path: '/', user: user(ALL_PERMISSIONS) });
    const box = await screen.findByRole('searchbox', {
      name: 'Search samples by name or barcode',
    });

    await userEvent.click(box);
    await userEvent.keyboard('/');

    expect(box).toHaveValue('/');
  });

  it('navigates to the lookup screen with the query', async () => {
    const { router } = renderApp({ path: '/', user: user(ALL_PERMISSIONS) });
    const box = await screen.findByRole('searchbox', {
      name: 'Search samples by name or barcode',
    });

    await userEvent.type(box, 'liver{Enter}');

    expect(router.state.location.pathname).toBe('/lookup');
    expect(router.state.location.search).toBe('?q=liver');
  });
});

describe('lab picker', () => {
  const twoLabs = user([], [lab(['sample.read'], 'lab-1'), lab(['sample.read'], 'lab-2')]);

  it('lists every membership and points the nav at the chosen lab', async () => {
    renderApp({ path: '/', user: twoLabs });
    const picker = await screen.findByLabelText('Current lab');

    await userEvent.selectOptions(picker, 'lab-2');

    const nav = screen.getByRole('navigation', { name: 'Main navigation' });
    expect(within(nav).getByRole('link', { name: 'Samples' })).toHaveAttribute(
      'href',
      '/labs/lab-2/samples',
    );
  });

  it('remembers the chosen lab as a UI preference', async () => {
    renderApp({ path: '/', user: twoLabs });
    const picker = await screen.findByLabelText('Current lab');

    await userEvent.selectOptions(picker, 'lab-2');

    expect(globalThis.localStorage.getItem('fmgr.selectedLabId')).toBe('lab-2');
  });
});

describe('user menu', () => {
  it('opens and offers the account screen and sign-out', async () => {
    renderApp({ path: '/', user: user(ALL_PERMISSIONS) });

    await userEvent.click(await screen.findByRole('button', { name: 'Ada Lovelace' }));

    expect(await screen.findByRole('menuitem', { name: 'Account settings' })).toHaveAttribute(
      'href',
      '/account',
    );
    expect(screen.getByRole('menuitem', { name: 'Sign out' })).toBeInTheDocument();
  });

  it('returns to the login screen when signing out', async () => {
    const { router } = renderApp({ path: '/', user: user(ALL_PERMISSIONS) });

    await userEvent.click(await screen.findByRole('button', { name: 'Ada Lovelace' }));
    await userEvent.click(await screen.findByRole('menuitem', { name: 'Sign out' }));

    expect(router.state.location.pathname).toBe('/login');
  });
});

describe('live-connection indicator', () => {
  it.each([
    ['live', 'Live updates: Live'],
    ['connecting', 'Live updates: Connecting'],
    ['offline', 'Live updates: Offline'],
  ] as const)('shows %s', async (connection, label) => {
    renderApp({ path: '/', user: user(ALL_PERMISSIONS), connection });

    expect(await screen.findByRole('status', { name: label })).toBeInTheDocument();
  });
});

describe('error boundary', () => {
  it('replaces a crashed subtree with the fallback and can recover', async () => {
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);
    let shouldThrow = true;

    function Bomb() {
      if (shouldThrow) {
        throw new Error('render exploded');
      }
      return <p>Recovered</p>;
    }

    const { rerender } = render(
      <ErrorBoundary
        fallback={(_error, reset) => (
          <button
            type="button"
            onClick={() => {
              shouldThrow = false;
              reset();
            }}
          >
            Try again
          </button>
        )}
      >
        <Bomb />
      </ErrorBoundary>,
    );

    expect(screen.getByRole('button', { name: 'Try again' })).toBeInTheDocument();
    expect(screen.queryByText('Recovered')).not.toBeInTheDocument();

    await userEvent.click(screen.getByRole('button', { name: 'Try again' }));
    rerender(
      <ErrorBoundary fallback={() => null}>
        <Bomb />
      </ErrorBoundary>,
    );

    expect(screen.getByText('Recovered')).toBeInTheDocument();
    consoleError.mockRestore();
  });

  it('renders the translated fallback through AppErrorBoundary', () => {
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);

    function Bomb(): never {
      throw new Error('boom');
    }

    render(
      <AppErrorBoundary>
        <Bomb />
      </AppErrorBoundary>,
    );

    expect(screen.getByRole('heading', { name: 'Something went wrong' })).toBeInTheDocument();
    consoleError.mockRestore();
  });
});

describe('placeholder screens', () => {
  it('name the TODO item that replaces them', async () => {
    // The samples route used to be the example here; G3.2, G3.6 and the others
    // replaced their placeholders, so this uses a route that still is one (the
    // shares screen, G3.13).
    renderApp({ path: `/labs/${LAB_ID}/shares`, user: user(['share.request']) });

    expect(
      await screen.findByRole('heading', { level: 1, name: sharesCopy.title }),
    ).toBeInTheDocument();
    expect(screen.getByText(/G3\.13/)).toBeInTheDocument();
  });
});

describe('session lifecycle (G2.1)', () => {
  it('returns to the sign-in screen when a request says the session is over', async () => {
    // The demo-breaking state: signed in, then the session ends server-side. The
    // audit saw the SPA answer this with twenty 500s and an error banner on a
    // screen the user cannot load, instead of a login prompt.
    const { router } = renderApp({ path: '/', user: user(ALL_PERMISSIONS) });
    const nav = await screen.findByRole('navigation', { name: 'Main navigation' });

    server.use(...fakeApi({ fail: { 'sample/list': 'UNAUTHENTICATED' } }));
    await userEvent.click(within(nav).getByRole('link', { name: 'Samples' }));

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/login');
    });
    // The interrupted address travels with the redirect, so signing in again
    // lands where the user was going rather than on the dashboard.
    expect(router.state.location.search).toBe(
      `?next=${encodeURIComponent(`/labs/${LAB_ID}/samples`)}`,
    );
    expect(
      await screen.findByRole('heading', { level: 1, name: authCopy.title }),
    ).toBeInTheDocument();
    expect(screen.queryByRole('heading', { name: 'Something went wrong' })).not.toBeInTheDocument();
  });

  it('sends a half-finished login to the second-factor screen, not the sign-in form', async () => {
    // `AuthService.Login` sets the session cookie *before* the TOTP code is
    // entered (#62), so this is a real, resumable state: `auth/whoami` refuses
    // the session and the SPA must offer the code prompt rather than log the
    // user out.
    const { router } = renderApp({ path: `/labs/${LAB_ID}/samples`, mfaPending: true });

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/login/mfa');
    });
    expect(router.state.location.search).toBe(
      `?next=${encodeURIComponent(`/labs/${LAB_ID}/samples`)}`,
    );
    expect(
      await screen.findByRole('heading', { level: 1, name: authCopy.mfa.title }),
    ).toBeInTheDocument();
  });

  it('revokes the session on the server when signing out', async () => {
    let logoutCalls = 0;
    server.use(
      http.post('/api/v1/auth/browser/logout', () => {
        logoutCalls += 1;
        return HttpResponse.json({});
      }),
    );
    const { router } = renderApp({ path: '/', user: user(ALL_PERMISSIONS) });

    await userEvent.click(await screen.findByRole('button', { name: 'Ada Lovelace' }));
    await userEvent.click(await screen.findByRole('menuitem', { name: 'Sign out' }));

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/login');
    });
    // Clearing the local session is not signing out: the cookie is the session
    // (G-arch 6), so the server has to revoke it too or the next request still
    // carries authority the user gave up.
    expect(logoutCalls).toBe(1);
  });

  it('signs in and continues to the interrupted screen', async () => {
    let signedIn = false;
    const { router } = renderApp({
      path: `/login?next=${encodeURIComponent(`/labs/${LAB_ID}/samples`)}`,
      loadSession: () => Promise.resolve(signedIn ? user(ALL_PERMISSIONS) : null),
    });

    await userEvent.type(
      await screen.findByLabelText(authCopy.email, { exact: false }),
      DEMO_USER_EMAIL,
    );
    await userEvent.type(screen.getByLabelText(authCopy.password, { exact: false }), DEMO_PASSWORD);
    signedIn = true;
    await userEvent.click(screen.getByRole('button', { name: authCopy.submit }));

    await waitFor(() => {
      expect(router.state.location.pathname).toBe(`/labs/${LAB_ID}/samples`);
    });
    expect(
      await screen.findByRole('heading', { level: 1, name: samplesCopy.title }),
    ).toBeInTheDocument();
  });

  it('walks a login with a second factor through the code prompt', async () => {
    let mfaComplete = false;
    const { router } = renderApp({
      path: '/login',
      loadSession: () => Promise.resolve(mfaComplete ? user(ALL_PERMISSIONS) : null),
    });

    await userEvent.type(
      await screen.findByLabelText(authCopy.email, { exact: false }),
      DEMO_MFA_EMAIL,
    );
    await userEvent.type(screen.getByLabelText(authCopy.password, { exact: false }), DEMO_PASSWORD);
    await userEvent.click(screen.getByRole('button', { name: authCopy.submit }));

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/login/mfa');
    });

    mfaComplete = true;
    await userEvent.type(
      await screen.findByLabelText(authCopy.mfa.code, { exact: false }),
      DEMO_TOTP_CODE,
    );
    await userEvent.click(screen.getByRole('button', { name: authCopy.mfa.submit }));

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/');
    });
  });
});
