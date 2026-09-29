// SPDX-License-Identifier: AGPL-3.0-or-later
import { create, type MessageInitShape } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import { TimestampSchema } from '../../gen/fmgr/v1/common/types_pb';
import {
  CustomFieldDefinitionSchema,
  FieldDataType,
  ItemTypeSchema,
  ScopeKind,
  type CustomFieldDefinition,
  type ItemType,
} from '../../gen/fmgr/v1/item_type_pb';
import {
  buildItemTypeTree,
  canReparent,
  definitionProblems,
  lineageOf,
  parseValidation,
  resolveFields,
  resolveInheritedFields,
  serializeValidation,
  tightenViolations,
} from './itemTypeModel';

/**
 * The item-type admin's domain rules (TODO.md G3.9, PRD §4.3, L10).
 *
 * These are pure functions, and they are where the acceptance criteria
 * actually live:
 *
 *  - **cycle-safe re-parenting** — `canReparent` is the drag guard, and
 *    `buildItemTypeTree`/`lineageOf`/`resolveFields` must survive data that
 *    already contains a cycle (a stale tree, a hand-edited row) instead of
 *    hanging the tab;
 *  - **tighten but do not drop** — `tightenViolations`, tested in both
 *    directions: the refusal *and* the narrowing that must be allowed;
 *  - **`is_phi` + `indexed` is refused (L10)** with a reason, and `is_phi` is
 *    only offered in a PHI lab — `definitionProblems`.
 *
 * The first test in `buildItemTypeTree` is the one to read twice: it runs the
 * cyclic input through an unguarded copy of the walk and watches it fail. A
 * guard nobody has watched fail is not a guard (the G3.1 standard).
 */

const CREATED_AT = create(TimestampSchema, { unixMicros: 1_758_931_200_000_000n });

function itemType(init: MessageInitShape<typeof ItemTypeSchema>): ItemType {
  return create(ItemTypeSchema, { labId: 'lab-demo', createdAt: CREATED_AT, ...init });
}

function cfd(init: MessageInitShape<typeof CustomFieldDefinitionSchema>): CustomFieldDefinition {
  return create(CustomFieldDefinitionSchema, {
    labId: 'lab-demo',
    scopeKind: ScopeKind.SAMPLE,
    dataType: FieldDataType.TEXT,
    required: false,
    validationJson: '{}',
    indexed: false,
    isPhi: false,
    createdAt: CREATED_AT,
    ...init,
  });
}

/** The demo lab's taxonomy, which every inheritance test descends from. */
const BLOOD = itemType({ id: 'it-blood', name: 'Blood' });
const SERUM = itemType({ id: 'it-serum', parentId: 'it-blood', name: 'Serum' });
const PLASMA = itemType({ id: 'it-plasma', parentId: 'it-blood', name: 'Plasma' });
const TISSUE = itemType({ id: 'it-tissue', parentId: 'it-blood', name: 'Tissue' });
const TREE = [BLOOD, SERUM, PLASMA, TISSUE];

/** Reads a tree into `id@depth:children` lines, depth first. */
function outline(nodes: ReturnType<typeof buildItemTypeTree>['roots']): string[] {
  return nodes.flatMap((node) => [
    `${node.type.id}@${String(node.depth)}:${String(node.children.length)}`,
    ...outline(node.children),
  ]);
}

describe('buildItemTypeTree', () => {
  it('nests children under their parent and numbers the depth from the root', () => {
    const tree = buildItemTypeTree(TREE);

    expect(outline(tree.roots)).toEqual([
      'it-blood@0:3',
      'it-plasma@1:0',
      'it-serum@1:0',
      'it-tissue@1:0',
    ]);
    expect(tree.cyclic).toEqual([]);
    expect(tree.orphaned).toEqual([]);
  });

  it('sorts siblings by name, then id, so the tree is stable across loads', () => {
    const tree = buildItemTypeTree([
      itemType({ id: 'it-z', name: 'Zeta' }),
      itemType({ id: 'it-b', name: 'Alpha' }),
      itemType({ id: 'it-a', name: 'Alpha' }),
    ]);

    expect(tree.roots.map((node) => node.type.id)).toEqual(['it-a', 'it-b', 'it-z']);
  });

  it('renders a node whose parent is not in the loaded set as a root, and names it', () => {
    const orphan = itemType({ id: 'it-orphan', parentId: 'it-missing', name: 'Orphan' });
    const tree = buildItemTypeTree([...TREE, orphan]);

    expect(tree.roots.map((node) => node.type.id)).toContain('it-orphan');
    expect(tree.orphaned.map((node) => node.id)).toEqual(['it-orphan']);
  });

  it('terminates on a parent cycle and reports every node in it', () => {
    // Two admins, two tabs: A is re-parented under B while B is being
    // re-parented under A. A cycle is impossible to *create* through this
    // screen, so the only way it is in the data is from outside it — and the
    // screen still has to render something rather than hang.
    const a = itemType({ id: 'it-a', parentId: 'it-b', name: 'A' });
    const b = itemType({ id: 'it-b', parentId: 'it-a', name: 'B' });

    const tree = buildItemTypeTree([a, b]);

    expect(tree.cyclic.map((node) => node.id).sort()).toEqual(['it-a', 'it-b']);
    // Nothing is silently dropped: a cycle with no root still renders.
    expect(tree.roots.map((node) => node.type.id).sort()).toEqual(['it-a', 'it-b']);
    expect(tree.roots.every((node) => node.children.length === 0)).toBe(true);
  });

  it('keeps the nodes below a cycle attached to it', () => {
    const a = itemType({ id: 'it-a', parentId: 'it-b', name: 'A' });
    const b = itemType({ id: 'it-b', parentId: 'it-a', name: 'B' });
    const leaf = itemType({ id: 'it-leaf', parentId: 'it-b', name: 'Leaf' });

    const tree = buildItemTypeTree([a, b, leaf]);

    expect(tree.cyclic.map((node) => node.id).sort()).toEqual(['it-a', 'it-b']);
    const root = tree.roots.find((node) => node.type.id === 'it-b');
    expect(root?.children.map((node) => node.type.id)).toEqual(['it-leaf']);
  });

  it('walks a thousand-deep chain: the guard is a visited set, not a depth limit', () => {
    // A depth limit would truncate this legitimate chain; the visited set does
    // not care how deep the data is, only whether a node repeats.
    const chain: ItemType[] = [itemType({ id: 'it-0', name: 'Node 0' })];
    for (let index = 1; index < 1000; index += 1) {
      chain.push(
        itemType({
          id: `it-${String(index)}`,
          parentId: `it-${String(index - 1)}`,
          name: `N${String(index)}`,
        }),
      );
    }

    const tree = buildItemTypeTree(chain);
    const lineage = lineageOf(chain, 'it-999');

    expect(tree.roots).toHaveLength(1);
    expect(lineage).toHaveLength(1000);
    expect(lineage[0]).toBe('it-999');
    expect(lineage.at(-1)).toBe('it-0');
  });

  it('is not fooled by an unguarded walk: the control copy does not terminate', () => {
    // The guard's whole job is termination, and this is the demonstration the
    // issue asks for — the same walk without the visited set, run against the
    // same input, with a step budget so the suite cannot hang on it either.
    // 10_000 steps is far more than the 2 nodes the guarded walk visits.
    const cyclic = [
      itemType({ id: 'it-a', parentId: 'it-b', name: 'A' }),
      itemType({ id: 'it-b', parentId: 'it-a', name: 'B' }),
    ];

    /** `lineageOf` with the visited set removed — the shape of the bug. */
    function unguardedLineage(types: readonly ItemType[], start: string, budget: number): number {
      let steps = 0;
      let cursor: string | undefined = start;
      while (cursor !== undefined && steps < budget) {
        steps += 1;
        cursor = types.find((candidate) => candidate.id === cursor)?.parentId;
      }
      return steps;
    }

    expect(unguardedLineage(cyclic, 'it-a', 10_000)).toBe(10_000);
    expect(lineageOf(cyclic, 'it-a')).toEqual(['it-a', 'it-b']);
  });
});

describe('lineageOf', () => {
  it('walks leaf to root and stops at a cycle', () => {
    expect(lineageOf(TREE, 'it-serum')).toEqual(['it-serum', 'it-blood']);
    expect(lineageOf(TREE, 'it-blood')).toEqual(['it-blood']);
    expect(lineageOf(TREE, 'it-nope')).toEqual([]);
  });

  it('stops at a broken link instead of guessing', () => {
    const orphan = itemType({ id: 'it-orphan', parentId: 'it-missing', name: 'Orphan' });
    expect(lineageOf([orphan], 'it-orphan')).toEqual(['it-orphan']);
  });
});

describe('resolveFields', () => {
  const notesOnBlood = cfd({
    id: 'cfd-notes-blood',
    itemTypeId: 'it-blood',
    key: 'notes',
    label: 'Notes',
    validationJson: JSON.stringify({ max_length: 20 }),
  });
  const notesOnSerum = cfd({
    id: 'cfd-notes-serum',
    itemTypeId: 'it-serum',
    key: 'notes',
    label: 'Serum notes',
    validationJson: JSON.stringify({ max_length: 5 }),
  });
  const global = cfd({ id: 'cfd-global', key: 'storage_note', label: 'Storage note' });
  const plasmaOnly = cfd({ id: 'cfd-plasma', itemTypeId: 'it-plasma', key: 'plasma_key' });
  const otherLab = cfd({ id: 'cfd-other-lab', labId: 'lab-second', key: 'other' });
  const boxScoped = cfd({
    id: 'cfd-box',
    itemTypeId: 'it-blood',
    scopeKind: ScopeKind.BOX,
    key: 'box_key',
  });

  const cfds = [notesOnBlood, notesOnSerum, global, plasmaOnly, otherLab, boxScoped];

  it('inherits ancestor and lab-global definitions', () => {
    const fields = resolveFields(cfds, TREE, 'it-serum');
    expect(fields.map((field) => field.cfd.key).sort()).toEqual(['notes', 'storage_note']);
  });

  it('lets the most-derived definition win and says what it shadows', () => {
    const fields = resolveFields(cfds, TREE, 'it-serum');
    const notes = fields.find((field) => field.cfd.key === 'notes');

    expect(notes?.cfd.id).toBe('cfd-notes-serum');
    expect(notes?.origin).toBe('node');
    expect(notes?.tightenedFrom?.id).toBe('cfd-notes-blood');
    expect(notes?.originId).toBe('it-serum');
  });

  it('marks the lab-wide definition as its own origin', () => {
    const fields = resolveFields(cfds, TREE, 'it-serum');
    const storage = fields.find((field) => field.cfd.key === 'storage_note');

    expect(storage?.origin).toBe('lab');
    expect(storage?.originId).toBeNull();
    expect(storage?.tightenedFrom).toBeNull();
  });

  it('does not inherit a sibling branch, another lab, or another scope', () => {
    const keys = resolveFields(cfds, TREE, 'it-serum').map((field) => field.cfd.key);
    expect(keys).not.toContain('plasma_key');
    expect(keys).not.toContain('other');
    expect(keys).not.toContain('box_key');
  });

  it('shows an ancestor definition on the node below it', () => {
    const fields = resolveFields(cfds, TREE, 'it-plasma');
    const storage = fields.find((field) => field.cfd.key === 'storage_note');
    expect(storage?.origin).toBe('lab');
    expect(fields.map((field) => field.cfd.key)).toContain('plasma_key');
  });

  it('terminates on a cyclic lineage instead of hanging', () => {
    const a = itemType({ id: 'it-a', parentId: 'it-b', name: 'A' });
    const b = itemType({ id: 'it-b', parentId: 'it-a', name: 'B' });
    const onA = cfd({ id: 'cfd-a', itemTypeId: 'it-a', key: 'a_key' });

    const fields = resolveFields([onA], [a, b], 'it-a');
    expect(fields.map((field) => field.cfd.key)).toEqual(['a_key']);
  });

  it('breaks a same-node tie the way the server iterates: the last row wins', () => {
    const first = cfd({ id: 'cfd-first', itemTypeId: 'it-blood', key: 'dup', label: 'First' });
    const second = cfd({ id: 'cfd-second', itemTypeId: 'it-blood', key: 'dup', label: 'Second' });

    const fields = resolveFields([first, second], TREE, 'it-serum');
    expect(fields.find((field) => field.cfd.key === 'dup')?.cfd.id).toBe('cfd-second');
  });
});

describe('resolveInheritedFields', () => {
  const onBlood = cfd({ id: 'cfd-blood', itemTypeId: 'it-blood', key: 'notes', label: 'Notes' });
  const onSerum = cfd({
    id: 'cfd-serum',
    itemTypeId: 'it-serum',
    key: 'notes',
    label: 'Serum notes',
  });
  const labWide = cfd({ id: 'cfd-global', key: 'storage_note' });
  const cfds = [onBlood, onSerum, labWide];

  it('leaves the node\u2019s own definition out and keeps the ancestor it shadows', () => {
    // The form has to compare a new definition against the *ancestor* of the
    // same key; on the node itself that row is already the node's own.
    const inherited = resolveInheritedFields(cfds, TREE, 'it-serum');

    expect(inherited.find((field) => field.cfd.key === 'notes')?.cfd.id).toBe('cfd-blood');
    expect(inherited.find((field) => field.cfd.key === 'storage_note')?.origin).toBe('lab');
  });

  it('returns the lab-wide definitions for a root node', () => {
    const inherited = resolveInheritedFields(cfds, TREE, 'it-blood');

    expect(inherited.map((field) => field.cfd.key)).toEqual(['storage_note']);
    expect(inherited[0]?.origin).toBe('lab');
  });
});

describe('tightenViolations', () => {
  it('refuses a child that drops a required parent field', () => {
    const parent = cfd({ id: 'p', key: 'tissue_grade', required: true });
    const child = cfd({ id: 'c', key: 'tissue_grade', required: false });

    expect(tightenViolations(parent, child)).toEqual([
      { code: 'required-dropped', constraint: 'required' },
    ]);
  });

  it('allows a child that makes an optional parent field required', () => {
    // The permissive direction, and the one a "required must match" rule would
    // wrongly refuse: tightening an optional field is exactly what a child is
    // for.
    const parent = cfd({ id: 'p', key: 'notes', required: false });
    const child = cfd({ id: 'c', key: 'notes', required: true });

    expect(tightenViolations(parent, child)).toEqual([]);
  });

  it('allows a child that keeps the parent requirement', () => {
    const parent = cfd({ id: 'p', key: 'notes', required: true });
    const child = cfd({ id: 'c', key: 'notes', required: true });

    expect(tightenViolations(parent, child)).toEqual([]);
  });

  it('allows narrowing a text length and refuses raising or dropping it', () => {
    const parent = cfd({ id: 'p', key: 'notes', validationJson: '{"max_length":20}' });

    expect(
      tightenViolations(parent, cfd({ id: 'c', key: 'notes', validationJson: '{"max_length":5}' })),
    ).toEqual([]);
    expect(
      tightenViolations(
        parent,
        cfd({ id: 'c', key: 'notes', validationJson: '{"max_length":40}' }),
      ),
    ).toEqual([{ code: 'constraint-widened', constraint: 'max_length' }]);
    expect(tightenViolations(parent, cfd({ id: 'c', key: 'notes' }))).toEqual([
      { code: 'constraint-dropped', constraint: 'max_length' },
    ]);
  });

  it('allows narrowing a numeric range and refuses widening or dropping either end', () => {
    const parent = cfd({
      id: 'p',
      key: 'aliquot_count',
      dataType: FieldDataType.INT,
      validationJson: '{"min":1,"max":10}',
    });

    expect(
      tightenViolations(
        parent,
        cfd({
          id: 'c',
          key: 'aliquot_count',
          dataType: FieldDataType.INT,
          validationJson: '{"min":2,"max":4}',
        }),
      ),
    ).toEqual([]);
    expect(
      tightenViolations(
        parent,
        cfd({
          id: 'c',
          key: 'aliquot_count',
          dataType: FieldDataType.INT,
          validationJson: '{"min":0,"max":10}',
        }),
      ),
    ).toEqual([{ code: 'constraint-widened', constraint: 'min' }]);
    expect(
      tightenViolations(
        parent,
        cfd({
          id: 'c',
          key: 'aliquot_count',
          dataType: FieldDataType.INT,
          validationJson: '{"min":1,"max":99}',
        }),
      ),
    ).toEqual([{ code: 'constraint-widened', constraint: 'max' }]);
    expect(
      tightenViolations(
        parent,
        cfd({
          id: 'c',
          key: 'aliquot_count',
          dataType: FieldDataType.INT,
          validationJson: '{"min":1}',
        }),
      ),
    ).toEqual([{ code: 'constraint-dropped', constraint: 'max' }]);
  });

  it('allows a subset of an enum and refuses an added value', () => {
    const parent = cfd({
      id: 'p',
      key: 'tube_type',
      dataType: FieldDataType.ENUM,
      validationJson: '{"values":["EDTA","heparin","plain"]}',
    });

    expect(
      tightenViolations(
        parent,
        cfd({
          id: 'c',
          key: 'tube_type',
          dataType: FieldDataType.ENUM,
          validationJson: '{"values":["EDTA","heparin"]}',
        }),
      ),
    ).toEqual([]);
    expect(
      tightenViolations(
        parent,
        cfd({
          id: 'c',
          key: 'tube_type',
          dataType: FieldDataType.ENUM,
          validationJson: '{"values":["EDTA","citrate"]}',
        }),
      ),
    ).toEqual([{ code: 'constraint-widened', constraint: 'values' }]);
    expect(
      tightenViolations(
        parent,
        cfd({ id: 'c', key: 'tube_type', dataType: FieldDataType.ENUM, validationJson: '{}' }),
      ),
    ).toEqual([{ code: 'constraint-dropped', constraint: 'values' }]);
  });

  it('compares date ranges as the server does, by string', () => {
    const parent = cfd({
      id: 'p',
      key: 'collection_date',
      dataType: FieldDataType.DATE,
      validationJson: '{"min":"2026-01-01","max":"2026-12-31"}',
    });

    expect(
      tightenViolations(
        parent,
        cfd({
          id: 'c',
          key: 'collection_date',
          dataType: FieldDataType.DATE,
          validationJson: '{"min":"2026-06-01","max":"2026-06-30"}',
        }),
      ),
    ).toEqual([]);
    expect(
      tightenViolations(
        parent,
        cfd({
          id: 'c',
          key: 'collection_date',
          dataType: FieldDataType.DATE,
          validationJson: '{"min":"2025-01-01","max":"2026-12-31"}',
        }),
      ),
    ).toEqual([{ code: 'constraint-widened', constraint: 'min' }]);
  });

  it('refuses a changed data type and a dropped PHI marking', () => {
    const parent = cfd({ id: 'p', key: 'donor_name', isPhi: true });
    const child = cfd({ id: 'c', key: 'donor_name', dataType: FieldDataType.INT });

    expect(tightenViolations(parent, child)).toEqual([
      { code: 'data-type-changed', constraint: 'data_type' },
      { code: 'phi-dropped', constraint: 'is_phi' },
    ]);
  });

  it('does not treat turning an index off as a loosening', () => {
    // The index is not a constraint on the value; L10 forces it off when a
    // field becomes PHI, and that must stay legal.
    const parent = cfd({ id: 'p', key: 'notes', indexed: true });
    const child = cfd({ id: 'c', key: 'notes', indexed: false, isPhi: true });

    expect(tightenViolations(parent, child)).toEqual([]);
  });
});

describe('canReparent', () => {
  it('refuses dropping a node onto itself', () => {
    expect(canReparent(TREE, 'it-blood', 'it-blood')).toBe('self');
  });

  it('refuses dropping a node into its own subtree', () => {
    expect(canReparent(TREE, 'it-blood', 'it-serum')).toBe('descendant');
    expect(canReparent(TREE, 'it-blood', 'it-tissue')).toBe('descendant');
    expect(canReparent(TREE, 'it-serum', 'it-plasma')).toBeNull();
  });

  it('accepts a re-parent that moves a node sideways or up to a root', () => {
    expect(canReparent(TREE, 'it-serum', 'it-plasma')).toBeNull();
    expect(canReparent(TREE, 'it-serum', 'it-tissue')).toBeNull();
    expect(canReparent(TREE, 'it-serum', null)).toBeNull();
    expect(canReparent(TREE, 'it-blood', null)).toBeNull();
  });

  it('names a drop on the current parent as a no-op rather than a move', () => {
    expect(canReparent(TREE, 'it-serum', 'it-blood')).toBe('unchanged');
  });

  it('terminates when the existing data already contains a cycle', () => {
    const a = itemType({ id: 'it-a', parentId: 'it-b', name: 'A' });
    const b = itemType({ id: 'it-b', parentId: 'it-a', name: 'B' });
    const below = itemType({ id: 'it-c', parentId: 'it-a', name: 'C' });

    // `it-c` hangs off the cycle. Re-parenting `it-a` under it would close a
    // second loop, and the walk that decides has to end even though neither
    // `it-a` nor `it-b` has a root.
    expect(canReparent([a, b, below], 'it-a', 'it-c')).toBe('descendant');
    // A drop on the current parent stays a no-op even here.
    expect(canReparent([a, b, below], 'it-a', 'it-b')).toBe('unchanged');
    expect(canReparent([a, b, below], 'it-a', null)).toBeNull();
  });
});

describe('parseValidation and serializeValidation', () => {
  it('reads the constraints the server validator implements', () => {
    expect(parseValidation('{"max_length":20}')).toEqual({ maxLength: 20 });
    expect(parseValidation('{"min":1,"max":10}')).toEqual({ min: 1, max: 10 });
    expect(parseValidation('{"min":"2026-01-01","max":"2026-12-31"}')).toEqual({
      min: '2026-01-01',
      max: '2026-12-31',
    });
    expect(parseValidation('{"values":["EDTA","heparin"]}')).toEqual({
      values: ['EDTA', 'heparin'],
    });
  });

  it('ignores malformed JSON and unknown keys rather than throwing', () => {
    expect(parseValidation('{not json')).toEqual({});
    expect(parseValidation('')).toEqual({});
    expect(parseValidation('{"future_constraint":true}')).toEqual({});
  });

  it('writes only the constraints that apply to the data type', () => {
    expect(serializeValidation({ maxLength: 20, min: 1 }, FieldDataType.TEXT)).toBe(
      '{"max_length":20}',
    );
    expect(serializeValidation({ values: ['a', 'b'] }, FieldDataType.ENUM)).toBe(
      '{"values":["a","b"]}',
    );
    expect(serializeValidation({}, FieldDataType.TEXT)).toBe('{}');
  });

  it('keeps an unknown constraint it cannot represent, so saving does not erase it', () => {
    // The editor knows the constraints `custom_field_validator.h` implements.
    // A newer server may store one this bundle has never heard of; dropping it
    // on save would silently change validation on a field the user only renamed.
    expect(
      serializeValidation({ maxLength: 5 }, FieldDataType.TEXT, '{"future":true,"max_length":20}'),
    ).toBe('{"future":true,"max_length":5}');
  });

  it('drops constraints that do not apply once the type changes', () => {
    expect(serializeValidation({ min: 1, max: 2 }, FieldDataType.INT, '{"max_length":20}')).toBe(
      '{"min":1,"max":2}',
    );
  });
});

describe('definitionProblems', () => {
  const base = { key: 'patient_id', label: 'Patient id', dataType: FieldDataType.TEXT };

  it('refuses is_phi together with indexed, with the L10 reason', () => {
    expect(
      definitionProblems({ ...base, isPhi: true, indexed: true }, { phiEnabled: true }),
    ).toEqual([{ code: 'phi-and-indexed' }]);
  });

  it('refuses is_phi when the lab has PHI mode off', () => {
    expect(definitionProblems({ ...base, isPhi: true }, { phiEnabled: false })).toEqual([
      { code: 'phi-not-enabled' },
    ]);
  });

  it('allows a PHI field in a PHI lab that is not indexed', () => {
    expect(definitionProblems({ ...base, isPhi: true }, { phiEnabled: true })).toEqual([]);
  });

  it('requires a key and a label', () => {
    expect(definitionProblems({ ...base, key: '  ' }, { phiEnabled: true })).toEqual([
      { code: 'key-required' },
    ]);
    expect(definitionProblems({ ...base, label: '' }, { phiEnabled: true })).toEqual([
      { code: 'label-required' },
    ]);
  });

  it('requires enum values and a usable range', () => {
    expect(
      definitionProblems(
        { ...base, dataType: FieldDataType.ENUM, values: [] },
        { phiEnabled: true },
      ),
    ).toEqual([{ code: 'enum-values-required' }]);
    expect(
      definitionProblems(
        { ...base, dataType: FieldDataType.ENUM, values: ['EDTA', 'EDTA'] },
        { phiEnabled: true },
      ),
    ).toEqual([{ code: 'enum-values-duplicated' }]);
    expect(
      definitionProblems(
        { ...base, dataType: FieldDataType.INT, min: 5, max: 1 },
        { phiEnabled: true },
      ),
    ).toEqual([{ code: 'range-inverted' }]);
  });

  it('accepts a plain definition', () => {
    expect(definitionProblems(base, { phiEnabled: false })).toEqual([]);
  });
});
