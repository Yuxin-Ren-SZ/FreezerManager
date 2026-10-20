// SPDX-License-Identifier: AGPL-3.0-or-later
import { call } from './client';

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
 */

export interface SignInResult {
  /**
   * True when the password was accepted but a TOTP code is still required.
   * The session cookie is already set at that point, which is what makes the
   * second-factor screen a resumable state rather than a dead end (#62).
   */
  readonly mfaRequired: boolean;
}

/** `POST /api/v1/auth/browser/login` — email + password, TOTP still pending. */
export async function signIn(email: string, password: string): Promise<SignInResult> {
  const response = await call('auth/browser/login', { email, password });
  return { mfaRequired: response.mfaRequired };
}

/** `POST /api/v1/auth/browser/submit-mfa` — completes the second factor. */
export async function submitMfaCode(totpCode: string): Promise<void> {
  await call('auth/browser/submit-mfa', { totpCode });
}

/**
 * `POST /api/v1/auth/browser/logout` — revokes the session server-side and
 * expires both cookies. Token-only on the server (#62), so it also works for a
 * login whose second factor was never completed.
 */
export async function signOutOfBrowserSession(): Promise<void> {
  await call('auth/browser/logout', {});
}
