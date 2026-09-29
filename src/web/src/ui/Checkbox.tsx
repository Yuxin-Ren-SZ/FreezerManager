// SPDX-License-Identifier: AGPL-3.0-or-later
import { useId } from 'react';
import type { InputHTMLAttributes, ReactNode } from 'react';
import styles from './Checkbox.module.css';
import { classNames } from './classNames';

export interface CheckboxProps extends Omit<InputHTMLAttributes<HTMLInputElement>, 'id' | 'type'> {
  label: ReactNode;
  hint?: string;
  error?: string;
  id?: string;
}

/**
 * A checkbox with its label wrapped around the input.
 *
 * Wrapping rather than `htmlFor` means the hit target includes the label text,
 * which matters at 360 px, and it keeps the association correct even when the
 * label is rich content. The hint and the error are attached with
 * `aria-describedby` for the same reason as in `TextField`.
 */
export function Checkbox({ label, hint, error, id, className, ...rest }: CheckboxProps) {
  const generatedId = useId();
  const fieldId = id ?? generatedId;
  const describedByIds = [
    hint ? `${fieldId}-hint` : undefined,
    error ? `${fieldId}-error` : undefined,
  ]
    .filter((value): value is string => Boolean(value))
    .join(' ');

  return (
    <div className={classNames(styles.root, className)}>
      <label className={styles.label} htmlFor={fieldId}>
        <input
          {...rest}
          id={fieldId}
          type="checkbox"
          className={styles.input}
          aria-invalid={error ? true : undefined}
          aria-describedby={describedByIds.length > 0 ? describedByIds : undefined}
        />
        <span className={styles.text}>{label}</span>
      </label>
      {hint ? (
        <p className={styles.hint} id={`${fieldId}-hint`}>
          {hint}
        </p>
      ) : null}
      {error ? (
        <p className={styles.error} id={`${fieldId}-error`} role="alert">
          {error}
        </p>
      ) : null}
    </div>
  );
}
