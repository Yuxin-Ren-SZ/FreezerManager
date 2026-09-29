// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import styles from './VisuallyHidden.module.css';

/**
 * Renders text for screen readers only.
 *
 * `clip-path: inset(50%)` in the stylesheet rather than `display: none` or
 * `visibility: hidden`, both of which remove the text from the accessibility
 * tree as well as from the screen.
 */
export function VisuallyHidden({ children }: { children: ReactNode }) {
  return <span className={styles.root}>{children}</span>;
}
