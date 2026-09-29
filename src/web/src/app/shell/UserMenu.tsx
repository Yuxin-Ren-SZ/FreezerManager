// SPDX-License-Identifier: AGPL-3.0-or-later
import * as DropdownMenu from '@radix-ui/react-dropdown-menu';
import { useTranslation } from 'react-i18next';
import { Link, useNavigate } from 'react-router-dom';
import { Button } from '../../ui';
import { useSession } from '../session';
import styles from './TopBar.module.css';

/** Where sign-out lands; G2.1 owns what `/login` does with the redirect. */
const LOGIN_PATH = '/login';

/**
 * The account menu.
 *
 * Radix's dropdown rather than a hand-rolled one: roving focus, type-ahead,
 * Escape-to-close and returning focus to the trigger are all easy to get
 * subtly wrong.
 *
 * "Sign out" clears the local session and returns to `/login`. G2.1 replaces
 * the body with `auth/browser/logout` (which revokes the session server-side)
 * plus a TanStack Query cache reset — G-arch 7 requires the cache to be dropped
 * on logout, and that reset belongs with the auth flow, not with a menu.
 */
export function UserMenu() {
  const { t } = useTranslation('shell');
  const { user, signOut } = useSession();
  const navigate = useNavigate();

  if (user === null) {
    return null;
  }

  return (
    <DropdownMenu.Root>
      <DropdownMenu.Trigger asChild>
        <Button variant="ghost" aria-haspopup="menu">
          {user.displayName}
        </Button>
      </DropdownMenu.Trigger>
      <DropdownMenu.Portal>
        <DropdownMenu.Content className={styles.menu} align="end" sideOffset={4}>
          <DropdownMenu.Label className={styles.menuLabel}>
            {t('userMenu.signedInAs', { email: user.email })}
          </DropdownMenu.Label>
          <DropdownMenu.Separator className={styles.menuSeparator} />
          <DropdownMenu.Item asChild>
            <Link to="/account" className={styles.menuItem}>
              {t('userMenu.account')}
            </Link>
          </DropdownMenu.Item>
          <DropdownMenu.Item
            className={styles.menuItem}
            onSelect={() => {
              signOut();
              void navigate(LOGIN_PATH);
            }}
          >
            {t('userMenu.signOut')}
          </DropdownMenu.Item>
        </DropdownMenu.Content>
      </DropdownMenu.Portal>
    </DropdownMenu.Root>
  );
}
