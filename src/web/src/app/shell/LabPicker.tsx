// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { useLabs } from '../labs';
import { Select } from '../../ui';
import styles from './TopBar.module.css';

/**
 * Which lab the shell is scoped to.
 *
 * Every lab-scoped route carries `:labId` in its own path, so this picker is
 * what the *nav* builds its links from. It stays a plain `<select>`: with a
 * handful of memberships, a listbox in a popover is more machinery than the
 * choice deserves.
 */
export function LabPicker() {
  const { t } = useTranslation('shell');
  const { labs, selectedLabId, selectLab } = useLabs();

  if (labs.length === 0 || selectedLabId === null) {
    return <span className={styles.noLab}>{t('labPicker.label')}&nbsp;—</span>;
  }

  return (
    <Select
      label={t('labPicker.label')}
      labelHidden
      className={styles.labPicker}
      value={selectedLabId}
      options={labs.map((lab) => ({ value: lab.labId, label: lab.labName }))}
      onChange={(event) => {
        selectLab(event.target.value);
      }}
    />
  );
}
