// SPDX-License-Identifier: AGPL-3.0-or-later
import { screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { ALL_PERMISSIONS } from '../../app/permissions';
import { renderApp } from '../../app/testing';
import { currentUserWith } from '../../test/session';
import { DEMO_MFA_EMAIL, DEMO_PASSWORD, DEMO_USER_EMAIL, fakeApi } from '../../test/fakeApi';
import { server } from '../../test/server';
import { axe } from '../../test/setup';
import authCopy from '../../../locales/en/auth.json';

/**
 * The sign-in form (G2.1). It talks to G0.1's existing browser route
 * (`POST /api/v1/auth/browser/login`), which puts the session in an `HttpOnly`
 * cookie — so nothing here may read, hold or store a token.
 */
beforeEach(() => {
  server.use(...fakeApi());
});

/**
 * The kit appends the required marker inside the `<label>`, so its text is
 * `Email*`. Substring matching tolerates that, and `SampleForm.test.tsx` does
 * the same thing with a regex for the same reason.
 */
function labelled(label: string): HTMLElement {
  return screen.getByLabelText(label, { exact: false });
}

async function findLabelled(label: string): Promise<HTMLElement> {
  return screen.findByLabelText(label, { exact: false });
}

async function submitCredentials(email: string, password: string) {
  await userEvent.type(await findLabelled(authCopy.email), email);
  await userEvent.type(labelled(authCopy.password), password);
  await userEvent.click(screen.getByRole('button', { name: authCopy.submit }));
}

describe('LoginScreen', () => {
  it('signs in with the seeded password and continues to the target', async () => {
    let signedIn = false;
    const { router } = renderApp({
      path: '/login',
      loadSession: () => Promise.resolve(signedIn ? currentUserWith(ALL_PERMISSIONS) : null),
    });

    signedIn = true;
    await submitCredentials(DEMO_USER_EMAIL, DEMO_PASSWORD);

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/');
    });
  });

  it('refuses a wrong password without saying which half was wrong', async () => {
    const { router } = renderApp({ path: '/login', user: null });

    await submitCredentials(DEMO_USER_EMAIL, 'not-the-password');

    // One message for an unknown email and a wrong password alike: naming the
    // field would turn the form into an account-enumeration oracle. The server
    // refuses both with the same `UNAUTHENTICATED`.
    expect(await screen.findByRole('alert')).toHaveTextContent(authCopy.invalidCredentials);
    expect(router.state.location.pathname).toBe('/login');
  });

  it('tells a user the server was unreachable rather than blaming the password', async () => {
    server.use(...fakeApi({ fail: { 'auth/browser/login': 'UNAVAILABLE' } }));
    renderApp({ path: '/login', user: null });

    await submitCredentials(DEMO_USER_EMAIL, DEMO_PASSWORD);

    expect(await screen.findByRole('alert')).toHaveTextContent(authCopy.unreachable);
  });

  it('does not treat a refused sign-in as an expired session', async () => {
    // `client.ts` reports every `UNAUTHENTICATED` to the session-expired
    // listeners; a login that fails is not a session ending, and the form must
    // keep rendering instead of being torn out from under the user.
    const { router } = renderApp({ path: '/login', user: null });

    await submitCredentials(DEMO_USER_EMAIL, 'not-the-password');

    expect(await screen.findByRole('alert')).toBeInTheDocument();
    expect(router.state.location.pathname).toBe('/login');
    expect(labelled(authCopy.password)).toBeInTheDocument();
  });

  it('sends an account with a second factor to the code prompt', async () => {
    const { router } = renderApp({ path: '/login', user: null });

    await submitCredentials(DEMO_MFA_EMAIL, DEMO_PASSWORD);

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/login/mfa');
    });
    expect(
      await screen.findByRole('heading', { level: 1, name: authCopy.mfa.title }),
    ).toBeInTheDocument();
  });

  it('keeps the interrupted address through the code prompt', async () => {
    const target = '/labs/lab-1/samples';
    const { router } = renderApp({
      path: `/login?next=${encodeURIComponent(target)}`,
      user: null,
    });

    await submitCredentials(DEMO_MFA_EMAIL, DEMO_PASSWORD);

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/login/mfa');
    });
    expect(router.state.location.search).toBe(`?next=${encodeURIComponent(target)}`);
  });

  it('refuses a next target that leaves this origin', async () => {
    let signedIn = false;
    const { router } = renderApp({
      path: `/login?next=${encodeURIComponent('//evil.example/steal')}`,
      loadSession: () => Promise.resolve(signedIn ? currentUserWith(ALL_PERMISSIONS) : null),
    });

    signedIn = true;
    await submitCredentials(DEMO_USER_EMAIL, DEMO_PASSWORD);

    // An open redirect here would make the sign-in page the first hop of a
    // phishing chain, so anything but a same-origin path falls back home.
    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/');
    });
  });

  it('sends a visitor who is already signed in on to the target', async () => {
    const { router } = renderApp({ path: '/login', user: currentUserWith(ALL_PERMISSIONS) });

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/');
    });
    expect(screen.queryByLabelText(authCopy.password, { exact: false })).not.toBeInTheDocument();
  });

  it('disables the form while the request is in flight', async () => {
    server.use(...fakeApi({ latencyMs: 50 }));
    renderApp({ path: '/login', user: null });

    await submitCredentials(DEMO_USER_EMAIL, DEMO_PASSWORD);

    // A regex, because the loading spinner's hidden "Loading…" is part of the
    // button's accessible name (`controls.test.tsx` asserts the same way).
    const button = screen.getByRole('button', { name: new RegExp(authCopy.submitting, 'i') });
    expect(button).toBeDisabled();
    expect(button).toHaveAttribute('aria-busy', 'true');
    expect(await screen.findByRole('button', { name: authCopy.submit })).toBeEnabled();
  });

  it('explains an expired second factor when the code prompt sent the user back', async () => {
    renderApp({ path: '/login?expired=1', user: null });

    // By text, then asserted to be a live region: the lazy route's `Suspense`
    // fallback is also a `role="status"`, so a role query alone is ambiguous.
    expect(await screen.findByText(authCopy.mfa.expired)).toHaveAttribute('role', 'status');
  });

  it('labels both fields, so a password manager and a screen reader agree', async () => {
    renderApp({ path: '/login', user: null });

    const email = await findLabelled(authCopy.email);
    expect(email).toHaveAttribute('type', 'email');
    expect(email).toHaveAttribute('autocomplete', 'username');

    const password = labelled(authCopy.password);
    expect(password).toHaveAttribute('type', 'password');
    expect(password).toHaveAttribute('autocomplete', 'current-password');
  });

  it('has no accessibility violations', async () => {
    const { container } = renderApp({ path: '/login', user: null });
    await findLabelled(authCopy.email);

    expect(await axe(container)).toHaveNoViolations();
  });
});

describe('LoginScreen not leaking the session', () => {
  it('never writes to localStorage or sessionStorage', async () => {
    const localSet = vi.spyOn(Storage.prototype, 'setItem');
    renderApp({ path: '/login', user: null });

    await submitCredentials(DEMO_USER_EMAIL, DEMO_PASSWORD);
    await screen.findByRole('alert');

    // AGENTS.md §5 / G-arch 6-7: no PHI, no token, no session id in browser
    // storage. `fmgr.selectedLabId` is the only allowed key, and this screen
    // has no business writing even that.
    expect(localSet).not.toHaveBeenCalled();

    localSet.mockRestore();
  });
});
