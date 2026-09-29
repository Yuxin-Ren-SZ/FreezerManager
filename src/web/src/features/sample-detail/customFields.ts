// SPDX-License-Identifier: AGPL-3.0-or-later
import type { CustomFieldDefinition, ItemType } from '../../gen/fmgr/v1/item_type_pb';
import { FieldDataType, ScopeKind } from '../../gen/fmgr/v1/item_type_pb';

/**
 * The custom-field model behind the sample create/edit form (TODO.md G3.3).
 *
 * Two things are ported from C++ rather than invented here, and both are the
 * reason this module exists as a unit-testable function rather than as logic
 * inside a component:
 *
 *  1. **Inheritance** — `storage::resolve_custom_field_defs`
 *     (`src/storage/CustomFieldResolver.h`). The server's
 *     `ListCustomFieldDefinitions` filters to the definitions attached to one
 *     item type and does *no* ancestor resolution, so if the client passes an
 *     `item_type_id` it gets the leaf's fields only. A form built that way looks
 *     perfectly correct against an item type with no parent, which is why the
 *     whole lab's definitions are fetched and walked here.
 *  2. **Validation** — `core::validate_custom_fields`
 *     (`src/core/custom_field_validator.h`), rule for rule. It is a mirror, not
 *     a second opinion: the server decides, and this only decides what the user
 *     is told before the round trip.
 *
 * Messages are returned as i18n keys rather than sentences, because the same
 * key is used by the client-side mirror and by `serverErrors.ts` when the server
 * reports a rule this bundle knows about.
 */

/** A validation failure, addressed to the field the user has to fix. */
export interface FieldMessage {
  /** The custom-field key, or the core field's name. */
  readonly key: string;
  /** An i18n key in the `sample-detail` namespace. */
  readonly messageKey: string;
}

/** The hardening cap on submitted values: `k_max_custom_fields_per_entity`. */
export const MAX_CUSTOM_FIELDS = 200;

/**
 * The definitions that apply to one item type, in the order the server's
 * resolver documents: lab-global first, then the ancestry from the root
 * towards the leaf. On a duplicate `key` the most-derived definition wins, so a
 * child can tighten a parent's rule.
 *
 * The walk is cycle-guarded even though the repository rejects lineage cycles:
 * a partially-loaded item-type list is enough to produce one here.
 */
export function resolveInheritedDefinitions(
  itemTypes: readonly ItemType[],
  cfds: readonly CustomFieldDefinition[],
  itemTypeId: string,
): CustomFieldDefinition[] {
  const byId = new Map(itemTypes.map((itemType) => [itemType.id, itemType]));
  const subject = byId.get(itemTypeId);
  if (subject === undefined) {
    return [];
  }

  // Walk leaf → root so the most-derived node is visited first and recorded
  // with the highest rank; then reverse for display.
  const lineage: string[] = [];
  const seen = new Set<string>();
  let cursor: string | undefined = itemTypeId;
  while (cursor !== undefined && !seen.has(cursor)) {
    seen.add(cursor);
    lineage.push(cursor);
    cursor = byId.get(cursor)?.parentId;
  }

  const rank = new Map(lineage.map((id, index) => [id, lineage.length - index]));
  const best = new Map<string, { rank: number; cfd: CustomFieldDefinition }>();

  for (const cfd of cfds) {
    if (cfd.labId !== subject.labId) continue;
    if (cfd.scopeKind !== ScopeKind.SAMPLE) continue;
    // The server's query already excludes tombstones; filtering here too means
    // an archived definition can never reach a form through a caller that
    // passed `include_archived` or a fake that returned one.
    if (cfd.archivedAt !== undefined) continue;

    let specificity = 0; // lab-global
    if (cfd.itemTypeId !== undefined) {
      const found = rank.get(cfd.itemTypeId);
      if (found === undefined) continue; // attached outside this lineage
      specificity = found;
    }

    const slot = best.get(cfd.key);
    if (slot === undefined || specificity >= slot.rank) {
      best.set(cfd.key, { rank: specificity, cfd });
    }
  }

  // The lab-global group first, then the ancestry root → leaf. Within a group
  // the insertion order of `cfds` is kept, so the order is stable across
  // renders.
  return [...best.values()]
    .map((entry, index) => ({ ...entry, index }))
    .sort((a, b) => a.rank - b.rank || a.index - b.index)
    .map((entry) => entry.cfd);
}

/**
 * A definition's `validation_json`, parsed.
 *
 * An unparseable blob is *no constraints*, which is what the C++'s
 * `parse_constraints()` does — failing open here matches the server rather than
 * inventing a stricter client.
 */
function constraints(cfd: CustomFieldDefinition): Record<string, unknown> {
  try {
    const parsed: unknown = JSON.parse(cfd.validationJson === '' ? '{}' : cfd.validationJson);
    if (parsed !== null && typeof parsed === 'object' && !Array.isArray(parsed)) {
      return parsed as Record<string, unknown>;
    }
    return {};
  } catch {
    return {};
  }
}

/** The allowed values of an enum definition; empty means "no constraint". */
export function enumValues(cfd: CustomFieldDefinition): string[] {
  const values = constraints(cfd).values;
  return Array.isArray(values)
    ? values.filter((value): value is string => typeof value === 'string')
    : [];
}

/** The constraints worth showing as a hint under a field. */
export function constraintsOf(cfd: CustomFieldDefinition): Record<string, unknown> {
  return constraints(cfd);
}

/** UTF-8 byte length, because the C++ compares `std::string::size()`. */
function byteLength(value: string): number {
  return new TextEncoder().encode(value).length;
}

const UUID_PATTERN =
  /^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/;

/** One submitted value against one definition; mirrors `validate_single_field`. */
function validateField(
  cfd: CustomFieldDefinition,
  value: unknown,
  present: boolean,
): string | null {
  if (cfd.required && !present) {
    return 'validation.required';
  }
  if (!present) {
    return null;
  }

  const parsed = constraints(cfd);

  switch (cfd.dataType) {
    case FieldDataType.TEXT:
      if (typeof value !== 'string') return 'validation.text.expected';
      if (typeof parsed.max_length === 'number' && byteLength(value) > parsed.max_length) {
        return 'validation.text.tooLong';
      }
      return null;

    case FieldDataType.INT:
      // JavaScript has one number type, so `Number.isInteger` is the mirror of
      // nlohmann's `is_number_integer()`; `1.5` is what actually reaches the wire
      // as a non-integer.
      if (typeof value !== 'number' || !Number.isInteger(value)) return 'validation.int.expected';
      if (typeof parsed.min === 'number' && value < parsed.min) return 'validation.belowMin';
      if (typeof parsed.max === 'number' && value > parsed.max) return 'validation.aboveMax';
      return null;

    case FieldDataType.FLOAT:
      // The C++ rejects booleans explicitly: nlohmann reads `true` as 1.
      if (typeof value !== 'number') return 'validation.float.expected';
      if (typeof parsed.min === 'number' && value < parsed.min) return 'validation.belowMin';
      if (typeof parsed.max === 'number' && value > parsed.max) return 'validation.aboveMax';
      return null;

    case FieldDataType.BOOL:
      return typeof value === 'boolean' ? null : 'validation.bool.expected';

    case FieldDataType.DATE:
      return validateDateOrDatetime(value, parsed, true);

    case FieldDataType.DATETIME:
      return validateDateOrDatetime(value, parsed, false);

    case FieldDataType.ENUM: {
      if (typeof value !== 'string') return 'validation.enum.expected';
      const allowed = enumValues(cfd);
      if (allowed.length === 0) return null;
      return allowed.includes(value) ? null : 'validation.enum.notAllowed';
    }

    case FieldDataType.REFERENCE:
      if (typeof value !== 'string') return 'validation.reference.expected';
      return UUID_PATTERN.test(value) ? null : 'validation.reference.invalid';

    case FieldDataType.UNSPECIFIED:
    default:
      return null;
  }
}

/**
 * The C++'s shape checks: a length floor plus hyphens (and a `T`) in fixed
 * positions, then a lexicographic min/max comparison — which is why the
 * constraints are compared as strings and not as dates.
 */
function validateDateOrDatetime(
  value: unknown,
  parsed: Record<string, unknown>,
  dateOnly: boolean,
): string | null {
  const prefix = dateOnly ? 'validation.date' : 'validation.datetime';
  if (typeof value !== 'string') {
    return `${prefix}.expected`;
  }
  const shaped = dateOnly
    ? value.length >= 10 && value[4] === '-' && value[7] === '-'
    : value.length >= 19 &&
      value[4] === '-' &&
      value[7] === '-' &&
      value[10] === 'T' &&
      value[13] === ':' &&
      value[16] === ':';
  if (!shaped) {
    return `${prefix}.format`;
  }
  if (typeof parsed.min === 'string' && value < parsed.min) return 'validation.beforeMin';
  if (typeof parsed.max === 'string' && value > parsed.max) return 'validation.afterMax';
  return null;
}

/**
 * Every failure in `values` against `definitions`, accumulated rather than
 * short-circuited, exactly as `validate_custom_fields()` does.
 *
 * Presence is the C++'s notion: a key that is absent or `null` is missing, and
 * an empty string is a present, valid value. A key with no definition is
 * ignored — the validator iterates the definitions, not the submitted object.
 */
export function validateCustomFieldValues(
  definitions: readonly CustomFieldDefinition[],
  values: Readonly<Record<string, unknown>>,
): FieldMessage[] {
  const keys = Object.keys(values);
  if (keys.length > MAX_CUSTOM_FIELDS) {
    // The C++ reports this with an empty key, so it belongs to the group rather
    // than to a field.
    return [{ key: '', messageKey: 'validation.tooMany' }];
  }

  const errors: FieldMessage[] = [];
  for (const cfd of definitions) {
    const present = Object.hasOwn(values, cfd.key) && values[cfd.key] !== null;
    const messageKey = validateField(cfd, values[cfd.key], present);
    if (messageKey !== null) {
      errors.push({ key: cfd.key, messageKey });
    }
  }
  return errors;
}

/** `custom_fields_json` as an object. Anything unusable is "no values". */
export function parseCustomFieldValues(json: string): Record<string, unknown> {
  if (json === '') {
    return {};
  }
  try {
    const parsed: unknown = JSON.parse(json);
    if (parsed !== null && typeof parsed === 'object' && !Array.isArray(parsed)) {
      return parsed as Record<string, unknown>;
    }
    return {};
  } catch {
    return {};
  }
}

/** The submitted values as `custom_fields_json`. */
export function serializeCustomFieldValues(values: Readonly<Record<string, unknown>>): string {
  return JSON.stringify(values);
}
