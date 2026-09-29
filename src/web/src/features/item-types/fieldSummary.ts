// SPDX-License-Identifier: AGPL-3.0-or-later
import type { TFunction } from 'i18next';
import {
  FieldDataType,
  type CustomFieldDefinition,
  type ItemType,
} from '../../gen/fmgr/v1/item_type_pb';
import {
  lineageOf,
  parseValidation,
  type EffectiveField,
  type ValidationConstraints,
} from './itemTypeModel';

/**
 * How a field definition is described on screen (TODO.md G3.9).
 *
 * Presentation only, and every string comes from the feature's own namespace:
 * the *rules* live in `itemTypeModel.ts`, which owns no copy.
 */

/**
 * The i18next key for one data type.
 *
 * A map rather than `enumLabel()`: `common.json`'s `enums.*` are keyed by proto
 * name and only cover the enums the sample screens needed, and the admin screen
 * needs all eight `FieldDataType` values plus their own wording ("Yes/no", not
 * "BOOL").
 */
const DATA_TYPE_KEYS: Readonly<Partial<Record<FieldDataType, string>>> = {
  [FieldDataType.TEXT]: 'dataTypes.text',
  [FieldDataType.INT]: 'dataTypes.int',
  [FieldDataType.FLOAT]: 'dataTypes.float',
  [FieldDataType.BOOL]: 'dataTypes.bool',
  [FieldDataType.DATE]: 'dataTypes.date',
  [FieldDataType.DATETIME]: 'dataTypes.datetime',
  [FieldDataType.ENUM]: 'dataTypes.enum',
  [FieldDataType.REFERENCE]: 'dataTypes.reference',
};

export function dataTypeLabel(t: TFunction<'itemTypes'>, dataType: FieldDataType): string {
  const key = DATA_TYPE_KEYS[dataType];
  // The key is computed from the enum, so the literal-key inference cannot
  // apply; an unknown value falls back to the number rather than showing a key.
  return key === undefined ? String(dataType) : String(t(key as never));
}

/** The data types a definition may use, in the order the form offers them. */
export const DEFINABLE_DATA_TYPES: readonly FieldDataType[] = [
  FieldDataType.TEXT,
  FieldDataType.INT,
  FieldDataType.FLOAT,
  FieldDataType.BOOL,
  FieldDataType.DATE,
  FieldDataType.DATETIME,
  FieldDataType.ENUM,
  FieldDataType.REFERENCE,
];

/** One translated constraint, e.g. `max length 20`; `null` when unset. */
export function constraintPart(
  t: TFunction<'itemTypes'>,
  constraints: ValidationConstraints,
  constraint: 'max_length' | 'min' | 'max' | 'values',
): string | null {
  switch (constraint) {
    case 'max_length':
      return constraints.maxLength === undefined
        ? null
        : t('fields.constraint.maxLength', { value: constraints.maxLength });
    case 'min':
      return constraints.min === undefined
        ? null
        : t('fields.constraint.min', { value: constraints.min });
    case 'max':
      return constraints.max === undefined
        ? null
        : t('fields.constraint.max', { value: constraints.max });
    case 'values':
      return constraints.values === undefined || constraints.values.length === 0
        ? null
        : t('fields.constraint.values', { values: constraints.values.join(', ') });
  }
}

/** Every constraint a definition carries, for its row in the list. */
export function constraintSummary(
  t: TFunction<'itemTypes'>,
  cfd: CustomFieldDefinition,
): readonly string[] {
  const constraints = parseValidation(cfd.validationJson);
  return (['max_length', 'min', 'max', 'values'] as const)
    .map((constraint) => constraintPart(t, constraints, constraint))
    .filter((part): part is string => part !== null);
}

/** Where a field comes from, as the row under it reads. */
export function originLabel(t: TFunction<'itemTypes'>, field: EffectiveField): string {
  if (field.origin === 'lab') {
    return t('fields.originLab');
  }
  return t('fields.originAncestor', { name: field.originName ?? '' });
}

/**
 * "Blood / Serum": a node's lineage, used for the breadcrumb and for the
 * parent list, so two nodes sharing a name stay distinguishable.
 */
export function pathLabel(types: readonly ItemType[], id: string): string {
  return lineageOf(types, id)
    .map((ancestor) => types.find((candidate) => candidate.id === ancestor)?.name ?? ancestor)
    .reverse()
    .join(' / ');
}
