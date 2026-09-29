// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect } from 'react';
import { onSessionExpired } from '../api/client';
import { useSession } from './session';

/**
 * Turns "a request was refused because there is no session" into app state.
 *
 * G-arch 7 requires any 401 to clear the query cache and end the session, and
 * `src/api/client.ts` reports exactly that — but only the session can act on it,
 * and only while there *is* a session. That guard is the whole reason this is a
 * component rather than a subscription inside `SessionProvider`:
 *
 * - `Login`, `SubmitMfa` and `Logout` answer `UNAUTHENTICATED` for their own
 *   reasons (a wrong password, a wrong code, a session already gone). None of
 *   them means "the session you had has ended", and tearing the sign-in screen
 *   out from under a user who mistyped a password would be absurd.
 * - `RequireSession` turns the resulting `unauthenticated` state into
 *   `/login?next=…`, so the user gets a login prompt instead of an error banner
 *   on a screen they cannot load.
 */
export function SessionExpiryWatcher() {
  const { status, expire } = useSession();

  useEffect(() => {
    if (status !== 'authenticated') {
      return undefined;
    }
    return onSessionExpired((error) => {
      expire(error);
    });
  }, [status, expire]);

  return null;
}
