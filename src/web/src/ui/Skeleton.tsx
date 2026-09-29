// SPDX-License-Identifier: AGPL-3.0-or-later
import styles from './Skeleton.module.css';
import { classNames } from './classNames';

export interface SkeletonProps {
  /** Any CSS length; defaults to the stylesheet's `width: 100%`. */
  width?: string;
  height?: string;
  className?: string;
}

/**
 * A placeholder block for content that is still loading.
 *
 * `aria-hidden`, because a skeleton is a visual stand-in for something a screen
 * reader should be told about once, in words. The region that owns it sets
 * `aria-busy` and carries the announcement (see `Spinner`, or `EmptyState` for
 * the settled state).
 */
export function Skeleton({ width, height, className }: SkeletonProps) {
  return (
    <span
      className={classNames(styles.root, className)}
      style={{ width, height }}
      aria-hidden="true"
    />
  );
}
