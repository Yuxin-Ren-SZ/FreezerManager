// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import i18n from '../../app/i18n';
import {
  CustomFieldDefinitionSchema,
  FieldDataType,
  ItemTypeSchema,
  ScopeKind,
} from '../../gen/fmgr/v1/item_type_pb';
import { SampleSchema } from '../../gen/fmgr/v1/sample_pb';
import { buildSampleColumns, customFieldColumnId, sampleColumnIds } from './sampleColumns';

/**
 * The column set of the sample browser (TODO.md G3.2).
 *
 * What matters here is the *derivation*: one column per custom-field definition,
 * in the order the server sent them, with a PHI field marked as such because its
 * value only appears when the server chose to disclose it (G-arch 7).
 */

const t = i18n.getFixedT('en', 'samples');
const tEnums = i18n.getFixedT('en');

function cfd(key: string, label: string, isPhi = false, itemTypeId?: string) {
  return create(CustomFieldDefinitionSchema, {
    id: `cfd-${itemTypeId ?? 'lab'}-${key}`,
    labId: 'lab-demo',
    key,
    label,
    dataType: FieldDataType.TEXT,
    scopeKind: ScopeKind.SAMPLE,
    isPhi,
    ...(itemTypeId === undefined ? {} : { itemTypeId }),
  });
}

describe('sampleColumnIds', () => {
  it('is the built-in columns plus one per custom-field definition', () => {
    expect(
      sampleColumnIds({ cfds: [cfd('concentration', 'Concentration', false, 'it-serum')] }),
    ).toEqual([
      'name',
      'barcode',
      'status',
      'itemType',
      'location',
      'volume',
      'created',
      customFieldColumnId('it-serum', 'concentration'),
    ]);
  });

  it('is only the built-in columns without definitions', () => {
    // No definitions came back: the lab has none, or the caller cannot read
    // them. Since #69 that read is gated on `sample.read`, so this is *not* the
    // read-only member's case — see `SampleBrowserScreen.test.tsx`.
    expect(sampleColumnIds({ cfds: [] })).toHaveLength(7);
  });
});

describe('buildSampleColumns', () => {
  const options = {
    labId: 'lab-demo',
    t,
    tEnums,
    itemTypes: [],
    cfds: [
      cfd('concentration', 'Concentration', false, 'it-serum'),
      cfd('mrn', 'Record number', true, 'it-serum'),
    ],
    locationPath: () => ({ placed: false, partial: false, segments: [] }),
  };

  it('labels each column from the copy, not from the field key', () => {
    const headers = buildSampleColumns(options).map((column) => column.header);

    expect(headers).toEqual([
      t('columns.name'),
      t('columns.barcode'),
      t('columns.status'),
      t('columns.itemType'),
      t('columns.location'),
      t('columns.volume'),
      t('columns.created'),
      'Concentration',
      t('columns.phi', { label: 'Record number' }),
    ]);
  });

  it('marks a PHI definition, because its value is only there when disclosed', () => {
    const phi = buildSampleColumns(options).at(-1);
    const plain = buildSampleColumns(options).at(-2);

    expect(String(phi?.header)).toContain('PHI');
    expect(String(plain?.header)).not.toContain('PHI');
  });

  it('gives every column a unique id, which is what the chooser toggles', () => {
    const ids = buildSampleColumns(options).map((column) => column.id);

    expect(new Set(ids).size).toBe(ids.length);
  });
});

describe('custom-field columns on more than one item type', () => {
  const itemTypes = [
    create(ItemTypeSchema, { id: 'it-tissue', labId: 'lab-demo', name: 'Tissue' }),
    create(ItemTypeSchema, { id: 'it-blood', labId: 'lab-demo', name: 'Blood' }),
  ];

  // The shape the demo audit reproduced: `patient_id` and `collection_date`
  // are both defined on two item types, so a `field:<key>` id collides twice.
  const sharedKeys = [
    cfd('patient_id', 'Patient ID', false, 'it-tissue'),
    cfd('patient_id', 'Patient ID', false, 'it-blood'),
    cfd('collection_date', 'Collection date', false, 'it-tissue'),
    cfd('collection_date', 'Collection date', false, 'it-blood'),
    cfd('stain', 'Stain', false, 'it-tissue'),
  ];

  const options = {
    labId: 'lab-demo',
    t,
    tEnums,
    itemTypes,
    cfds: sharedKeys,
    locationPath: () => ({ placed: false, partial: false, segments: [] }),
  };

  it('keys a column by item type and field key, so two types cannot collide', () => {
    const ids = sampleColumnIds({ cfds: sharedKeys });

    expect(ids.slice(7)).toEqual([
      'field:it-tissue:patient_id',
      'field:it-blood:patient_id',
      'field:it-tissue:collection_date',
      'field:it-blood:collection_date',
      'field:it-tissue:stain',
    ]);
    expect(new Set(ids).size).toBe(ids.length);
  });

  it('names the item type in the header only when a key is shared', () => {
    const headers = buildSampleColumns(options).map((column) => column.header);

    expect(headers.slice(7)).toEqual([
      t('columns.withItemType', { label: 'Patient ID', itemType: 'Tissue' }),
      t('columns.withItemType', { label: 'Patient ID', itemType: 'Blood' }),
      t('columns.withItemType', { label: 'Collection date', itemType: 'Tissue' }),
      t('columns.withItemType', { label: 'Collection date', itemType: 'Blood' }),
      // `stain` exists on one type only: adding the type would be noise.
      'Stain',
    ]);
  });

  it('marks a shared PHI key with both the item type and the PHI marker', () => {
    const phi = [
      cfd('donor_name', 'Donor name', true, 'it-tissue'),
      cfd('donor_name', 'Donor name', true, 'it-blood'),
    ];
    const headers = buildSampleColumns({ ...options, cfds: phi }).map((column) => column.header);

    expect(headers.slice(7)).toEqual([
      t('columns.phiWithType', { label: 'Donor name', itemType: 'Tissue' }),
      t('columns.phiWithType', { label: 'Donor name', itemType: 'Blood' }),
    ]);
  });

  it('falls back to the item-type id while the item-type list is missing', () => {
    const headers = buildSampleColumns({ ...options, itemTypes: [] }).map(
      (column) => column.header,
    );

    expect(headers[7]).toBe(
      t('columns.withItemType', { label: 'Patient ID', itemType: 'it-tissue' }),
    );
  });

  it('shows a shared key only under the winning unrelated root type', () => {
    const serum = create(SampleSchema, {
      id: 'sample-serum',
      itemTypeId: 'it-tissue',
      customFieldsJson: '{"patient_id":"T-123"}',
    });
    const blood = create(SampleSchema, {
      id: 'sample-blood',
      itemTypeId: 'it-blood',
      customFieldsJson: '{"patient_id":"B-456"}',
    });
    const columns = buildSampleColumns(options);
    const tissueColumn = columns.find((column) => column.id === 'field:it-tissue:patient_id');
    const bloodColumn = columns.find((column) => column.id === 'field:it-blood:patient_id');

    expect(renderCustomFieldCell(tissueColumn, serum)).toBe('T-123');
    expect(renderCustomFieldCell(bloodColumn, serum)).toBe('—');
    expect(renderCustomFieldCell(tissueColumn, blood)).toBe('—');
    expect(renderCustomFieldCell(bloodColumn, blood)).toBe('B-456');
  });

  it('shows inherited and lab-wide values under their winning definitions and child overrides', () => {
    const hierarchy = [
      create(ItemTypeSchema, { id: 'it-root', labId: 'lab-demo', name: 'Root' }),
      create(ItemTypeSchema, {
        id: 'it-child',
        labId: 'lab-demo',
        name: 'Child',
        parentId: 'it-root',
      }),
    ];
    const definitions = [
      cfd('storage_note', 'Storage note'),
      cfd('root_code', 'Root code', false, 'it-root'),
      cfd('shared_code', 'Root code', false, 'it-root'),
      cfd('shared_code', 'Child code', false, 'it-child'),
    ];
    const sample = create(SampleSchema, {
      id: 'sample-child',
      itemTypeId: 'it-child',
      customFieldsJson: '{"storage_note":"global","root_code":"inherited","shared_code":"child"}',
    });
    const columns = buildSampleColumns({ ...options, itemTypes: hierarchy, cfds: definitions });

    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field::storage_note'),
        sample,
      ),
    ).toBe('global');
    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field:it-root:root_code'),
        sample,
      ),
    ).toBe('inherited');
    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field:it-root:shared_code'),
        sample,
      ),
    ).toBe('—');
    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field:it-child:shared_code'),
        sample,
      ),
    ).toBe('child');
  });

  it('leaves type-specific cells empty when a sample type is unknown', () => {
    const unknown = create(SampleSchema, {
      id: 'sample-unknown',
      itemTypeId: 'it-missing',
      customFieldsJson: '{"patient_id":"unknown"}',
    });
    const columns = buildSampleColumns(options);

    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field:it-tissue:patient_id'),
        unknown,
      ),
    ).toBe('—');
    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field:it-blood:patient_id'),
        unknown,
      ),
    ).toBe('—');
  });

  it('bounds applicability to the visited types when ancestry is cyclic', () => {
    const cyclicTypes = [
      create(ItemTypeSchema, {
        id: 'it-a',
        labId: 'lab-demo',
        name: 'A',
        parentId: 'it-b',
      }),
      create(ItemTypeSchema, {
        id: 'it-b',
        labId: 'lab-demo',
        name: 'B',
        parentId: 'it-a',
      }),
      ...itemTypes,
    ];
    const definitions = [
      cfd('shared', 'A field', false, 'it-a'),
      cfd('shared', 'B field', false, 'it-b'),
      cfd('shared', 'Unrelated field', false, 'it-tissue'),
    ];
    const sample = create(SampleSchema, {
      id: 'sample-cycle',
      itemTypeId: 'it-a',
      customFieldsJson: '{"shared":"cycle-value"}',
    });
    const columns = buildSampleColumns({ ...options, itemTypes: cyclicTypes, cfds: definitions });

    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field:it-a:shared'),
        sample,
      ),
    ).toBe('cycle-value');
    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field:it-b:shared'),
        sample,
      ),
    ).toBe('—');
    expect(
      renderCustomFieldCell(
        columns.find((column) => column.id === 'field:it-tissue:shared'),
        sample,
      ),
    ).toBe('—');
  });
});

function renderCustomFieldCell(
  column: ReturnType<typeof buildSampleColumns>[number] | undefined,
  sample: ReturnType<typeof create<typeof SampleSchema>>,
): string | undefined {
  if (column === undefined || typeof column.cell !== 'function') return undefined;
  const cell = column.cell as unknown as (context: { row: { original: typeof sample } }) => unknown;
  return String(cell({ row: { original: sample } }));
}
