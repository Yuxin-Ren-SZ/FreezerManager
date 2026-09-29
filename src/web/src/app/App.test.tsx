// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { AppErrorBoundary, ErrorBoundary } from './ErrorBoundary';
import { ALL_PERMISSIONS, type PermissionKey } from './permissions';
import { ROUTES, type AppRoute } from './route-map';
import type { CurrentUser, LabMembership } from './session';
import { renderApp } from './testing';
import appShellCss from './shell/AppShell.module.css?raw';
import sideNavCss from './shell/SideNav.module.css?raw';

const LAB_ID = 'lab-1';

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
    sessionId: 's-1',
    mfaComplete: true,
    expiresAt: '4102444800000000',
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
    'renders the %s screen from the route map',
    async (_id, route) => {
      renderApp({ path: concretePath(route), user: user(ALL_PERMISSIONS) });

      // Every placeholder owns exactly one level-1 heading from its own
      // namespace. A missing route would render the 404 page instead, which has
      // no level-1 heading.
      expect(await screen.findByRole('heading', { level: 1 })).toBeInTheDocument();
      expect(screen.queryByRole('heading', { name: 'Page not found' })).not.toBeInTheDocument();
    },
  );

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
    renderApp({ path: `/labs/${LAB_ID}/samples`, user: user(['sample.read']) });

    expect(await screen.findByRole('heading', { level: 1, name: 'Samples' })).toBeInTheDocument();
    expect(screen.getByText(/G3\.2/)).toBeInTheDocument();
  });
});
