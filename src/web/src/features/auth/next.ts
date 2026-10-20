// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * The path a finished sign-in should continue to.
 *
 * `RequireSession` puts the interrupted address in `?next=`, and G2.1 has to
 * validate it before using it — the value comes from the URL bar, so it is
 * attacker-controllable: a link to
 * `/login?next=https://evil.example/` that the SPA follows would turn the sign-in
 * page into an open redirect, and `/login?next=//evil.example` is the same thing
 * with the scheme left off.
 *
 * So the rule is an allow-list, not a deny-list: keep only a **same-origin
 * absolute path** — one leading `/`, not followed by another `/` or a `\`
 * (browsers read `/\evil.example` as protocol-relative too). Anything else falls
 * back to the dashboard.
 */
export function safeNext(search: string): string {
  const raw = new URLSearchParams(search).get('next');
  return isSafePath(raw) ? raw : '/';
}

function isSafePath(value: string | null): value is string {
  if (value === null || value === '' || !value.startsWith('/')) {
    return false;
  }
  const second = value.charAt(1);
  return second !== '/' && second !== '\\';
}

/** `/login/mfa?next=…` — the second-factor step, carrying the target along. */
export function mfaPath(next: string): string {
  return `/login/mfa?next=${encodeURIComponent(next)}`;
}

/**
 * `/login?next=…`, optionally marked as "the previous attempt is over" so the
 * form can say so instead of looking like a first visit.
 */
export function loginPath(next: string, options: { expired?: boolean } = {}): string {
  const expired = options.expired === true ? '&expired=1' : '';
  return `/login?next=${encodeURIComponent(next)}${expired}`;
}
