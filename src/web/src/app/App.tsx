// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import styles from './App.module.css';

/**
 * The one page G1.1 ships, so that `npm run check` proves the whole toolchain
 * works end to end (i18n resources, CSS Modules, design tokens, React).
 *
 * G1.3 replaces it with the real app shell, the router and the placeholder
 * routes from the route map in TODO.md §Section G; feature screens then live in
 * `src/features/<name>/`.
 */
export function App() {
  const { t } = useTranslation();

  return (
    <main className={styles.page}>
      <h1 className={styles.title}>{t('app.scaffold.title')}</h1>
      <p className={styles.body}>{t('app.scaffold.body')}</p>
    </main>
  );
}
