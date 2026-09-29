// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { Outlet } from 'react-router-dom';
import { SideNav } from './SideNav';
import { TopBar } from './TopBar';
import styles from './AppShell.module.css';

/** The `id` the skip link targets; also the router's scroll anchor. */
export const MAIN_CONTENT_ID = 'fmgr-main-content';

/**
 * The frame every signed-in screen renders inside: top bar, side nav, and the
 * one `<main>` landmark.
 *
 * The skip link is first in the DOM on purpose — it is the only way to get past
 * a nav that is 12 entries long without tabbing through all of it.
 */
export function AppShell() {
  const { t } = useTranslation('shell');

  return (
    <div className={styles.shell}>
      <a className={styles.skipLink} href={`#${MAIN_CONTENT_ID}`}>
        {t('skipToContent')}
      </a>
      <TopBar />
      <div className={styles.body}>
        <SideNav />
        <main id={MAIN_CONTENT_ID} className={styles.main} tabIndex={-1}>
          <Outlet />
        </main>
      </div>
    </div>
  );
}
