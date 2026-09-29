// SPDX-License-Identifier: AGPL-3.0-or-later
import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react';
import type { ReactNode } from 'react';
import type { PermissionKey } from './permissions';

/**
 * The signed-in user, as `AuthService.WhoAmI` (G0.2) will report them.
 *
 * `expiresAt` is the UTC-micros string G-arch 9 keeps everywhere; it is parsed
 * only for display, never for scheduling.
 */
export interface LabMembership {
  labId: string;
  labName: string;
  roleId: string;
  roleName: string;
  permissions: readonly PermissionKey[];
  isPhiEnabled: boolean;
}

export interface CurrentUser {
  userId: string;
  email: string;
  displayName: string;
  sessionId: string;
  mfaComplete: boolean;
  /** UTC micros, as a string (G-arch 4: int64 travels as a string). */
  expiresAt: string;
  /** Deployment-wide grants, e.g. `backup.run` for a SystemAdmin. */
  permissions: readonly PermissionKey[];
  labs: readonly LabMembership[];
}

export type SessionStatus = 'loading' | 'authenticated' | 'unauthenticated' | 'error';

export interface SessionValue {
  status: SessionStatus;
  user: CurrentUser | null;
  /** Non-null only when `status === 'error'`. */
  error: Error | null;
  /** Re-runs the loader; the retry action on the session error screen. */
  reload: () => void;
  /**
   * Clears the session locally. G2.1 replaces the body with
   * `auth/browser/logout` plus a query-cache reset — the shell only needs to
   * know that "signed out" is a state it can be put into.
   */
  signOut: () => void;
}

/**
 * How the shell learns who is signed in. G2.1 swaps `stubSessionLoader` for a
 * call to `auth/whoami`; nothing else in the shell changes.
 *
 * Resolves with `null` for "no valid session" (a 401), and rejects for
 * "could not ask" (a network failure) — the two must not look the same, or a
 * dropped connection would silently bounce a user to the login page.
 */
export type SessionLoader = () => Promise<CurrentUser | null>;

const SessionContext = createContext<SessionValue | null>(null);

export interface SessionProviderProps {
  loadSession: SessionLoader;
  children: ReactNode;
}

export function SessionProvider({ loadSession, children }: SessionProviderProps) {
  const [status, setStatus] = useState<SessionStatus>('loading');
  const [user, setUser] = useState<CurrentUser | null>(null);
  const [error, setError] = useState<Error | null>(null);
  const [attempt, setAttempt] = useState(0);

  // `reload()` is where the state goes back to `loading`; doing it here in the
  // effect body would be a synchronous setState during render-commit, which
  // cascades an extra render on every mount.
  useEffect(() => {
    let cancelled = false;

    loadSession().then(
      (loaded) => {
        if (cancelled) {
          return;
        }
        setUser(loaded);
        setStatus(loaded === null ? 'unauthenticated' : 'authenticated');
      },
      (cause: unknown) => {
        if (cancelled) {
          return;
        }
        setUser(null);
        setError(cause instanceof Error ? cause : new Error(String(cause)));
        setStatus('error');
      },
    );

    return () => {
      cancelled = true;
    };
  }, [loadSession, attempt]);

  const reload = useCallback(() => {
    setStatus('loading');
    setError(null);
    setAttempt((current) => current + 1);
  }, []);

  const signOut = useCallback(() => {
    setUser(null);
    setError(null);
    setStatus('unauthenticated');
  }, []);

  const value = useMemo<SessionValue>(
    () => ({ status, user, error, reload, signOut }),
    [status, user, error, reload, signOut],
  );

  return <SessionContext.Provider value={value}>{children}</SessionContext.Provider>;
}

export function useSession(): SessionValue {
  const value = useContext(SessionContext);
  if (value === null) {
    throw new Error('useSession() must be called inside a <SessionProvider>');
  }
  return value;
}

/**
 * Whether the user holds `permission`, globally or through a lab membership.
 *
 * With `labId` the check is scoped to that lab's membership. Without it, the
 * check passes if *any* membership grants the permission, which is what a
 * global affordance wants ("is there a lab where I could do this?").
 */
export function can(
  user: CurrentUser | null,
  permission: PermissionKey,
  labId?: string | null,
): boolean {
  if (user === null) {
    return false;
  }
  if (user.permissions.includes(permission)) {
    return true;
  }
  if (labId !== undefined && labId !== null) {
    const membership = user.labs.find((lab) => lab.labId === labId);
    return membership?.permissions.includes(permission) ?? false;
  }
  return user.labs.some((lab) => lab.permissions.includes(permission));
}

/** `can()` for components. */
export function useCan(permission: PermissionKey, labId?: string | null): boolean {
  const { user } = useSession();
  return can(user, permission, labId);
}
