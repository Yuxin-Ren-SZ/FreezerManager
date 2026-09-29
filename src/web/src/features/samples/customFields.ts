// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * The custom-field half of a sample row (TODO.md G3.2).
 *
 * Custom fields are a JSON string on the wire and a *definition* list from
 * `custom-field-def/list`; the table joins them by `key`. Nothing here knows
 * about React, so the join is testable on its own.
 */

/** A `custom_fields_json` body as a map. Malformed or non-object JSON is empty. */
export function parseCustomFields(json: string): Readonly<Record<string, unknown>> {
  if (json === '') {
    return {};
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(json);
  } catch {
    return {};
  }
  if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return {};
  }
  return parsed as Record<string, unknown>;
}

/**
 * One value as text, or `''` when the row does not carry it.
 *
 * Deliberately no translation and no formatting by data type: the value is
 * data, not UI copy, and a PHI field is either present (the server disclosed
 * it) or absent, never guessed at.
 */
export function formatCustomFieldValue(value: unknown): string {
  if (value === undefined || value === null || value === '') {
    return '';
  }
  if (typeof value === 'string') {
    return value;
  }
  if (typeof value === 'number' || typeof value === 'boolean' || typeof value === 'bigint') {
    return String(value);
  }
  try {
    return JSON.stringify(value);
  } catch {
    // A cyclic or otherwise unserialisable value: an empty cell beats a row
    // that throws while rendering.
    return '';
  }
}
