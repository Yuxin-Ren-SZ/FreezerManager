// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * One order for every browser auth operation (#204).
 *
 * `auth/browser/login` sets the `fmgr_session` cookie and `auth/browser/logout`
 * expires it, and the browser applies each `Set-Cookie` when that response
 * arrives, not in the order the user acted. A logout still in flight when a
 * newer login succeeds would expire the newer cookie when its reply finally
 * lands. That can happen in one tab (since #203, sign-out reaches the login
 * screen before the server answers) and across tabs, which share the jar. The
 * gateway cannot prevent it: a deletion cookie cannot say "only if the jar
 * still holds the session I revoked".
 *
 * So the SPA runs login, MFA submission and logout one at a time, origin-wide:
 * an operation starts only once the previous one's response, cookies and all,
 * has been applied or abandoned. Across tabs that order comes from the Web
 * Locks API. `navigator.locks` exists only in secure contexts, which the
 * `Secure` session cookie already requires outside the dev loop; without it
 * the order holds within this tab only.
 */
export const BROWSER_AUTH_LOCK_NAME = 'fmgr-browser-auth-v1';

let tabTail: Promise<unknown> = Promise.resolve();

export function withBrowserAuthOrder<T>(operation: () => Promise<T>): Promise<T> {
  const { locks } = navigator as { locks?: LockManager };
  if (locks !== undefined) {
    return locks.request(BROWSER_AUTH_LOCK_NAME, { mode: 'exclusive' }, operation);
  }
  const run = tabTail.then(operation);
  tabTail = run.catch(() => undefined);
  return run;
}
