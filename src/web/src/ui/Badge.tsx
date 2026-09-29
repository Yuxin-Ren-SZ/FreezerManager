// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import styles from './Badge.module.css';
import { classNames } from './classNames';

export type BadgeTone = 'neutral' | 'info' | 'success' | 'warning' | 'danger';

export interface BadgeProps {
  tone?: BadgeTone;
  children: ReactNode;
  className?: string;
}

/**
 * A short status label — sample state, box occupancy, connection health.
 *
 * Deliberately not a `role="status"`: a badge usually re-renders on every poll,
 * and a live region would read the whole grid aloud on each refresh. The text
 * is in the accessibility tree as ordinary text; the colour is decoration, so
 * the label must always say what the colour says.
 */
export function Badge({ tone = 'neutral', children, className }: BadgeProps) {
  return <span className={classNames(styles.root, styles[tone], className)}>{children}</span>;
}
