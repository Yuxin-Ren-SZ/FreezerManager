// SPDX-License-Identifier: AGPL-3.0-or-later
import { HttpResponse, http } from 'msw';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { server } from '../test/server';
import { apiRoutes } from './routes';

/**
 * Overlapping browser auth operations (#204).
 *
 * The gateway answers `auth/browser/login` with a fresh `fmgr_session` cookie
 * and `auth/browser/logout` with a deletion cookie, and the browser applies
 * each when that response arrives. These tests hold an old logout's response,
 * complete a newer login, release the logout, and then ask `WhoAmI` with
 * whatever cookie the jar is left holding.
 *
 * `fakeApi()` cannot show this: it changes its session state when a request
 * arrives, which is the server's view. The jar below changes when a response
 * is delivered, which is the browser's, and a response whose request was
 * aborted is never delivered. Every value is synthetic.
 */

const OLD_SESSION = 'synthetic-session-old';

interface Held {
  readonly promise: Promise<void>;
  readonly release: () => void;
}

function hold(): Held {
  let resolveHeld: (() => void) | undefined;
  const promise = new Promise<void>((resolve) => {
    resolveHeld = resolve;
  });
  return { promise, release: () => resolveHeld?.() };
}

function browserCookieGateway() {
  let jar: string | null = OLD_SESSION;
  let minted = 0;
  const userBySession = new Map<string, string>([[OLD_SESSION, 'user-old']]);
  const revoked = new Set<string>();
  const received: string[] = [];
  const logoutHeld = hold();
  const state = { logoutWasAborted: false };

  server.use(
    http.post(apiRoutes['auth/browser/logout'].path, async ({ request }) => {
      received.push('logout');
      // The server revokes the session the request carried, on arrival.
      if (jar !== null) revoked.add(jar);
      await logoutHeld.promise;
      if (request.signal.aborted) {
        state.logoutWasAborted = true;
        return HttpResponse.error();
      }
      jar = null; // `Set-Cookie: fmgr_session=; Max-Age=0`
      return HttpResponse.json({});
    }),
    http.post(apiRoutes['auth/browser/login'].path, () => {
      received.push('login');
      minted += 1;
      const session = `synthetic-session-new-${String(minted)}`;
      userBySession.set(session, 'user-new');
      jar = session;
      return HttpResponse.json({ session_id: session, user_id: 'user-new', mfa_required: true });
    }),
    http.post(apiRoutes['auth/browser/submit-mfa'].path, () => {
      received.push('submit-mfa');
      return HttpResponse.json({});
    }),
    http.post(apiRoutes['auth/whoami'].path, () => {
      const user = jar === null || revoked.has(jar) ? undefined : userBySession.get(jar);
      if (user === undefined) {
        return HttpResponse.json(
          { code: 'UNAUTHENTICATED', message: 'no valid session' },
          { status: 401 },
        );
      }
      return HttpResponse.json({ user_id: user, email: 'synthetic@example.test' });
    }),
  );

  return { received, releaseLogout: logoutHeld.release, state };
}

/** Long enough for a request that is not being held back to reach its handler. */
const settle = () => new Promise((resolve) => setTimeout(resolve, 25));

/**
 * A minimal origin-wide `LockManager`: exclusive locks granted in request
 * order. Two tabs share one, exactly as two tabs share the browser's.
 */
function installSharedLockManager() {
  const requests: { name: string; mode: LockMode | undefined }[] = [];
  const tails = new Map<string, Promise<unknown>>();
  const locks = {
    request(name: string, options: LockOptions, callback: (lock: Lock | null) => unknown) {
      requests.push({ name, mode: options.mode });
      const previous = tails.get(name) ?? Promise.resolve();
      const granted = previous.then(() => callback({ name, mode: options.mode ?? 'exclusive' }));
      tails.set(
        name,
        granted.catch(() => undefined),
      );
      return granted;
    },
  };
  Object.defineProperty(navigator, 'locks', { value: locks, configurable: true });
  return { requests };
}

/** A tab: its own copy of every module, so only the shared lock can order it. */
async function openTab() {
  vi.resetModules();
  const auth = await import('./auth');
  const client = await import('./client');
  return { ...auth, call: client.call };
}

afterEach(() => {
  vi.useRealTimers();
  Reflect.deleteProperty(navigator, 'locks');
});

describe('browser auth operation ordering (#204)', () => {
  it('sends a login only after an earlier logout response in the same tab has landed', async () => {
    const gateway = browserCookieGateway();
    const tab = await openTab();

    const logout = tab.signOutOfBrowserSession();
    await vi.waitFor(() => {
      expect(gateway.received).toEqual(['logout']);
    });
    const login = tab.signIn('synthetic@example.test', 'synthetic-password');
    await settle();
    expect(gateway.received).toEqual(['logout']);

    gateway.releaseLogout();
    await logout;
    await login;

    expect(gateway.received).toEqual(['logout', 'login']);
    await expect(tab.call('auth/whoami')).resolves.toMatchObject({ userId: 'user-new' });
  });

  it('orders a logout in one tab before a login in another through the origin-wide lock', async () => {
    const lockManager = installSharedLockManager();
    const gateway = browserCookieGateway();
    const signingOut = await openTab();
    const signingIn = await openTab();

    const logout = signingOut.signOutOfBrowserSession();
    await vi.waitFor(() => {
      expect(gateway.received).toEqual(['logout']);
    });
    const login = signingIn.signIn('synthetic@example.test', 'synthetic-password');
    const mfa = signingIn.submitMfaCode('000000');
    await settle();
    expect(gateway.received).toEqual(['logout']);

    gateway.releaseLogout();
    await logout;
    await login;
    await mfa;

    expect(gateway.received).toEqual(['logout', 'login', 'submit-mfa']);
    expect(lockManager.requests).toHaveLength(3);
    expect(lockManager.requests).toEqual(
      Array.from({ length: 3 }, () => ({ name: 'fmgr-browser-auth-v1', mode: 'exclusive' })),
    );
    await expect(signingIn.call('auth/whoami')).resolves.toMatchObject({ userId: 'user-new' });
  });

  it('abandons a hung logout at its deadline so its late reply cannot expire a newer login', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'], shouldAdvanceTime: true });
    const gateway = browserCookieGateway();
    const tab = await openTab();

    const logout = tab.signOutOfBrowserSession();
    const logoutOutcome = logout.then(
      () => 'resolved',
      (error: unknown) => error,
    );
    await vi.waitFor(() => {
      expect(gateway.received).toEqual(['logout']);
    });
    const login = tab.signIn('synthetic@example.test', 'synthetic-password');

    await vi.advanceTimersByTimeAsync(tab.BROWSER_LOGOUT_TIMEOUT_MS);

    await expect(logoutOutcome).resolves.toMatchObject({ code: 'UNAVAILABLE' });
    await login;
    expect(gateway.received).toEqual(['logout', 'login']);

    // The old reply finally comes back; the browser no longer delivers it.
    gateway.releaseLogout();
    await vi.waitFor(() => {
      expect(gateway.state.logoutWasAborted).toBe(true);
    });
    await expect(tab.call('auth/whoami')).resolves.toMatchObject({ userId: 'user-new' });
  });

  it('lets the next login through after a logout that failed', async () => {
    const gateway = browserCookieGateway();
    server.use(
      http.post(apiRoutes['auth/browser/logout'].path, () =>
        HttpResponse.json({ code: 'UNAVAILABLE', message: 'synthetic outage' }, { status: 503 }),
      ),
    );
    const tab = await openTab();

    await expect(tab.signOutOfBrowserSession()).rejects.toMatchObject({ code: 'UNAVAILABLE' });
    await tab.signIn('synthetic@example.test', 'synthetic-password');

    expect(gateway.received).toEqual(['login']);
    await expect(tab.call('auth/whoami')).resolves.toMatchObject({ userId: 'user-new' });
  });
});
