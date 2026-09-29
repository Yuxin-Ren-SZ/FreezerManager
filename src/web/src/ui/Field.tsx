// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import styles from './Field.module.css';

/**
 * Shared wiring for the labelled form controls (TextField, Select, Checkbox).
 *
 * `aria-describedby` is built here so that every control points at its hint and
 * its error in the same order, and so that neither is ever rendered without
 * being announced.
 */
export function describedBy(id: string, { hint, error }: { hint?: string; error?: string }) {
  const ids = [hint ? `${id}-hint` : undefined, error ? `${id}-error` : undefined].filter(
    (value): value is string => Boolean(value),
  );
  return ids.length > 0 ? ids.join(' ') : undefined;
}

export interface FieldMessagesProps {
  id: string;
  hint?: string;
  error?: string;
}

export function FieldMessages({ id, hint, error }: FieldMessagesProps) {
  return (
    <>
      {hint ? (
        <p className={styles.hint} id={`${id}-hint`}>
          {hint}
        </p>
      ) : null}
      {error ? (
        // `role="alert"` rather than a bare paragraph: an error that appears
        // only as red text after a failed submit is never read out.
        <p className={styles.error} id={`${id}-error`} role="alert">
          {error}
        </p>
      ) : null}
    </>
  );
}

export function FieldLabel({
  htmlFor,
  children,
  required,
}: {
  htmlFor: string;
  children: ReactNode;
  required?: boolean;
}) {
  return (
    <label className={styles.label} htmlFor={htmlFor}>
      {children}
      {required ? (
        <span className={styles.required} aria-hidden="true">
          *
        </span>
      ) : null}
    </label>
  );
}
