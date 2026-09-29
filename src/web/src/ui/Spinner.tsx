// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import styles from './Spinner.module.css';
import { classNames } from './classNames';
import { VisuallyHidden } from './VisuallyHidden';

export type SpinnerSize = 'sm' | 'md' | 'lg';

export interface SpinnerProps {
  /** Overrides the translated "Loading…" announcement. */
  label?: string;
  size?: SpinnerSize;
  className?: string;
}

/**
 * An indeterminate progress indicator.
 *
 * It is a `role="status"` region rather than a bare `<span>` so that a screen
 * reader announces that something is happening; the visible part (the rotating
 * ring) is `aria-hidden` because it carries no information. The animation is
 * disabled under `prefers-reduced-motion` in the stylesheet.
 */
export function Spinner({ label, size = 'md', className }: SpinnerProps) {
  const { t } = useTranslation('ui');

  return (
    <span className={classNames(styles.root, styles[size], className)} role="status">
      <span className={styles.glyph} aria-hidden="true" />
      <VisuallyHidden>{label ?? t('spinner.loading')}</VisuallyHidden>
    </span>
  );
}
