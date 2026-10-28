// SPDX-License-Identifier: AGPL-3.0-or-later
import { call } from './client';
import { withBrowserAuthOrder } from './authOrder';
import { announceSessionChange } from '../app/sessionChange';

/**
 * The browser session: the three `auth/browser/*` routes G0.1 added
 * (`RestGateway.cc`), which are the same `AuthService` RPCs as `auth/*` but for
 * a client that cannot hold a bearer token.
 *
 * The gateway answers them with `Set-Cookie: fmgr_session=…; HttpOnly` and mints
 * `fmgr_csrf` alongside it, so **no token ever reaches JavaScript** (G-arch 6,
 * AGENTS.md §5). That is why `signIn` reads one field — `mfa_required` — out of
 * `LoginResponse` and deliberately ignores `session_token`, which the browser
 * route leaves empty in the first place.
 *
 * All three run through `withBrowserAuthOrder`, so a late logout reply can
 * never expire the cookie of a login that started after it (#204).
 */

export interface SignInResult {
  /**
   * True when the password was accepted but a TOTP code is still required.
   * The session cookie is already set at that point, which is what makes the
   * second-factor screen a resumable state rather than a dead end (#62).
   */
  readonly mfaRequired: boolean;
}

/**
 * How long a logout may hold the auth order before it is abandoned (#204).
 *
 * Without a deadline a hung logout would block every later sign-in, in every
 * tab. Abandoning it keeps the order sound: if its response headers had
 * arrived, the deletion cookie is already applied; if not, the aborted fetch
 * is never delivered, so its deletion cookie never lands. Local state is
 * already signed out by then (`SessionProvider.signOut`); as with any failed
 * logout, the server may not have revoked the session.
 */
export const BROWSER_LOGOUT_TIMEOUT_MS = 10_000;

/** `POST /api/v1/auth/browser/login` — email + password, TOTP still pending. */
export async function signIn(email: string, password: string): Promise<SignInResult> {
  return withBrowserAuthOrder(async () => {
    const response = await call('auth/browser/login', { email, password });
    announceSessionChange();
    return { mfaRequired: response.mfaRequired };
  });
}

/** `POST /api/v1/auth/browser/submit-mfa` — completes the second factor. */
export async function submitMfaCode(totpCode: string): Promise<void> {
  await withBrowserAuthOrder(async () => {
    await call('auth/browser/submit-mfa', { totpCode });
    announceSessionChange();
  });
}

/**
 * `POST /api/v1/auth/browser/logout` — revokes the session server-side and
 * expires both cookies. Token-only on the server (#62), so it also works for a
 * login whose second factor was never completed.
 */
export async function signOutOfBrowserSession(): Promise<void> {
  await withBrowserAuthOrder(async () => {
    const deadline = new AbortController();
    const timer = setTimeout(() => {
      deadline.abort();
    }, BROWSER_LOGOUT_TIMEOUT_MS);
    try {
      await call('auth/browser/logout', {}, { signal: deadline.signal });
    } finally {
      clearTimeout(timer);
    }
    announceSessionChange();
  });
}
