// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ButtonHTMLAttributes, ReactNode } from 'react';
import { Spinner } from './Spinner';
import styles from './Button.module.css';
import { classNames } from './classNames';

export type ButtonVariant = 'primary' | 'secondary' | 'danger' | 'ghost';
export type ButtonSize = 'sm' | 'md' | 'lg';

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  size?: ButtonSize;
  /** Shows a spinner, disables the button and marks it `aria-busy`. */
  loading?: boolean;
  /** Stretches the button across its container. */
  block?: boolean;
  children: ReactNode;
}

/**
 * The one button in the kit.
 *
 * `type` defaults to `"button"`: an HTML `<button>` inside a form defaults to
 * `"submit"`, which turns "Cancel" into "save anyway".
 *
 * `loading` disables the control rather than merely decorating it, so a second
 * click cannot fire the mutation twice; the label stays visible (the spinner is
 * additive) because a button that empties itself is a layout jump.
 */
export function Button({
  variant = 'secondary',
  size = 'md',
  loading = false,
  block = false,
  type = 'button',
  disabled,
  className,
  children,
  ...rest
}: ButtonProps) {
  return (
    <button
      {...rest}
      type={type}
      disabled={disabled ?? loading}
      aria-busy={loading ? true : undefined}
      className={classNames(
        styles.root,
        styles[variant],
        styles[size],
        block && styles.block,
        className,
      )}
    >
      {loading ? <Spinner size="sm" className={styles.spinner} /> : null}
      <span className={styles.label}>{children}</span>
    </button>
  );
}
