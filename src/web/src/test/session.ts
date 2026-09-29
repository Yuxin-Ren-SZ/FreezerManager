// SPDX-License-Identifier: AGPL-3.0-or-later
import type { CurrentUser, LabMembership } from '../app/session';
import { ALL_PERMISSIONS, type PermissionKey } from '../app/permissions';

/**
 * A `WhoAmI`-shaped user for screen tests (TODO.md G3.3).
 *
 * Screens gate their affordances on the session's permissions (G-arch 8), so a
 * test that wants "a ReadOnly member" or "a caller who cannot read the field
 * definitions" — `sample.read` since #69, not `custom_field.define` — needs a
 * real `CurrentUser`, not a mock: `can()` reads `permissions` and `labs`.
 */

export const DEMO_LAB_ID = 'lab-demo';

export interface CurrentUserOptions {
  readonly labId?: string;
  /** Deployment-wide grants, outside any lab membership. */
  readonly globalPermissions?: readonly PermissionKey[];
  /** Labs the user is a member of; defaults to one membership in `labId`. */
  readonly labs?: readonly string[];
}

export function currentUserWith(
  permissions: readonly PermissionKey[],
  options: CurrentUserOptions = {},
): CurrentUser {
  const labId = options.labId ?? DEMO_LAB_ID;
  const labIds = options.labs ?? [labId];
  const membership = (id: string): LabMembership => ({
    labId: id,
    labName: id,
    roleId: 'role-test',
    roleName: 'Test',
    permissions,
    isPhiEnabled: true,
  });

  return {
    userId: 'user-1',
    email: 'user@example.test',
    displayName: 'Test User',
    sessionId: 'session-1',
    mfaComplete: true,
    expiresAt: '4102444800000000',
    permissions: options.globalPermissions ?? [],
    labs: labIds.map(membership),
  };
}

/** A user with every permission, for the happy paths. */
export function allPermissionsUser(): CurrentUser {
  return currentUserWith(ALL_PERMISSIONS);
}
