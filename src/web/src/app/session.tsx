// SPDX-License-Identifier: AGPL-3.0-or-later
import { QueryClientContext, type QueryClient } from '@tanstack/react-query';
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import type { ReactNode } from 'react';
import { signOutOfBrowserSession } from '../api/auth';
import { isMfaRequired } from '../api/errors';
import type { PermissionKey } from './permissions';

/**
 * The signed-in user, as `AuthService.WhoAmI` (G0.2) reports them.
 *
 * There is deliberately **no session id, token or expiry** here: the session is
 * an `HttpOnly` cookie (G-arch 6) and the SPA has no business holding any of
 * those, which is what AGENTS.md §5 requires and what `WhoAmI` promises not to
 * send. The shape is a subset of that response, so a field cannot creep in
 * without the RPC growing it.
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
  /** True for a deployment-wide administrator, as `WhoAmI` reports it. */
  isSystemAdmin: boolean;
  /** Deployment-wide grants, e.g. `backup.run` for a SystemAdmin. */
  permissions: readonly PermissionKey[];
  /** Every lab the user is a member of, with the role they hold in each. */
  labs: readonly LabMembership[];
}

export type SessionStatus =
  /** The loader has not answered yet. */
  | 'loading'
  | 'authenticated'
  /** A session exists but its second factor is outstanding (see `MfaPendingError`). */
  | 'mfa-pending'
  /** The server answered "no valid session"; the SPA sends the user to `/login`. */
  | 'unauthenticated'
  /** The loader could not be asked at all — a network failure, not a sign-out. */
  | 'error';

/**
 * Thrown by a `SessionLoader` when the server says a session exists whose
 * second factor is still outstanding.
 *
 * **This is not a sign-out.** `AuthService.Login` sets the browser's session
 * cookie *before* the TOTP code is entered, and `AuthMiddleware` refuses every
 * other RPC until it is (#62) — with `UNAUTHENTICATED`, the same status a dead
 * session gets. Without this distinction a half-finished login would look
 * exactly like an expired one, and the SPA would dead-end the user at the
 * password form instead of asking for the code. The state is deliberately
 * *resumable*: a reload lands back on `/login/mfa`.
 */
export class MfaPendingError extends Error {
  constructor(message = 'the session is waiting for its second factor') {
    super(message);
    this.name = 'MfaPendingError';
  }
}

export interface SessionReload {
  status: SessionStatus;
  user: CurrentUser | null;
  error: Error | null;
}

export interface SessionValue {
  status: SessionStatus;
  user: CurrentUser | null;
  /** Non-null only when `status === 'error'`. */
  error: Error | null;
  /** Re-runs the loader; the retry action on the session error screen. */
  reload: () => void;
  /**
   * Re-runs the loader *now* and reports what it found.
   *
   * The sign-in and second-factor screens use it after the server accepts a
   * credential: the cookie is the session, so the only way to learn who the
   * user now is — and whether the second factor is still outstanding — is to
   * ask again.
   */
  refresh: () => Promise<SessionReload>;
  /**
   * Ends the session: revokes it server-side, drops the query cache and returns
   * the app to the signed-out state. The server call is best-effort — a network
   * failure must not leave the user apparently signed in.
   */
  signOut: () => Promise<void>;
  /**
   * The session ended under us — any 401 from any request (G-arch 7). Clears
   * the query cache and puts the app in the signed-out state, or back on the
   * second-factor screen when the refusal is `mfa_required`.
   */
  expire: (cause?: unknown) => void;
}

/**
 * How the shell learns who is signed in: the `auth/whoami` call (G0.2).
 *
 * Resolves with `null` for "no valid session" (a 401), rejects with
 * `MfaPendingError` for "a session, but the second factor is outstanding", and
 * rejects with anything else for "could not ask" (a network failure) — those
 * three must not look the same, or a dropped connection would silently bounce a
 * user to the login page and a half-finished login would look like a dead one.
 */
export type SessionLoader = () => Promise<CurrentUser | null>;

const SessionContext = createContext<SessionValue | null>(null);

export interface SessionProviderProps {
  loadSession: SessionLoader;
  children: ReactNode;
}

interface SessionState {
  status: SessionStatus;
  user: CurrentUser | null;
  error: Error | null;
}

const SIGNED_OUT: SessionState = { status: 'unauthenticated', user: null, error: null };

function signedOutState(): SessionState {
  return { ...SIGNED_OUT };
}

function toError(cause: unknown): Error {
  return cause instanceof Error ? cause : new Error(String(cause));
}

/**
 * The query cache, when there is one above this provider.
 *
 * `useQueryClient()` throws without a `QueryClientProvider`, and a screen test
 * that only needs a session — `renderWithProviders(ui, { user })` — mounts this
 * provider without one. Reading the context directly keeps that legitimate:
 * with no cache there is nothing to clear, which is exactly the behaviour such a
 * test wants. In the app `AppProviders` always supplies one.
 */
function useQueryCache(): QueryClient | null {
  return useContext(QueryClientContext) ?? null;
}

export function SessionProvider({ loadSession, children }: SessionProviderProps) {
  const queryClient = useQueryCache();
  const [state, setState] = useState<SessionState>({ status: 'loading', user: null, error: null });
  const [attempt, setAttempt] = useState(0);

  // Every load gets an id, and only the newest may write state. Without it, a
  // slow first load that resolves after a sign-in would overwrite the session
  // with the answer to a question nobody is asking any more.
  const runId = useRef(0);

  const load = useCallback(async (): Promise<SessionReload> => {
    const id = ++runId.current;
    let next: SessionState;
    try {
      const loaded = await loadSession();
      next = {
        status: loaded === null ? 'unauthenticated' : 'authenticated',
        user: loaded,
        error: null,
      };
    } catch (cause) {
      next =
        cause instanceof MfaPendingError
          ? { status: 'mfa-pending', user: null, error: null }
          : { status: 'error', user: null, error: toError(cause) };
    }

    if (runId.current === id) {
      setState(next);
    }
    return { ...next };
  }, [loadSession]);

  // Loading in the effect body rather than during render is what keeps the
  // first paint off the network; the state write inside `load()` happens after
  // the await, never during render-commit.
  useEffect(() => {
    void load();
  }, [load, attempt]);

  const reload = useCallback(() => {
    setState((current) => ({ ...current, status: 'loading', error: null }));
    setAttempt((current) => current + 1);
  }, []);

  const refresh = useCallback(async (): Promise<SessionReload> => load(), [load]);

  const signOut = useCallback(async () => {
    // Anything already in flight belongs to the session being ended.
    runId.current += 1;
    try {
      await signOutOfBrowserSession();
    } catch {
      // The local session still ends. The cookie may outlive this call, but the
      // next request that uses it gets a 401 and `expire()` finishes the job —
      // leaving the user apparently signed in would be worse.
    }
    queryClient?.clear();
    setState(signedOutState());
  }, [queryClient]);

  const expire = useCallback(
    (cause?: unknown) => {
      runId.current += 1;
      // G-arch 7: any 401 clears the cache. The payloads may contain PHI, and a
      // screen that kept rendering them after the session ended would show data
      // the user is no longer allowed to see.
      queryClient?.clear();
      setState(
        isMfaRequired(cause)
          ? { status: 'mfa-pending', user: null, error: null }
          : signedOutState(),
      );
    },
    [queryClient],
  );

  const value = useMemo<SessionValue>(
    () => ({
      status: state.status,
      user: state.user,
      error: state.error,
      reload,
      refresh,
      signOut,
      expire,
    }),
    [state, reload, refresh, signOut, expire],
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
