// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { formatCustomFieldValue, parseCustomFields } from './customFields';

/**
 * `custom_fields_json` is a JSON *string* inside the JSON body (G-arch 4), and
 * it is the only place a sample's custom-field values live. It comes from the
 * server, so it can be anything at all — including a value shape this bundle
 * has never seen — and the screen must not throw on a row because of it.
 */

describe('parseCustomFields', () => {
  it('reads an object of values', () => {
    expect(parseCustomFields('{"concentration":"12.5","freeze_thaw_count":3}')).toEqual({
      concentration: '12.5',
      freeze_thaw_count: 3,
    });
  });

  it('treats the empty default as no values', () => {
    expect(parseCustomFields('')).toEqual({});
    expect(parseCustomFields('{}')).toEqual({});
  });

  it('returns no values rather than throwing on malformed JSON', () => {
    // One corrupt row must not blank the whole table.
    expect(parseCustomFields('{not json')).toEqual({});
  });

  it('returns no values when the JSON is not an object', () => {
    expect(parseCustomFields('"a string"')).toEqual({});
    expect(parseCustomFields('null')).toEqual({});
    expect(parseCustomFields('[1,2]')).toEqual({});
  });
});

describe('formatCustomFieldValue', () => {
  it('shows a string as it is', () => {
    expect(formatCustomFieldValue('Serum A')).toBe('Serum A');
  });

  it('shows numbers and booleans without inventing copy', () => {
    expect(formatCustomFieldValue(12.5)).toBe('12.5');
    expect(formatCustomFieldValue(3)).toBe('3');
    expect(formatCustomFieldValue(true)).toBe('true');
    expect(formatCustomFieldValue(false)).toBe('false');
  });

  it('is empty for a field the row does not carry', () => {
    // Absent is the normal case: a definition is lab-wide, a value is per row.
    expect(formatCustomFieldValue(undefined)).toBe('');
    expect(formatCustomFieldValue(null)).toBe('');
    expect(formatCustomFieldValue('')).toBe('');
  });

  it('serialises a structured value instead of showing [object Object]', () => {
    expect(formatCustomFieldValue({ a: 1 })).toBe('{"a":1}');
  });

  it('does not throw on a value the server invented', () => {
    expect(() => formatCustomFieldValue(Number.NaN)).not.toThrow();
  });
});
