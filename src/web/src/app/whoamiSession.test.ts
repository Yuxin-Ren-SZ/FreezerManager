// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it, vi } from 'vitest';
import { ApiError, MFA_REQUIRED_PREFIX } from '../api/errors';
import { MfaPendingError, type CurrentUser } from './session';
import { createWhoAmISessionLoader, toCurrentUser, type WhoAmIWire } from './whoamiSession';

/**
 * The SPA's half of the `WhoAmI` contract (G0.2). The mapping is small, which is
 * exactly why the three *answers* it has to keep apart are worth pinning: a user,
 * "no session" and "a session with its second factor outstanding" all arrive
 * from the same RPC and only the last two are errors.
 */
const RESPONSE: WhoAmIWire = {
  userId: 'user-1',
  email: 'ada@example.test',
  displayName: 'Ada Lovelace',
  isSystemAdmin: true,
  permissions: ['backup.run'],
  labs: [
    {
      labId: 'lab-1',
      labName: 'Demo Lab',
      roleId: 'role-labadmin',
      roleName: 'LabAdmin',
      permissions: ['sample.read', 'sample.write'],
      isPhiEnabled: true,
    },
    {
      labId: 'lab-2',
      labName: 'Second Lab',
      roleId: 'role-readonly',
      roleName: 'ReadOnly',
      permissions: ['sample.read'],
    },
  ],
};

describe('toCurrentUser', () => {
  it('maps the identity and every membership, in order', () => {
    const user = toCurrentUser(RESPONSE);

    expect(user.userId).toBe('user-1');
    expect(user.email).toBe('ada@example.test');
    expect(user.displayName).toBe('Ada Lovelace');
    expect(user.isSystemAdmin).toBe(true);
    expect(user.permissions).toEqual(['backup.run']);
    expect(user.labs.map((lab) => lab.labId)).toEqual(['lab-1', 'lab-2']);
    expect(user.labs[0]?.roleName).toBe('LabAdmin');
  });

  it('drops a permission this bundle does not know, instead of widening the type', () => {
    const [firstLab] = RESPONSE.labs ?? [];
    const user = toCurrentUser({
      ...RESPONSE,
      permissions: ['backup.run', 'sample.reed', 'from-a-newer-server'],
      labs: [
        { ...firstLab, permissions: ['sample.read', 'not.a.permission'] },
      ] as WhoAmIWire['labs'],
    });

    expect(user.permissions).toEqual(['backup.run']);
    expect(user.labs[0]?.permissions).toEqual(['sample.read']);
  });

  it('fails closed on a missing PHI flag and a missing lab list', () => {
    const user = toCurrentUser({
      userId: 'u',
      email: 'e@example.test',
      displayName: 'No Labs',
    });

    expect(user.labs).toEqual([]);
    expect(user.isSystemAdmin).toBe(false);
    expect(toCurrentUser(RESPONSE).labs[1]?.isPhiEnabled).toBe(false);
  });

  it('never carries a session id, token or expiry', () => {
    const user: CurrentUser = toCurrentUser(RESPONSE);

    // AGENTS.md §5 / G-arch 6: the session is the HttpOnly cookie. A field that
    // could hold a credential must not exist on the type at all.
    expect(Object.keys(user).sort()).toEqual([
      'displayName',
      'email',
      'isSystemAdmin',
      'labs',
      'permissions',
      'userId',
    ]);
  });
});

describe('createWhoAmISessionLoader', () => {
  it('resolves the user a valid session describes', async () => {
    const load = createWhoAmISessionLoader(() => Promise.resolve(RESPONSE));

    await expect(load()).resolves.toMatchObject({ userId: 'user-1' });
  });

  it('resolves null for a plain UNAUTHENTICATED, which is a signed-out visitor', async () => {
    const load = createWhoAmISessionLoader(() =>
      Promise.reject(new ApiError('UNAUTHENTICATED', 'invalid credentials', { httpStatus: 401 })),
    );

    await expect(load()).resolves.toBeNull();
  });

  it('rejects with MfaPendingError when the session still owes its second factor', async () => {
    const load = createWhoAmISessionLoader(() =>
      Promise.reject(
        new ApiError(
          'UNAUTHENTICATED',
          `${MFA_REQUIRED_PREFIX} MFA required before this operation`,
          {
            httpStatus: 401,
          },
        ),
      ),
    );

    // Same status as the case above, different state, different screen: this is
    // the distinction that keeps a half-finished login resumable (#62).
    await expect(load()).rejects.toBeInstanceOf(MfaPendingError);
  });

  it('propagates anything else, so a dropped connection is not a sign-out', async () => {
    const load = createWhoAmISessionLoader(() =>
      Promise.reject(new ApiError('UNAVAILABLE', 'the server could not be reached')),
    );

    const error = await load().catch((caught: unknown) => caught);
    expect(error).toBeInstanceOf(ApiError);
    expect((error as ApiError).code).toBe('UNAVAILABLE');
  });

  it('asks the server once per call and reports what it said', async () => {
    const fetchWhoAmI = vi.fn(() => Promise.resolve(RESPONSE));
    const load = createWhoAmISessionLoader(fetchWhoAmI);

    await load();
    await load();

    expect(fetchWhoAmI).toHaveBeenCalledTimes(2);
  });
});
