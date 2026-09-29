// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { ErrorState } from '../../ui';
import type { PermissionKey } from '../permissions';

export interface NoAccessProps {
  /** The permission(s) the screen wanted, for the "ask an admin" message. */
  permissions?: readonly PermissionKey[];
  /** `true` when the user belongs to no lab at all, which is a different fix. */
  noLab?: boolean;
  /** `true` when the user is a member of other labs but not of this one. */
  notAMember?: boolean;
}

/**
 * "You are signed in, and this screen is not for you."
 *
 * Deliberately *not* a 403 or a redirect: the user is authenticated and the nav
 * should stay usable. G-arch 8 — this is UX, not security; the server refuses
 * the underlying calls regardless, and every feature screen still handles
 * `PERMISSION_DENIED` because a permission can be revoked mid-session.
 */
export function NoAccess({ permissions, noLab = false, notAMember = false }: NoAccessProps) {
  const { t } = useTranslation('shell');

  return (
    <ErrorState
      title={t('noAccess.title')}
      description={
        noLab
          ? t('noAccess.noLab')
          : notAMember
            ? t('noAccess.notAMember')
            : t('noAccess.body', { permission: permissions?.join(' or ') })
      }
    />
  );
}
