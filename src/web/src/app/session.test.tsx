// SPDX-License-Identifier: AGPL-3.0-or-later
import { QueryClientProvider, type QueryClient } from '@tanstack/react-query';
import { act, renderHook, waitFor } from '@testing-library/react';
import type { ReactNode } from 'react';
import { HttpResponse, http } from 'msw';
import { beforeEach, describe, expect, it } from 'vitest';
import { ApiError, MFA_REQUIRED_PREFIX } from '../api/errors';
import { createTestQueryClient } from '../test/render';
import { currentUserWith } from '../test/session';
import { fakeApi } from '../test/fakeApi';
import { server } from '../test/server';
import { MfaPendingError, SessionProvider, useSession, type SessionLoader } from './session';

/**
 * The session state machine (G2.1), on its own.
 *
 * `App.test.tsx` covers what the *app* does with each state — the redirects, the
 * sign-out menu, the expired-session bounce. This file covers the provider: the
 * four states it distinguishes, and the two side effects that must not be
 * skipped, because both are `AGENTS.md` §5 requirements rather than UX polish —
 * the query cache holds API payloads (which may contain PHI) and must not
 * outlive the session, and the server must be told when the session ends.
 */
beforeEach(() => {
  server.use(...fakeApi());
});

function renderSession(loader: SessionLoader) {
  const queryClient: QueryClient = createTestQueryClient();
  const wrapper = ({ children }: { children: ReactNode }) => (
    <QueryClientProvider client={queryClient}>
      <SessionProvider loadSession={loader}>{children}</SessionProvider>
    </QueryClientProvider>
  );

  return { queryClient, ...renderHook(() => useSession(), { wrapper }) };
}

/** Something a screen's query would have cached before the session ended. */
function seedCache(queryClient: QueryClient): void {
  queryClient.setQueryData(['sample', 'sample-1'], { name: 'Serum A' });
}

describe('SessionProvider', () => {
  it('reports the user the loader resolved', async () => {
    const user = currentUserWith(['sample.read']);
    const { result } = renderSession(() => Promise.resolve(user));

    await waitFor(() => {
      expect(result.current.status).toBe('authenticated');
    });
    expect(result.current.user).toEqual(user);
  });

  it('keeps "no session" and "could not ask" apart', async () => {
    const signedOut = renderSession(() => Promise.resolve(null));
    await waitFor(() => {
      expect(signedOut.result.current.status).toBe('unauthenticated');
    });
    expect(signedOut.result.current.error).toBeNull();

    // A dropped connection must never look like being signed out: the guard
    // shows a retry for this state and the login page for the other.
    const unreachable = renderSession(() =>
      Promise.reject(new ApiError('UNAVAILABLE', 'the server could not be reached')),
    );
    await waitFor(() => {
      expect(unreachable.result.current.status).toBe('error');
    });
    expect(unreachable.result.current.error?.message).toBe('the server could not be reached');
  });

  it('gives a half-finished login its own state rather than signing the user out', async () => {
    const { result } = renderSession(() => Promise.reject(new MfaPendingError()));

    await waitFor(() => {
      expect(result.current.status).toBe('mfa-pending');
    });
    // Not an error to report and not a sign-out: the app sends this state to
    // the code prompt (#62).
    expect(result.current.error).toBeNull();
    expect(result.current.user).toBeNull();
  });

  it('drops the query cache when the session expires', async () => {
    const { result, queryClient } = renderSession(() =>
      Promise.resolve(currentUserWith(['sample.read'])),
    );
    await waitFor(() => {
      expect(result.current.status).toBe('authenticated');
    });
    seedCache(queryClient);

    act(() => {
      result.current.expire(
        new ApiError('UNAUTHENTICATED', 'session expired', { httpStatus: 401 }),
      );
    });

    await waitFor(() => {
      expect(result.current.status).toBe('unauthenticated');
    });
    expect(queryClient.getQueryCache().getAll()).toHaveLength(0);
  });

  it('returns to the code prompt when the refusal says the second factor is outstanding', async () => {
    const { result } = renderSession(() => Promise.resolve(currentUserWith(['sample.read'])));
    await waitFor(() => {
      expect(result.current.status).toBe('authenticated');
    });

    act(() => {
      result.current.expire(
        new ApiError('UNAUTHENTICATED', `${MFA_REQUIRED_PREFIX} MFA required`, { httpStatus: 401 }),
      );
    });

    // Same status as an expiry, different screen — this is the only signal that
    // separates the two (`GrpcErrorTranslation.h`).
    await waitFor(() => {
      expect(result.current.status).toBe('mfa-pending');
    });
  });

  it('revokes the session on the server and clears the cache when signing out', async () => {
    let logoutCalls = 0;
    server.use(
      http.post('/api/v1/auth/browser/logout', () => {
        logoutCalls += 1;
        return HttpResponse.json({});
      }),
    );
    const { result, queryClient } = renderSession(() =>
      Promise.resolve(currentUserWith(['sample.read'])),
    );
    await waitFor(() => {
      expect(result.current.status).toBe('authenticated');
    });
    seedCache(queryClient);

    await act(async () => {
      await result.current.signOut();
    });

    expect(logoutCalls).toBe(1);
    expect(result.current.status).toBe('unauthenticated');
    expect(result.current.user).toBeNull();
    expect(queryClient.getQueryCache().getAll()).toHaveLength(0);
  });

  it('still ends the local session when the server cannot be told', async () => {
    server.use(...fakeApi({ fail: { 'auth/browser/logout': 'UNAVAILABLE' } }));
    const { result } = renderSession(() => Promise.resolve(currentUserWith(['sample.read'])));
    await waitFor(() => {
      expect(result.current.status).toBe('authenticated');
    });

    await act(async () => {
      await result.current.signOut();
    });

    // A network failure is not a reason to leave the user apparently signed in.
    expect(result.current.status).toBe('unauthenticated');
  });

  it('re-runs the loader on refresh and reports what it found', async () => {
    let answer: Awaited<ReturnType<SessionLoader>> = null;
    const { result } = renderSession(() => Promise.resolve(answer));
    await waitFor(() => {
      expect(result.current.status).toBe('unauthenticated');
    });

    answer = currentUserWith(['sample.read']);
    let reloaded: Awaited<ReturnType<typeof result.current.refresh>> | undefined;
    await act(async () => {
      reloaded = await result.current.refresh();
    });

    expect(reloaded?.status).toBe('authenticated');
    await waitFor(() => {
      expect(result.current.status).toBe('authenticated');
    });
  });
});
