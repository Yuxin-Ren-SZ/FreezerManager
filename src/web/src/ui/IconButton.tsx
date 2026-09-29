// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ButtonHTMLAttributes, ReactNode } from 'react';
import styles from './IconButton.module.css';
import { classNames } from './classNames';

export type IconButtonVariant = 'ghost' | 'secondary' | 'danger';
export type IconButtonSize = 'sm' | 'md';

export interface IconButtonProps extends Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'children'> {
  /**
   * The accessible name. Required, and not optional: an icon-only button
   * without a name is invisible to a screen reader, and making it required
   * turns that into a type error instead of a review comment.
   */
  label: string;
  variant?: IconButtonVariant;
  size?: IconButtonSize;
  children: ReactNode;
}

/**
 * A square, icon-only button. The glyph is `aria-hidden`; `label` is the name.
 */
export function IconButton({
  label,
  variant = 'ghost',
  size = 'md',
  type = 'button',
  className,
  children,
  ...rest
}: IconButtonProps) {
  return (
    <button
      {...rest}
      type={type}
      aria-label={label}
      title={rest.title ?? label}
      className={classNames(styles.root, styles[variant], styles[size], className)}
    >
      <span aria-hidden="true">{children}</span>
    </button>
  );
}
