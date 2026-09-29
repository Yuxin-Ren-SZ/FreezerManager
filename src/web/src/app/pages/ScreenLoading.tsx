// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { Spinner } from '../../ui';
import styles from './ScreenLoading.module.css';

/**
 * What fills the content area while a screen's chunk is on its way (issue #64).
 *
 * It renders *inside* the shell: `AppShell`, the top bar and the nav are eager
 * and paint first, so this only ever stands in for the screen the user asked
 * for. Deferring the frame itself would trade a spinner for the layout shift it
 * is there to avoid.
 */
export function ScreenLoading() {
  const { t } = useTranslation('shell');

  return (
    <div className={styles.centred}>
      <Spinner size="lg" label={t('screen.loading')} />
    </div>
  );
}
