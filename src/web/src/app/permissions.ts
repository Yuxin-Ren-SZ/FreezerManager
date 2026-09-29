// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * The permission keys the SPA knows how to reason about.
 *
 * This mirrors `src/core/permissions.h`, which is the source of truth: the C++
 * catalog is what the server enforces and what `AuthService.WhoAmI` (G0.2)
 * returns. Keeping a mirror here rather than `string` is what makes a typo in a
 * route guard — `can('sample.reed')` — a compile error instead of a screen
 * that is silently never reachable.
 *
 * G-arch 8: this is UX only. Every screen still handles `PERMISSION_DENIED`,
 * because the server is the only enforcement point.
 */
export type PermissionKey =
  | 'sample.read'
  | 'sample.write'
  | 'sample.checkout'
  | 'sample.delete_soft'
  | 'sample.delete_hard'
  | 'box.configure'
  | 'freezer.configure'
  | 'custom_field.define'
  | 'item_type.define'
  | 'user.invite'
  | 'user.manage_roles'
  | 'audit.read'
  | 'audit.export'
  | 'backup.run'
  | 'share.request'
  | 'share.approve'
  | 'phi.read'
  | 'lab.configure'
  | 'lab.enable_phi'
  | 'key.rotate'
  | 'session.revoke'
  | 'lab.provision';

/** Everything `WhoAmI` can report, for tests and for the dev stub. */
export const ALL_PERMISSIONS: readonly PermissionKey[] = [
  'sample.read',
  'sample.write',
  'sample.checkout',
  'sample.delete_soft',
  'sample.delete_hard',
  'box.configure',
  'freezer.configure',
  'custom_field.define',
  'item_type.define',
  'user.invite',
  'user.manage_roles',
  'audit.read',
  'audit.export',
  'backup.run',
  'share.request',
  'share.approve',
  'phi.read',
  'lab.configure',
  'lab.enable_phi',
  'key.rotate',
  'session.revoke',
  'lab.provision',
];
