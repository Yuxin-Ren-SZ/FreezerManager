// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import styles from './State.module.css';
import { classNames } from './classNames';

export interface EmptyStateProps {
  title: string;
  description?: string;
  /** Usually a `Button` that leads somewhere useful. */
  action?: ReactNode;
  /** Decorative; hidden from assistive technology. */
  icon?: ReactNode;
  className?: string;
}

/**
 * "There is nothing here, and that is fine."
 *
 * Distinct from `ErrorState` on purpose: an empty sample list and a failed
 * request look nothing alike and must not be mistaken for each other. The
 * heading is `h2` because a state panel always sits under the screen's `h1`.
 */
export function EmptyState({ title, description, action, icon, className }: EmptyStateProps) {
  return (
    <div className={classNames(styles.root, className)}>
      {icon ? (
        <span className={styles.icon} aria-hidden="true">
          {icon}
        </span>
      ) : null}
      <h2 className={styles.title}>{title}</h2>
      {description ? <p className={styles.description}>{description}</p> : null}
      {action ? <div className={styles.action}>{action}</div> : null}
    </div>
  );
}
