// SPDX-License-Identifier: AGPL-3.0-or-later
import { QueryClientProvider, type QueryClient } from '@tanstack/react-query';
import { act, renderHook, waitFor } from '@testing-library/react';
import type { ReactNode } from 'react';
import { HttpResponse, http } from 'msw';
import { beforeEach, describe, expect, it, vi } from 'vitest';
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
  queryClient.setQueryData(['protected'], { sentinel: 'synthetic-protected-data' });
}

function signalFromAnotherTab(): void {
  const peer = new BroadcastChannel('fmgr-session-change-v1');
  peer.postMessage({ type: 'session-changed', id: `${String(Date.now())}-test-peer` });
  peer.close();
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

  it.each(['success', 'failure'])(
    'ends local state before a pending logout resolves with %s and ignores an old identity reply',
    async (outcome) => {
      let finishLogout: (() => void) | undefined;
      const logoutPending = new Promise<void>((resolve) => {
        finishLogout = resolve;
      });
      let logoutCalls = 0;
      server.use(
        http.post('/api/v1/auth/browser/logout', async () => {
          logoutCalls += 1;
          await logoutPending;
          return outcome === 'success' ? HttpResponse.json({}) : HttpResponse.error();
        }),
      );
      const user = currentUserWith(['sample.read']);
      let resolveOld: ((value: typeof user) => void) | undefined;
      const loader = vi
        .fn<SessionLoader>()
        .mockResolvedValueOnce(user)
        .mockImplementationOnce(
          () =>
            new Promise((resolve) => {
              resolveOld = resolve;
            }),
        );
      const { result, queryClient } = renderSession(loader);
      await waitFor(() => {
        expect(result.current.status).toBe('authenticated');
      });
      seedCache(queryClient);
      let refresh: Promise<unknown> | undefined;
      let logout: Promise<void> | undefined;
      act(() => {
        refresh = result.current.refresh();
        logout = result.current.signOut();
      });

      expect(result.current.status).toBe('unauthenticated');
      expect(result.current.user).toBeNull();
      expect(queryClient.getQueryCache().getAll()).toHaveLength(0);
      await waitFor(() => {
        expect(logoutCalls).toBe(1);
      });
      await act(async () => {
        resolveOld?.(user);
        await refresh;
      });
      expect(result.current.status).toBe('unauthenticated');
      await act(async () => {
        finishLogout?.();
        await logout;
      });
      expect(result.current.status).toBe('unauthenticated');
      expect(result.current.expired).toBe(false);
    },
  );

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

  it('clears protected data and hides the old identity before a cross-tab reload resolves', async () => {
    type UserResolver = (user: ReturnType<typeof currentUserWith> | null) => void;
    const resolvers: UserResolver[] = [];
    const makeLoader = (): SessionLoader => {
      let loads = 0;
      return () => {
        loads += 1;
        if (loads === 1) return Promise.resolve(currentUserWith(['sample.read', 'phi.read']));
        return new Promise((resolve) => {
          resolvers.push(resolve);
        });
      };
    };
    const firstTab = renderSession(makeLoader());
    const secondTab = renderSession(makeLoader());
    await waitFor(() => {
      expect(firstTab.result.current.status).toBe('authenticated');
    });
    await waitFor(() => {
      expect(secondTab.result.current.status).toBe('authenticated');
    });
    seedCache(firstTab.queryClient);
    seedCache(secondTab.queryClient);

    act(() => {
      signalFromAnotherTab();
    });

    await waitFor(() => {
      expect(firstTab.result.current.status).toBe('loading');
    });
    expect(secondTab.result.current.status).toBe('loading');
    expect(firstTab.result.current.user).toBeNull();
    expect(secondTab.result.current.user).toBeNull();
    expect(firstTab.queryClient.getQueryCache().getAll()).toHaveLength(0);
    expect(secondTab.queryClient.getQueryCache().getAll()).toHaveLength(0);
    await waitFor(() => {
      expect(resolvers).toHaveLength(2);
    });

    act(() => {
      resolvers[0]?.(currentUserWith(['sample.read']));
      resolvers[1]?.(currentUserWith(['sample.read']));
    });
    await waitFor(() => {
      expect(firstTab.result.current.status).toBe('authenticated');
    });
    expect(secondTab.result.current.status).toBe('authenticated');
    expect(firstTab.result.current.user?.permissions).toEqual([]);
    expect(firstTab.result.current.user?.labs[0]?.permissions).toEqual(['sample.read']);
    expect(secondTab.result.current.user?.labs[0]?.permissions).toEqual(['sample.read']);
  });

  it('ignores an old in-flight identity load after a cross-tab session change', async () => {
    let resolveOld: ((user: ReturnType<typeof currentUserWith> | null) => void) | undefined;
    let loads = 0;
    const loader: SessionLoader = () => {
      loads += 1;
      if (loads === 1) {
        return new Promise((resolve) => {
          resolveOld = resolve;
        });
      }
      return Promise.resolve(currentUserWith(['sample.read']));
    };
    const { result } = renderSession(loader);

    act(() => {
      signalFromAnotherTab();
    });

    await waitFor(() => {
      expect(result.current.status).toBe('authenticated');
    });
    act(() => {
      resolveOld?.(currentUserWith(['backup.run']));
    });
    await waitFor(() => {
      expect(result.current.user?.permissions).toEqual([]);
    });
    expect(result.current.user?.labs[0]?.permissions).toEqual(['sample.read']);
  });

  it('closes the cross-tab listener when the provider unmounts', async () => {
    const close = vi.fn();
    class TrackedBroadcastChannel {
      onmessage: ((event: MessageEvent<unknown>) => void) | null = null;

      constructor(readonly name: string) {}

      postMessage(): void {
        void 0;
      }

      close(): void {
        close();
      }
    }
    vi.stubGlobal('BroadcastChannel', TrackedBroadcastChannel);
    try {
      const rendered = renderSession(() => Promise.resolve(currentUserWith(['sample.read'])));
      await waitFor(() => {
        expect(rendered.result.current.status).toBe('authenticated');
      });

      rendered.unmount();

      expect(close).toHaveBeenCalledOnce();
    } finally {
      vi.unstubAllGlobals();
    }
  });

  it('uses the storage event fallback when BroadcastChannel is unavailable', async () => {
    vi.stubGlobal('BroadcastChannel', undefined);
    let resolveReload: ((user: ReturnType<typeof currentUserWith> | null) => void) | undefined;
    let loads = 0;
    const loader: SessionLoader = () => {
      loads += 1;
      if (loads === 1) return Promise.resolve(currentUserWith(['phi.read']));
      return new Promise((resolve) => {
        resolveReload = resolve;
      });
    };
    try {
      const rendered = renderSession(loader);
      await waitFor(() => {
        expect(rendered.result.current.status).toBe('authenticated');
      });
      seedCache(rendered.queryClient);

      window.dispatchEvent(
        new StorageEvent('storage', { key: 'fmgr-session-change-v1', newValue: 'peer-signal' }),
      );

      await waitFor(() => {
        expect(rendered.result.current.status).toBe('loading');
      });
      expect(rendered.queryClient.getQueryCache().getAll()).toHaveLength(0);
      act(() => {
        resolveReload?.(null);
      });
      await waitFor(() => {
        expect(rendered.result.current.status).toBe('unauthenticated');
      });
      rendered.unmount();
    } finally {
      vi.unstubAllGlobals();
    }
  });
});
