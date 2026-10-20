// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import i18n from '../../app/i18n';
import {
  CustomFieldDefinitionSchema,
  FieldDataType,
  ItemTypeSchema,
} from '../../gen/fmgr/v1/item_type_pb';
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
});
