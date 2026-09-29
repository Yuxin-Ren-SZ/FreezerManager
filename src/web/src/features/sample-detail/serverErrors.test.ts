// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { ApiError } from '../../api/errors';
import { mapServerFailure } from './serverErrors';

/**
 * Where a server rejection lands in the sample form (TODO.md G3.3).
 *
 * The server decides; the client's job is to put the decision on the offending
 * field instead of in a banner that makes the user hunt for it. The status codes
 * and messages below are copied from the C++ that produces them:
 *
 *  - `prepare_custom_fields()` → `custom field validation failed: [key: message] …`
 *    (`SampleServiceImpl.cc`), rendered from `core::validate_custom_fields`;
 *  - `samples_position_unique` → `ALREADY_EXISTS`
 *    (`GrpcErrorTranslation.h`: `UniqueViolation` → `ALREADY_EXISTS`);
 *  - `container_type size_class is not accepted at this box position`
 *    (`validate_sample()` in both backends) → `INVALID_ARGUMENT`.
 *
 * A field name is either a core field of the sample message (`positionLabel`,
 * `containerTypeId`, …) or a custom-field key, which is what that key is called
 * in `custom_fields_json`.
 */

const failure = (code: ApiError['code'], message: string) => new ApiError(code, message);

describe('mapServerFailure', () => {
  describe('custom fields', () => {
    it('puts a single rejected custom field on that field', () => {
      const mapped = mapServerFailure(
        failure(
          'INVALID_ARGUMENT',
          'custom field validation failed: [tissue_grade: required field is missing or null]',
        ),
        'create',
      );

      expect(mapped.fields).toEqual({
        tissue_grade: { ns: 'sample-detail', key: 'validation.required' },
      });
      expect(mapped.form).toBeNull();
    });

    it('splits several rejected fields out of one message', () => {
      const mapped = mapServerFailure(
        failure(
          'INVALID_ARGUMENT',
          'custom field validation failed: [notes: string length 12 exceeds max_length 5] ' +
            "[tube_type: value 'citrate' is not in the allowed enum set]",
        ),
        'update',
      );

      expect(Object.keys(mapped.fields).sort()).toEqual(['notes', 'tube_type']);
      expect(mapped.fields.notes).toEqual({
        ns: 'sample-detail',
        key: 'validation.text.tooLong',
      });
      expect(mapped.fields.tube_type).toEqual({
        ns: 'sample-detail',
        key: 'validation.enum.notAllowed',
      });
    });

    it('maps each message rule to the same key the client-side mirror uses', () => {
      const cases: readonly [string, string][] = [
        ['required field is missing or null', 'validation.required'],
        ['expected string value', 'validation.text.expected'],
        ['expected integer value', 'validation.int.expected'],
        ['expected numeric value', 'validation.float.expected'],
        ['expected boolean value', 'validation.bool.expected'],
        ['expected string value for enum', 'validation.enum.expected'],
        ['expected string UUID for reference', 'validation.reference.expected'],
        ['reference value is not a valid UUID', 'validation.reference.invalid'],
        ['value is below minimum', 'validation.belowMin'],
        ['value exceeds maximum', 'validation.aboveMax'],
        ['date must be in ISO-8601 format', 'validation.date.format'],
        ['datetime must be in ISO-8601 format', 'validation.datetime.format'],
        ['date is before minimum', 'validation.beforeMin'],
        ['datetime is after maximum', 'validation.afterMax'],
      ];

      for (const [message, key] of cases) {
        const mapped = mapServerFailure(
          failure('INVALID_ARGUMENT', `custom field validation failed: [k: ${message}]`),
          'create',
        );
        expect(mapped.fields.k, message).toEqual({ ns: 'sample-detail', key });
      }
    });

    it('falls back to a named "the server rejected this field" when the reason is new', () => {
      // A rule this bundle does not know about must still name the field: that
      // is the whole point of not showing a generic banner.
      const mapped = mapServerFailure(
        failure('INVALID_ARGUMENT', 'custom field validation failed: [k: something new]'),
        'create',
      );

      expect(mapped.fields.k).toEqual({ ns: 'sample-detail', key: 'validation.rejected' });
    });

    it('maps the too-many-fields cap, which has no key of its own', () => {
      const mapped = mapServerFailure(
        failure(
          'INVALID_ARGUMENT',
          'custom field validation failed: [: too many custom fields: 201 exceeds the limit of 200]',
        ),
        'create',
      );

      // The C++ reports this one with an empty key, so the message itself has to
      // carry the information; it goes to the custom-fields group, not a field.
      expect(mapped.fields).toEqual({});
      expect(mapped.form).toEqual({ ns: 'sample-detail', key: 'server.tooManyFields' });
    });
  });

  describe('rules only the server can check', () => {
    it('puts a size-class mismatch on the container type field', () => {
      const mapped = mapServerFailure(
        failure(
          'INVALID_ARGUMENT',
          'container_type size_class is not accepted at this box position',
        ),
        'create',
      );

      expect(mapped.fields).toEqual({
        containerTypeId: { ns: 'sample-detail', key: 'server.sizeClassNotAccepted' },
      });
      expect(mapped.form).toBeNull();
    });

    it('puts an unknown position on the position field', () => {
      const mapped = mapServerFailure(
        failure('INVALID_ARGUMENT', "position_label does not exist in this box's BoxType"),
        'create',
      );

      expect(mapped.fields).toEqual({
        positionLabel: { ns: 'sample-detail', key: 'server.unknownPosition' },
      });
    });

    it('puts a dead box reference on the box field', () => {
      const mapped = mapServerFailure(
        failure('INVALID_ARGUMENT', 'box_id does not reference a live Box in this lab'),
        'create',
      );

      expect(mapped.fields).toEqual({
        boxId: { ns: 'sample-detail', key: 'server.unknownBox' },
      });
    });

    it('puts a dead item-type reference on the item type field', () => {
      const mapped = mapServerFailure(
        failure('INVALID_ARGUMENT', 'item_type_id does not reference a live ItemType in this lab'),
        'create',
      );

      expect(mapped.fields).toEqual({
        itemTypeId: { ns: 'sample-detail', key: 'server.unknownItemType' },
      });
    });
  });

  describe('a taken position', () => {
    it('lands on the position field when creating', () => {
      const mapped = mapServerFailure(
        failure(
          'ALREADY_EXISTS',
          'execute sqlite sample statement: UNIQUE constraint failed: samples.box_id, samples.position_label',
        ),
        'create',
      );

      // The message differs between SQLite and Postgres and is not worth
      // parsing: on a create, the only unique constraint a sample can hit is
      // the position index.
      expect(mapped.fields).toEqual({
        positionLabel: { ns: 'sample-detail', key: 'server.positionTaken' },
      });
      expect(mapped.form).toBeNull();
    });

    it('lands on the destination position field when moving', () => {
      const mapped = mapServerFailure(failure('ALREADY_EXISTS', 'duplicate key value'), 'move');

      expect(mapped.fields).toEqual({
        positionLabel: { ns: 'sample-detail', key: 'server.positionTaken' },
      });
    });

    it('is a form-level failure for an operation with no position to blame', () => {
      const mapped = mapServerFailure(failure('ALREADY_EXISTS', 'nope'), 'delete');

      expect(mapped.fields).toEqual({});
      expect(mapped.form).toEqual({ ns: 'common', key: 'errors.ALREADY_EXISTS' });
    });
  });

  describe('everything else', () => {
    it('keeps a permission refusal at the form level, not on a field', () => {
      const mapped = mapServerFailure(failure('PERMISSION_DENIED', 'no'), 'create');

      expect(mapped).toEqual({
        fields: {},
        form: { ns: 'common', key: 'errors.PERMISSION_DENIED' },
      });
    });

    it('maps an expired session to the code the shell acts on', () => {
      expect(mapServerFailure(failure('UNAUTHENTICATED', 'expired'), 'update').form).toEqual({
        ns: 'common',
        key: 'errors.UNAUTHENTICATED',
      });
    });

    it('maps a network failure', () => {
      expect(mapServerFailure(failure('UNAVAILABLE', 'offline'), 'create').form).toEqual({
        ns: 'common',
        key: 'errors.UNAVAILABLE',
      });
    });

    it('keeps an unrecognised INVALID_ARGUMENT at the form level', () => {
      const mapped = mapServerFailure(failure('INVALID_ARGUMENT', 'name is required'), 'create');

      expect(mapped.fields).toEqual({});
      expect(mapped.form).toEqual({ ns: 'common', key: 'errors.INVALID_ARGUMENT' });
    });

    it('does not treat "custom field validation failed" with no parsable field as a field', () => {
      const mapped = mapServerFailure(
        failure('INVALID_ARGUMENT', 'custom field validation failed:'),
        'create',
      );

      expect(mapped.fields).toEqual({});
      expect(mapped.form).toEqual({ ns: 'common', key: 'errors.INVALID_ARGUMENT' });
    });

    it('maps anything that is not an ApiError to UNKNOWN', () => {
      expect(mapServerFailure(new Error('boom'), 'create').form).toEqual({
        ns: 'common',
        key: 'errors.UNKNOWN',
      });
      expect(mapServerFailure(undefined, 'create').form).toEqual({
        ns: 'common',
        key: 'errors.UNKNOWN',
      });
    });
  });
});
