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
 * "Sign out" asks the session to end and returns to `/login`. G2.1 put both
 * halves where they belong: `signOut()` revokes the session server-side
 * (`auth/browser/logout`) and drops the TanStack Query cache — G-arch 7 — so
 * this menu only has to say that the user is done.
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
              // Fire-and-forget: revoking the session must not block the
              // navigation, and `signOut()` ends the local state even when the
              // server cannot be reached.
              void signOut();
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
