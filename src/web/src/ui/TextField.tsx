// SPDX-License-Identifier: AGPL-3.0-or-later
import { useId } from 'react';
import type { InputHTMLAttributes } from 'react';
import { FieldLabel, FieldMessages, describedBy } from './Field';
import styles from './TextField.module.css';
import { classNames } from './classNames';

export interface TextFieldProps extends Omit<InputHTMLAttributes<HTMLInputElement>, 'id'> {
  label: string;
  /** Guidance shown under the field and included in its accessible description. */
  hint?: string;
  /** Validation message; also sets `aria-invalid`. */
  error?: string;
  /** Overrides the generated id, for a `<label for>` elsewhere. */
  id?: string;
}

export function TextField({ label, hint, error, id, className, ...rest }: TextFieldProps) {
  const generatedId = useId();
  const fieldId = id ?? generatedId;

  return (
    <div className={classNames(styles.root, className)}>
      <FieldLabel htmlFor={fieldId} required={rest.required}>
        {label}
      </FieldLabel>
      <input
        {...rest}
        id={fieldId}
        className={styles.input}
        aria-invalid={error ? true : undefined}
        aria-describedby={describedBy(fieldId, { hint, error })}
      />
      <FieldMessages id={fieldId} hint={hint} error={error} />
    </div>
  );
}
