// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import styles from './PlaceholderScreen.module.css';

/**
 * G-arch 11: every screen in the route map gets a route, a nav entry and a file
 * in its own `features/<name>/` directory before it is built, so a feature task
 * edits one directory and never the shell.
 *
 * This is what those files render until then. The heading is `h1` because a
 * screen owns exactly one, and it comes from the feature's own i18next
 * namespace — the same key the real screen will keep using for its title.
 */
export interface PlaceholderScreenProps {
  /** The feature's i18next namespace, e.g. `samples`. */
  namespace: FeatureNamespace;
  /** The TODO.md id that replaces this screen, e.g. `G3.2`. */
  task: string;
}

export type FeatureNamespace =
  | 'account'
  | 'audit'
  | 'auth'
  | 'csvImport'
  | 'home'
  | 'itemTypes'
  | 'layout'
  | 'lookup'
  | 'members'
  | 'samples'
  | 'scan'
  | 'shares';

export function PlaceholderScreen({ namespace, task }: PlaceholderScreenProps) {
  const { t } = useTranslation(namespace);

  return (
    <section className={styles.root}>
      <h1 className={styles.title}>{t('title')}</h1>
      <p className={styles.body}>{t('placeholder', { task })}</p>
    </section>
  );
}
