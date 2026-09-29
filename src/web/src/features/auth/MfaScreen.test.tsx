// SPDX-License-Identifier: AGPL-3.0-or-later
import { screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it } from 'vitest';
import { call } from '../../api/client';
import { ALL_PERMISSIONS } from '../../app/permissions';
import { renderApp } from '../../app/testing';
import { currentUserWith } from '../../test/session';
import {
  createDemoLab,
  DEMO_MFA_EMAIL,
  DEMO_PASSWORD,
  DEMO_TOTP_CODE,
  fakeApi,
} from '../../test/fakeApi';
import { server } from '../../test/server';
import { axe } from '../../test/setup';
import authCopy from '../../../locales/en/auth.json';

/**
 * The second-factor screen (G2.1).
 *
 * G0.1 sets the session cookie *before* the TOTP code is entered, so this is a
 * real, resumable state rather than a corner of the login form: a reload lands
 * back here, and the only thing that ends it is an accepted code, an explicit
 * "start over", or the session expiring (#62).
 */
beforeEach(() => {
  server.use(...fakeApi());
});

/** Puts the fake into the state a password login for an MFA account leaves it in. */
async function startPendingLogin() {
  await call('auth/browser/login', { email: DEMO_MFA_EMAIL, password: DEMO_PASSWORD });
}

/**
 * The kit appends the required marker inside the `<label>`, so its text is
 * `Email*`. Substring matching tolerates that, and `SampleForm.test.tsx` does
 * the same thing with a regex for the same reason.
 */
async function findLabelled(label: string): Promise<HTMLElement> {
  return screen.findByLabelText(label, { exact: false });
}

async function submitCode(code: string) {
  await userEvent.type(await findLabelled(authCopy.mfa.code), code);
  await userEvent.click(screen.getByRole('button', { name: authCopy.mfa.submit }));
}

describe('MfaScreen', () => {
  it('completes the login with an accepted code and continues to the target', async () => {
    const target = '/labs/lab-1/samples';
    await startPendingLogin();
    let mfaComplete = false;
    const { router } = renderApp({
      path: `/login/mfa?next=${encodeURIComponent(target)}`,
      loadSession: () => Promise.resolve(mfaComplete ? currentUserWith(ALL_PERMISSIONS) : null),
    });

    mfaComplete = true;
    await submitCode(DEMO_TOTP_CODE);

    await waitFor(() => {
      expect(router.state.location.pathname).toBe(target);
    });
  });

  it('refuses a wrong code and keeps the half-finished login alive', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));
    await call('auth/browser/login', { email: DEMO_MFA_EMAIL, password: DEMO_PASSWORD });
    const { router } = renderApp({ path: '/login/mfa', mfaPending: true });

    await submitCode('000000');

    expect(await screen.findByRole('alert')).toHaveTextContent(authCopy.mfa.invalidCode);
    // Still here, and still pending: a mistyped digit must not cost the login,
    // and the server's refusal (`InvalidCredentials`) is not a sign-out.
    expect(router.state.location.pathname).toBe('/login/mfa');
    expect(lab.auth.pendingMfaUserId).toBe('user-mfa');
  });

  it('returns to the sign-in screen when the pending session is gone', async () => {
    // No password login was ever accepted in this fake, so the cookie the
    // browser is holding is not a session any more — the abandoned-login case.
    const { router } = renderApp({ path: '/login/mfa', user: null });

    await submitCode(DEMO_TOTP_CODE);

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/login');
    });
    expect(router.state.location.search).toContain('expired=1');
    // Looked up by its text, then asserted to be a live region: the route's own
    // `Suspense` fallback is also a `role="status"`, so a bare role query can
    // match a spinner instead of the notice.
    expect(await screen.findByText(authCopy.mfa.expired)).toHaveAttribute('role', 'status');
  });

  it('gives up the pending login when the user starts over', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));
    await startPendingLogin();
    const { router } = renderApp({ path: '/login/mfa', user: null });

    await userEvent.click(await screen.findByRole('button', { name: authCopy.mfa.startOver }));

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/login');
    });
    // #62: an abandoned login must be able to revoke the credential it was
    // handed, or the cookie outlives the attempt.
    expect(lab.auth.pendingMfaUserId).toBeNull();
  });

  it('sends a visitor whose session turns out to be complete straight in', async () => {
    // The code was already accepted elsewhere: `submit-mfa` refuses ("session
    // MFA is already complete"), and the right answer is the app, not an error.
    const { router } = renderApp({
      path: '/login/mfa',
      loadSession: () => Promise.resolve(currentUserWith(ALL_PERMISSIONS)),
    });

    await waitFor(() => {
      expect(router.state.location.pathname).toBe('/');
    });
  });

  it('does not offer a password field on the second-factor screen', async () => {
    await startPendingLogin();
    renderApp({ path: '/login/mfa', user: null });

    expect(await findLabelled(authCopy.mfa.code)).toBeInTheDocument();
    expect(screen.queryByLabelText(authCopy.password, { exact: false })).not.toBeInTheDocument();
  });

  it('asks for a one-time code the way a phone keyboard and a password manager expect', async () => {
    await startPendingLogin();
    renderApp({ path: '/login/mfa', user: null });

    const code = await findLabelled(authCopy.mfa.code);
    expect(code).toHaveAttribute('autocomplete', 'one-time-code');
    expect(code).toHaveAttribute('inputmode', 'numeric');
    expect(code).toHaveAttribute('maxlength', '6');
  });

  it('has no accessibility violations', async () => {
    await startPendingLogin();
    const { container } = renderApp({ path: '/login/mfa', user: null });
    await findLabelled(authCopy.mfa.code);

    expect(await axe(container)).toHaveNoViolations();
  });
});
