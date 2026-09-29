// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import styles from './Kbd.module.css';
import { classNames } from './classNames';

export interface KbdProps {
  children: ReactNode;
  className?: string;
}

/**
 * A keyboard key, as `<kbd>` so that the shortcut is machine-readable rather
 * than just a styled word.
 */
export function Kbd({ children, className }: KbdProps) {
  return <kbd className={classNames(styles.root, className)}>{children}</kbd>;
}
