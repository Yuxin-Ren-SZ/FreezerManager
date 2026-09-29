// SPDX-License-Identifier: AGPL-3.0-or-later
import type { CurrentUser } from './session';
import { ALL_PERMISSIONS } from './permissions';

/**
 * TODO(G1.2): replace with `auth/whoami`.
 *
 * G1.3 runs alongside G1.2 and TODO.md says explicitly that this task "can run
 * alongside G1.2, using a stubbed current user until G1.2's fakes land". This
 * is that stub, and it is deliberately one file with one export so that
 * deleting it is a one-line change in `App.tsx`.
 *
 * It is a *development* stub, not a fallback: nothing here authenticates
 * anybody, and the server still rejects every call the SPA makes without a
 * session cookie (G0.1). The permission list is the LabAdmin baseline so that
 * every screen in the route map is reachable while the shell is being built.
 */
export const STUB_CURRENT_USER: CurrentUser = {
  userId: 'stub-user',
  email: 'stub@example.invalid',
  displayName: 'Stub User',
  sessionId: 'stub-session',
  mfaComplete: true,
  // Far future, UTC micros as a string (G-arch 9).
  expiresAt: '4102444800000000',
  permissions: [],
  labs: [
    {
      labId: 'lab-1',
      labName: 'Demo Lab',
      roleId: 'role-labadmin',
      roleName: 'LabAdmin',
      permissions: [
        'sample.read',
        'sample.write',
        'sample.checkout',
        'sample.delete_soft',
        'box.configure',
        'freezer.configure',
        'custom_field.define',
        'item_type.define',
        'user.invite',
        'user.manage_roles',
        'audit.read',
        'audit.export',
        'share.request',
        'share.approve',
        'lab.configure',
      ],
      isPhiEnabled: false,
    },
  ],
};

/** The stub with every permission, for the dev server and for tests. */
export const STUB_CURRENT_USER_ALL_PERMISSIONS: CurrentUser = {
  ...STUB_CURRENT_USER,
  permissions: ALL_PERMISSIONS,
  labs: STUB_CURRENT_USER.labs.map((lab) => ({ ...lab, permissions: ALL_PERMISSIONS })),
};

/** Resolves the stub after a tick, so the shell exercises its loading state. */
export function stubSessionLoader(): Promise<CurrentUser> {
  return Promise.resolve(STUB_CURRENT_USER);
}
