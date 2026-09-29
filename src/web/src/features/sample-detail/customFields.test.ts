// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import {
  CustomFieldDefinitionSchema,
  FieldDataType,
  ItemTypeSchema,
  ScopeKind,
  type CustomFieldDefinition,
  type ItemType,
} from '../../gen/fmgr/v1/item_type_pb';
import { TimestampSchema } from '../../gen/fmgr/v1/common/types_pb';
import {
  parseCustomFieldValues,
  resolveInheritedDefinitions,
  serializeCustomFieldValues,
  validateCustomFieldValues,
} from './customFields';

/**
 * The custom-field model behind the sample form (TODO.md G3.3).
 *
 * Two contracts, both taken from C++ rather than invented here:
 *
 *  - **inheritance** mirrors `storage::resolve_custom_field_defs`: lab-global
 *    definitions apply to every item type, an ancestor's definition applies to
 *    its descendants, and on a duplicate key the most-derived one wins. Reading
 *    only the leaf is the bug this test exists to catch, and it is invisible
 *    against an item type with no parent.
 *  - **validation** mirrors `core::validate_custom_fields`, rule for rule. The
 *    server decides; this only decides what the user is told before the round
 *    trip, so a rule that disagrees with the C++ is a bug in the UI, not a
 *    second opinion.
 */

const at = create(TimestampSchema, { unixMicros: 1n });

const itemType = (id: string, parentId?: string): ItemType =>
  create(ItemTypeSchema, { id, labId: 'lab-1', name: id, parentId, createdAt: at });

const cfd = (init: {
  id: string;
  key: string;
  dataType: FieldDataType;
  itemTypeId?: string;
  labId?: string;
  label?: string;
  required?: boolean;
  isPhi?: boolean;
  validationJson?: string;
  scopeKind?: ScopeKind;
  archived?: boolean;
}): CustomFieldDefinition =>
  create(CustomFieldDefinitionSchema, {
    labId: init.labId ?? 'lab-1',
    scopeKind: init.scopeKind ?? ScopeKind.SAMPLE,
    itemTypeId: init.itemTypeId,
    key: init.key,
    label: init.label ?? init.key,
    dataType: init.dataType,
    required: init.required ?? false,
    validationJson: init.validationJson ?? '{}',
    indexed: false,
    isPhi: init.isPhi ?? false,
    createdAt: at,
    archivedAt: init.archived === true ? at : undefined,
  });

/** Blood → {Serum, Plasma}; DNA in another lab; SerumCell off on its own. */
const ITEM_TYPES: ItemType[] = [
  itemType('it-blood'),
  itemType('it-serum', 'it-blood'),
  itemType('it-plasma', 'it-blood'),
  itemType('it-cell'),
  itemType('it-dna', undefined),
];

describe('resolveInheritedDefinitions', () => {
  it('keeps the fields defined on the item type itself', () => {
    const cfds = [cfd({ id: 'c1', key: 'leaf_only', dataType: FieldDataType.TEXT, itemTypeId: 'it-serum' })];

    const resolved = resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-serum');

    expect(resolved.map((entry) => entry.key)).toEqual(['leaf_only']);
  });

  it('includes a field inherited from the parent item type', () => {
    const cfds = [
      cfd({ id: 'c1', key: 'from_blood', dataType: FieldDataType.INT, itemTypeId: 'it-blood' }),
      cfd({ id: 'c2', key: 'leaf_only', dataType: FieldDataType.TEXT, itemTypeId: 'it-serum' }),
    ];

    const resolved = resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-serum');

    expect(resolved.map((entry) => entry.key).sort()).toEqual(['from_blood', 'leaf_only']);
    // The inherited definition keeps its own id: it is the same definition, not
    // a copy attributed to the leaf.
    expect(resolved.find((entry) => entry.key === 'from_blood')?.itemTypeId).toBe('it-blood');
  });

  it('includes a field inherited from a grandparent, and orders globals first', () => {
    const types = [...ITEM_TYPES, itemType('it-ig', 'it-serum')];
    const cfds = [
      cfd({ id: 'c0', key: 'lab_global', dataType: FieldDataType.TEXT }),
      cfd({ id: 'c1', key: 'from_blood', dataType: FieldDataType.TEXT, itemTypeId: 'it-blood' }),
      cfd({ id: 'c2', key: 'from_serum', dataType: FieldDataType.TEXT, itemTypeId: 'it-serum' }),
    ];

    const resolved = resolveInheritedDefinitions(types, cfds, 'it-ig');

    // Globals first, then root → leaf: the order the server's resolver
    // documents, which is also the order a form should present.
    expect(resolved.map((entry) => entry.key)).toEqual(['lab_global', 'from_blood', 'from_serum']);
  });

  it('lets the most-derived definition of a duplicated key win', () => {
    const cfds = [
      cfd({
        id: 'parent',
        key: 'notes',
        label: 'Notes',
        dataType: FieldDataType.TEXT,
        itemTypeId: 'it-blood',
        validationJson: JSON.stringify({ max_length: 20 }),
      }),
      cfd({
        id: 'child',
        key: 'notes',
        label: 'Serum notes',
        dataType: FieldDataType.TEXT,
        itemTypeId: 'it-serum',
        validationJson: JSON.stringify({ max_length: 5 }),
      }),
    ];

    const resolved = resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-serum');

    expect(resolved).toHaveLength(1);
    expect(resolved[0]?.id).toBe('child');
    expect(resolved[0]?.label).toBe('Serum notes');
  });

  it('lets a leaf override a lab-global definition of the same key', () => {
    const cfds = [
      cfd({ id: 'global', key: 'notes', dataType: FieldDataType.TEXT }),
      cfd({ id: 'leaf', key: 'notes', dataType: FieldDataType.INT, itemTypeId: 'it-serum' }),
    ];

    const resolved = resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-serum');

    expect(resolved.map((entry) => entry.id)).toEqual(['leaf']);
  });

  it('leaves out a definition attached to a sibling item type', () => {
    const cfds = [cfd({ id: 'c1', key: 'plasma_only', dataType: FieldDataType.TEXT, itemTypeId: 'it-plasma' })];

    expect(resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-serum')).toEqual([]);
  });

  it('leaves out a definition attached to the parent of the item type', () => {
    // A field on Serum must not appear when the subject is Blood: inheritance
    // only travels down the chain.
    const cfds = [cfd({ id: 'c1', key: 'serum_only', dataType: FieldDataType.TEXT, itemTypeId: 'it-serum' })];

    expect(resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-blood')).toEqual([]);
  });

  it('leaves out another lab\u2019s definitions', () => {
    const cfds = [cfd({ id: 'c1', key: 'other_lab', dataType: FieldDataType.TEXT, labId: 'lab-2' })];

    expect(resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-serum')).toEqual([]);
  });

  it('leaves out an archived definition even though the server already filters them', () => {
    const cfds = [
      cfd({ id: 'c1', key: 'gone', dataType: FieldDataType.TEXT, itemTypeId: 'it-blood', archived: true }),
    ];

    expect(resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-serum')).toEqual([]);
  });

  it('leaves out a definition that is not sample-scoped', () => {
    const cfds = [
      cfd({
        id: 'c1',
        key: 'box_field',
        dataType: FieldDataType.TEXT,
        itemTypeId: 'it-blood',
        scopeKind: ScopeKind.BOX,
      }),
    ];

    expect(resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-serum')).toEqual([]);
  });

  it('returns nothing instead of looping when the item-type chain has a cycle', () => {
    const cyclic = [itemType('a', 'b'), itemType('b', 'a')];
    const cfds = [cfd({ id: 'c1', key: 'k', dataType: FieldDataType.TEXT, itemTypeId: 'a' })];

    const resolved = resolveInheritedDefinitions(cyclic, cfds, 'a');

    // `a` is in its own lineage, so its field resolves once — and the walk
    // terminates.
    expect(resolved.map((entry) => entry.id)).toEqual(['c1']);
  });

  it('returns nothing for an item type the caller does not have', () => {
    const cfds = [cfd({ id: 'c1', key: 'k', dataType: FieldDataType.TEXT, itemTypeId: 'it-blood' })];

    expect(resolveInheritedDefinitions(ITEM_TYPES, cfds, 'it-missing')).toEqual([]);
  });
});

describe('custom field values', () => {
  it('parses a stored blob', () => {
    expect(parseCustomFieldValues('{"a":1,"b":false}')).toEqual({ a: 1, b: false });
  });

  it('treats an empty or unparseable blob as no values rather than throwing', () => {
    expect(parseCustomFieldValues('')).toEqual({});
    expect(parseCustomFieldValues('not json')).toEqual({});
    expect(parseCustomFieldValues('null')).toEqual({});
    expect(parseCustomFieldValues('[1,2]')).toEqual({});
  });

  it('serializes to a JSON object the gateway will parse', () => {
    expect(serializeCustomFieldValues({ a: 1, b: 'x' })).toBe('{"a":1,"b":"x"}');
    expect(serializeCustomFieldValues({})).toBe('{}');
  });
});

describe('validateCustomFieldValues', () => {
  const def = (init: Parameters<typeof cfd>[0]) => cfd(init);

  it('accepts an empty object when nothing is required', () => {
    const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.TEXT })];

    expect(validateCustomFieldValues(defs, {})).toEqual([]);
  });

  it('reports a missing required field, and a null one', () => {
    const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.TEXT, required: true })];

    expect(validateCustomFieldValues(defs, {}).map((error) => error.messageKey)).toEqual([
      'validation.required',
    ]);
    expect(validateCustomFieldValues(defs, { k: null }).map((error) => error.messageKey)).toEqual([
      'validation.required',
    ]);
  });

  it('accepts an empty string for a required text field, as the C++ does', () => {
    // `present = iter != end() && !is_null()`: "" is present and valid.
    const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.TEXT, required: true })];

    expect(validateCustomFieldValues(defs, { k: '' })).toEqual([]);
  });

  it('ignores a key that has no definition', () => {
    // The server iterates the definitions, not the submitted object.
    const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.TEXT })];

    expect(validateCustomFieldValues(defs, { unknown: 1 })).toEqual([]);
  });

  describe('text', () => {
    it('rejects a value that is not a string', () => {
      const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.TEXT })];

      expect(validateCustomFieldValues(defs, { k: 5 })[0]?.messageKey).toBe(
        'validation.text.expected',
      );
    });

    it('rejects a string longer than max_length', () => {
      const defs = [
        def({
          id: 'c1',
          key: 'k',
          dataType: FieldDataType.TEXT,
          validationJson: JSON.stringify({ max_length: 3 }),
        }),
      ];

      const errors = validateCustomFieldValues(defs, { k: 'abcd' });

      expect(errors[0]?.messageKey).toBe('validation.text.tooLong');
      expect(validateCustomFieldValues(defs, { k: 'abc' })).toEqual([]);
    });

    it('measures max_length in UTF-8 bytes, because str.size() does', () => {
      const defs = [
        def({
          id: 'c1',
          key: 'k',
          dataType: FieldDataType.TEXT,
          validationJson: JSON.stringify({ max_length: 4 }),
        }),
      ];

      // "ää" is 2 characters but 4 bytes: accepted. "äää" is 3 characters and
      // 6 bytes: rejected. A character count would invert both.
      expect(validateCustomFieldValues(defs, { k: '\u00e4\u00e4' })).toEqual([]);
      expect(validateCustomFieldValues(defs, { k: '\u00e4\u00e4\u00e4' })[0]?.messageKey).toBe(
        'validation.text.tooLong',
      );
    });
  });

  describe('int', () => {
    const defs = [
      def({
        id: 'c1',
        key: 'k',
        dataType: FieldDataType.INT,
        validationJson: JSON.stringify({ min: 1, max: 10 }),
      }),
    ];

    it('rejects a non-integer', () => {
      expect(validateCustomFieldValues(defs, { k: '5' })[0]?.messageKey).toBe(
        'validation.int.expected',
      );
      expect(validateCustomFieldValues(defs, { k: 1.5 })[0]?.messageKey).toBe(
        'validation.int.expected',
      );
    });

    it('enforces min and max inclusively', () => {
      expect(validateCustomFieldValues(defs, { k: 1 })).toEqual([]);
      expect(validateCustomFieldValues(defs, { k: 10 })).toEqual([]);
      expect(validateCustomFieldValues(defs, { k: 0 })[0]?.messageKey).toBe(
        'validation.belowMin',
      );
      expect(validateCustomFieldValues(defs, { k: 11 })[0]?.messageKey).toBe(
        'validation.aboveMax',
      );
    });
  });

  describe('float', () => {
    const defs = [
      def({
        id: 'c1',
        key: 'k',
        dataType: FieldDataType.FLOAT,
        validationJson: JSON.stringify({ min: 0, max: 1 }),
      }),
    ];

    it('accepts a fractional number and enforces the range', () => {
      expect(validateCustomFieldValues(defs, { k: 0.5 })).toEqual([]);
      expect(validateCustomFieldValues(defs, { k: -0.1 })[0]?.messageKey).toBe(
        'validation.belowMin',
      );
      expect(validateCustomFieldValues(defs, { k: 2 })[0]?.messageKey).toBe('validation.aboveMax');
    });

    it('rejects a boolean, which nlohmann would read as a number', () => {
      expect(validateCustomFieldValues(defs, { k: true })[0]?.messageKey).toBe(
        'validation.float.expected',
      );
    });
  });

  describe('bool', () => {
    const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.BOOL })];

    it('takes true and false and rejects anything else', () => {
      expect(validateCustomFieldValues(defs, { k: true })).toEqual([]);
      expect(validateCustomFieldValues(defs, { k: false })).toEqual([]);
      expect(validateCustomFieldValues(defs, { k: 'true' })[0]?.messageKey).toBe(
        'validation.bool.expected',
      );
    });
  });

  describe('date', () => {
    const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.DATE })];

    it('accepts YYYY-MM-DD and rejects anything shorter or mis-shaped', () => {
      expect(validateCustomFieldValues(defs, { k: '2026-01-05' })).toEqual([]);
      expect(validateCustomFieldValues(defs, { k: '2026-1-5' })[0]?.messageKey).toBe(
        'validation.date.format',
      );
      expect(validateCustomFieldValues(defs, { k: 20260105 })[0]?.messageKey).toBe(
        'validation.date.expected',
      );
    });

    it('compares min and max as strings, as the C++ does', () => {
      const bounded = [
        def({
          id: 'c1',
          key: 'k',
          dataType: FieldDataType.DATE,
          validationJson: JSON.stringify({ min: '2026-01-01', max: '2026-12-31' }),
        }),
      ];

      expect(validateCustomFieldValues(bounded, { k: '2026-06-01' })).toEqual([]);
      expect(validateCustomFieldValues(bounded, { k: '2025-12-31' })[0]?.messageKey).toBe(
        'validation.beforeMin',
      );
      expect(validateCustomFieldValues(bounded, { k: '2027-01-01' })[0]?.messageKey).toBe(
        'validation.afterMax',
      );
    });
  });

  describe('datetime', () => {
    const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.DATETIME })];

    it('accepts ISO-8601 with seconds and rejects a minute-precision value', () => {
      expect(validateCustomFieldValues(defs, { k: '2026-01-05T10:30:00' })).toEqual([]);
      // `datetime-local` produces this without seconds; the form has to widen it
      // before submitting, and this is the assertion that pins that.
      expect(validateCustomFieldValues(defs, { k: '2026-01-05T10:30' })[0]?.messageKey).toBe(
        'validation.datetime.format',
      );
    });
  });

  describe('enum', () => {
    const defs = [
      def({
        id: 'c1',
        key: 'k',
        dataType: FieldDataType.ENUM,
        validationJson: JSON.stringify({ values: ['EDTA', 'heparin'] }),
      }),
    ];

    it('accepts one of the allowed values', () => {
      expect(validateCustomFieldValues(defs, { k: 'EDTA' })).toEqual([]);
    });

    it('rejects a value outside the set', () => {
      expect(validateCustomFieldValues(defs, { k: 'citrate' })[0]?.messageKey).toBe(
        'validation.enum.notAllowed',
      );
    });

    it('accepts any string when the definition lists no values', () => {
      const open = [def({ id: 'c1', key: 'k', dataType: FieldDataType.ENUM })];

      expect(validateCustomFieldValues(open, { k: 'anything' })).toEqual([]);
    });
  });

  describe('reference', () => {
    const defs = [def({ id: 'c1', key: 'k', dataType: FieldDataType.REFERENCE })];

    it('accepts a UUID in either case', () => {
      expect(
        validateCustomFieldValues(defs, { k: '5f0c1e6a-1f3a-4c9e-9a1e-2b3c4d5e6f70' }),
      ).toEqual([]);
      expect(
        validateCustomFieldValues(defs, { k: '5F0C1E6A-1F3A-4C9E-9A1E-2B3C4D5E6F70' }),
      ).toEqual([]);
    });

    it('rejects a value that is not a UUID', () => {
      expect(validateCustomFieldValues(defs, { k: 'sample-1' })[0]?.messageKey).toBe(
        'validation.reference.invalid',
      );
      expect(validateCustomFieldValues(defs, { k: 7 })[0]?.messageKey).toBe(
        'validation.reference.expected',
      );
    });
  });

  it('treats unparseable constraints as no constraints, as parse_constraints does', () => {
    const defs = [
      def({
        id: 'c1',
        key: 'k',
        dataType: FieldDataType.TEXT,
        validationJson: '{not json',
      }),
    ];

    expect(validateCustomFieldValues(defs, { k: 'anything at all' })).toEqual([]);
  });

  it('accumulates every error rather than stopping at the first', () => {
    const defs = [
      def({ id: 'c1', key: 'a', dataType: FieldDataType.INT }),
      def({ id: 'c2', key: 'b', dataType: FieldDataType.BOOL }),
    ];

    const errors = validateCustomFieldValues(defs, { a: 'x', b: 'y' });

    expect(errors.map((error) => error.key)).toEqual(['a', 'b']);
  });

  it('rejects more than 200 submitted keys, the hardening cap', () => {
    const tooMany = Object.fromEntries(
      Array.from({ length: 201 }, (_, index) => [`k${String(index)}`, 1]),
    );

    const errors = validateCustomFieldValues([], tooMany);

    expect(errors[0]?.messageKey).toBe('validation.tooMany');
  });
});
