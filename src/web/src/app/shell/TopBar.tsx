// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { ConnectionIndicator } from './ConnectionIndicator';
import { GlobalLookup } from './GlobalLookup';
import { LabPicker } from './LabPicker';
import { UserMenu } from './UserMenu';
import styles from './TopBar.module.css';

/**
 * The top bar: brand, the global lookup box, and the three stateful controls the
 * G1.3 acceptance criteria name — lab picker, user menu, live-connection
 * indicator.
 *
 * It wraps rather than scrolls at narrow widths, so at 360 px the lookup box
 * takes a row of its own instead of pushing the account menu off-screen.
 */
export function TopBar() {
  const { t } = useTranslation('shell');

  return (
    <header className={styles.topBar}>
      <Link to="/" className={styles.brand}>
        {t('app.name')}
      </Link>
      <GlobalLookup />
      <div className={styles.actions}>
        <ConnectionIndicator />
        <LabPicker />
        <UserMenu />
      </div>
    </header>
  );
}
