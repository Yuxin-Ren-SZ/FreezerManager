// SPDX-License-Identifier: AGPL-3.0-or-later
import { useId } from 'react';
import type { SelectHTMLAttributes } from 'react';
import { FieldLabel, FieldMessages, describedBy } from './Field';
import { VisuallyHidden } from './VisuallyHidden';
import styles from './Select.module.css';
import { classNames } from './classNames';

export interface SelectOption {
  value: string;
  label: string;
  disabled?: boolean;
}

export interface SelectProps extends Omit<
  SelectHTMLAttributes<HTMLSelectElement>,
  'id' | 'children'
> {
  label: string;
  options: readonly SelectOption[];
  /**
   * Rendered as a leading option with an empty value. Use it when "nothing
   * chosen yet" is a real state; omit it when the first option is the default.
   */
  placeholder?: string;
  hint?: string;
  error?: string;
  id?: string;
  /**
   * Renders the label for assistive technology only. For dense toolbars (the
   * app shell's lab picker) where the control's meaning is obvious visually but
   * an unlabelled `<select>` would be announced as "combobox".
   */
  labelHidden?: boolean;
}

/**
 * A native `<select>`.
 *
 * G-arch 1 allows a Radix primitive "only where accessibility is hard", and a
 * listbox is the one widget the platform already gets right: native selects
 * have working type-ahead, mobile pickers and screen-reader support that a
 * custom listbox has to re-earn. `@radix-ui/react-select` is not in the
 * dependency budget, so this is also the honest choice.
 */
export function Select({
  label,
  options,
  placeholder,
  hint,
  error,
  id,
  labelHidden = false,
  className,
  ...rest
}: SelectProps) {
  const generatedId = useId();
  const fieldId = id ?? generatedId;

  return (
    <div className={classNames(styles.root, className)}>
      {labelHidden ? (
        <VisuallyHidden>
          <FieldLabel htmlFor={fieldId} required={rest.required}>
            {label}
          </FieldLabel>
        </VisuallyHidden>
      ) : (
        <FieldLabel htmlFor={fieldId} required={rest.required}>
          {label}
        </FieldLabel>
      )}
      <select
        {...rest}
        id={fieldId}
        className={styles.select}
        aria-invalid={error ? true : undefined}
        aria-describedby={describedBy(fieldId, { hint, error })}
      >
        {placeholder !== undefined ? <option value="">{placeholder}</option> : null}
        {options.map((option) => (
          <option key={option.value} value={option.value} disabled={option.disabled}>
            {option.label}
          </option>
        ))}
      </select>
      <FieldMessages id={fieldId} hint={hint} error={error} />
    </div>
  );
}
