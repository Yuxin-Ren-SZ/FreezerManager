// SPDX-License-Identifier: AGPL-3.0-or-later
import { isApiError } from '../../api/errors';

/**
 * Where a server rejection goes on the sample form (TODO.md G3.3).
 *
 * The acceptance criterion is the whole reason this module is separate: *"a
 * server `INVALID_ARGUMENT` must be shown on the offending field, not as a
 * generic banner"*. The server's error body is only `{"code", "message"}`
 * (`RestErrorTranslation.h`), so the message text is the only place a field name
 * can come from — which makes parsing it a mirrored contract, not a guess:
 *
 *  - `prepare_custom_fields()` in `SampleServiceImpl.cc` renders
 *    `custom field validation failed:` followed by one `[key: message]` group per
 *    failure, with the messages coming from `core::validate_custom_fields`;
 *  - `validate_sample()` in both backends names the core fields it rejects
 *    (`box_id`, `position_label`, `container_type … size_class`);
 *  - `UniqueViolation` → `ALREADY_EXISTS` (`GrpcErrorTranslation.h`), and on a
 *    sample create or move the only unique constraint it can be is
 *    `samples_position_unique` on `(box_id, position_label)`.
 *
 * The English message itself is never shown: it is server text that can name
 * internal state, and `apiErrorMessage()`'s rule (G-arch 7) is that only
 * translated keys reach the screen. Each recognised rule maps to the *same* i18n
 * key the client-side mirror uses, so a rejection the mirror also knows about
 * reads identically whichever side caught it.
 */

/** A translated message, ready for `t(key, { ns })`. */
export interface FailureMessage {
  readonly ns: 'common' | 'sample-detail';
  readonly key: string;
}

export interface MappedFailure {
  /**
   * Messages addressed to a form field: a core field's wire name
   * (`positionLabel`, `containerTypeId`, …) or a custom-field key, which is also
   * what that field is called in `custom_fields_json`.
   */
  readonly fields: Readonly<Record<string, FailureMessage | undefined>>;
  /** Set only when nothing could be attributed to a field. */
  readonly form: FailureMessage | null;
}

/** The operations that can produce a failure, for the code-level fallbacks. */
export type SampleOperation = 'create' | 'update' | 'move' | 'checkout' | 'delete';

const feature = (key: string): FailureMessage => ({ ns: 'sample-detail', key });
const common = (key: string): FailureMessage => ({ ns: 'common', key: `errors.${key}` });

/**
 * One `core::validate_custom_fields` message → the i18n key the mirror uses for
 * the same rule. A rule this bundle does not know about still names the field,
 * with the "the server rejected this value" key: that is the difference between
 * a form the user can fix and a banner they have to interpret.
 */
function customFieldMessageKey(message: string): string {
  // Ordered longest-first, because several of these share a prefix
  // ("value is below minimum" / "value exceeds maximum" / "value '…'").
  const table: readonly [string, string][] = [
    ['required field is missing or null', 'validation.required'],
    ['expected string value for enum', 'validation.enum.expected'],
    ['expected string UUID for reference', 'validation.reference.expected'],
    ['reference value is not a valid UUID', 'validation.reference.invalid'],
    ['expected string value', 'validation.text.expected'],
    ['expected integer value', 'validation.int.expected'],
    ['expected numeric value', 'validation.float.expected'],
    ['expected boolean value', 'validation.bool.expected'],
    ['exceeds max_length', 'validation.text.tooLong'],
    ['is not in the allowed enum set', 'validation.enum.notAllowed'],
    ['value is below minimum', 'validation.belowMin'],
    ['value exceeds maximum', 'validation.aboveMax'],
    // `to_string(FieldDataType)` is lowercase: "date", "datetime".
    ['date must be in ISO-8601 format', 'validation.date.format'],
    ['datetime must be in ISO-8601 format', 'validation.datetime.format'],
    ['date is before minimum', 'validation.beforeMin'],
    ['datetime is before minimum', 'validation.beforeMin'],
    ['date is after maximum', 'validation.afterMax'],
    ['datetime is after maximum', 'validation.afterMax'],
  ];

  for (const [needle, key] of table) {
    if (message.includes(needle)) {
      return key;
    }
  }
  return 'validation.rejected';
}

/** `[key: message] [key: message]` → `{ key: messageKey }`. */
function parseCustomFieldErrors(message: string): Record<string, FailureMessage> {
  const fields: Record<string, FailureMessage> = {};
  // The key cannot contain `]`, and the C++ builds the group with `[key: …]`;
  // a message containing `]` would still be captured up to the last one, which
  // is why the greedy match is bounded by the next `[` instead.
  for (const match of message.matchAll(/\[([^\]]+?):\s*([^[]*?)\]/g)) {
    const key = match[1].trim();
    const detail = match[2].trim();
    if (key === '') {
      continue;
    }
    fields[key] = feature(customFieldMessageKey(detail));
  }
  return fields;
}

/**
 * Attribution for a non-custom-field `INVALID_ARGUMENT`, by the exact strings
 * the backends use. Anything unrecognised stays at the form level.
 */
function coreFieldFor(message: string): string | null {
  if (message.includes('container_type size_class is not accepted at this box position')) {
    return 'containerTypeId';
  }
  if (message.includes("position_label does not exist in this box's BoxType")) {
    return 'positionLabel';
  }
  if (message.includes('position_label must be set when box_id is set')) {
    return 'positionLabel';
  }
  if (message.includes('box_id does not reference a live Box')) {
    return 'boxId';
  }
  if (message.includes('container_type_id does not reference a live ContainerType')) {
    return 'containerTypeId';
  }
  if (message.includes('item_type_id does not reference a live ItemType')) {
    return 'itemTypeId';
  }
  return null;
}

const CORE_MESSAGE_KEYS: Readonly<Record<string, string>> = {
  containerTypeId: 'server.sizeClassNotAccepted',
  positionLabel: 'server.unknownPosition',
  boxId: 'server.unknownBox',
  itemTypeId: 'server.unknownItemType',
};

/** The custom-field messages the C++ reports without a key of their own. */
const UNKEYED_CUSTOM_FIELD_RULES = ['too many custom fields'];

export function mapServerFailure(error: unknown, operation: SampleOperation): MappedFailure {
  if (!isApiError(error)) {
    return { fields: {}, form: common('UNKNOWN') };
  }

  if (
    error.code === 'INVALID_ARGUMENT' &&
    error.message.includes('custom field validation failed')
  ) {
    // The cap is reported with an empty key (`[: too many custom fields: …]`),
    // so it has no field to name and belongs to the custom-field group.
    if (UNKEYED_CUSTOM_FIELD_RULES.some((needle) => error.message.includes(needle))) {
      return { fields: {}, form: feature('server.tooManyFields') };
    }
    const fields = parseCustomFieldErrors(error.message);
    if (Object.keys(fields).length > 0) {
      return { fields, form: null };
    }
    return { fields: {}, form: common('INVALID_ARGUMENT') };
  }

  if (error.code === 'INVALID_ARGUMENT') {
    const field = coreFieldFor(error.message);
    if (field !== null) {
      return {
        fields: { [field]: feature(CORE_MESSAGE_KEYS[field] ?? 'validation.rejected') },
        form: null,
      };
    }
    return { fields: {}, form: common('INVALID_ARGUMENT') };
  }

  if (error.code === 'ALREADY_EXISTS') {
    // A create or a move can only collide on `samples_position_unique`; for
    // anything else there is no position to blame, so it stays a form-level
    // failure rather than being pinned to the wrong field.
    if (operation === 'create' || operation === 'update' || operation === 'move') {
      return { fields: { positionLabel: feature('server.positionTaken') }, form: null };
    }
    return { fields: {}, form: common('ALREADY_EXISTS') };
  }

  return { fields: {}, form: common(error.code) };
}
