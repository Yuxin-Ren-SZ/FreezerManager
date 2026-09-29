// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import i18n from '../../app/i18n';
import { CustomFieldDefinitionSchema, FieldDataType } from '../../gen/fmgr/v1/item_type_pb';
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

function cfd(key: string, label: string, isPhi = false) {
  return create(CustomFieldDefinitionSchema, {
    id: `cfd-${key}`,
    labId: 'lab-demo',
    key,
    label,
    dataType: FieldDataType.TEXT,
    isPhi,
  });
}

describe('sampleColumnIds', () => {
  it('is the built-in columns plus one per custom-field definition', () => {
    expect(sampleColumnIds({ cfds: [cfd('concentration', 'Concentration')] })).toEqual([
      'name',
      'barcode',
      'status',
      'itemType',
      'location',
      'volume',
      'created',
      customFieldColumnId('concentration'),
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
    cfds: [cfd('concentration', 'Concentration'), cfd('mrn', 'Record number', true)],
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
