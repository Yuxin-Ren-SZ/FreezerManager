// SPDX-License-Identifier: AGPL-3.0-or-later
import type { TFunction } from 'i18next';
import { Link } from 'react-router-dom';
import { enumLabel, formatTimestamp } from '../../api/helpers';
import type { CustomFieldDefinition, ItemType } from '../../gen/fmgr/v1/item_type_pb';
import { SampleStatus, SampleStatusSchema, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { Badge, type BadgeTone, type TableColumn } from '../../ui';
import type { LocationPath } from '../layout/layoutModel';
import { formatCustomFieldValue, parseCustomFields } from './customFields';
import { placementKind, placementPath } from './placement';

/**
 * The columns of the sample browser (TODO.md G3.2).
 *
 * Two decisions worth knowing:
 *
 *  - **Custom fields are columns, and their set comes from the server.** Each
 *    `CustomFieldDefinition` becomes one column keyed `field:<key>`; a row that
 *    does not carry the value shows a dash rather than an empty cell, because
 *    "this row has no value" and "this row's value is the empty string" are
 *    different things to a scientist reading the table.
 *  - **Placement has three states, not two** (`placement.ts`): a cell must not
 *    call a broken location "never placed".
 *
 * Every user-visible string comes from the `samples` namespace, except the
 * status labels, which come from `common`'s `enums.SampleStatus.*` through the
 * shared `enumLabel` helper.
 */

/** A dash for a value the row does not carry. A glyph, not copy. */
const EMPTY_CELL = '\u2014';

/** Lifecycle state to badge tone; an unknown state stays neutral. */
const STATUS_TONE: Readonly<Record<number, BadgeTone>> = {
  [SampleStatus.ACTIVE]: 'success',
  [SampleStatus.CHECKED_OUT]: 'info',
  [SampleStatus.DEPLETED]: 'warning',
  [SampleStatus.DESTROYED]: 'neutral',
  [SampleStatus.TOMBSTONED]: 'danger',
};

export interface SampleColumnsOptions {
  readonly labId: string;
  /** `samples` namespace. */
  readonly t: TFunction;
  /** The default namespace, for `enums.*`. */
  readonly tEnums: TFunction;
  readonly itemTypes: readonly ItemType[];
  /** The lab's field definitions; one column each, in server order. */
  readonly cfds: readonly CustomFieldDefinition[];
  readonly locationPath: (boxId: string, position?: string) => LocationPath;
}

/** `field:concentration` — stable, and distinct from a built-in column id. */
export function customFieldColumnId(key: string): string {
  return `field:${key}`;
}

/**
 * The column ids, in order. Exported so a test can assert the shape — including
 * that a custom field became a column — without rendering a table.
 */
export function sampleColumnIds(options: Pick<SampleColumnsOptions, 'cfds'>): string[] {
  return [
    'name',
    'barcode',
    'status',
    'itemType',
    'location',
    'volume',
    'created',
    ...options.cfds.map((cfd) => customFieldColumnId(cfd.key)),
  ];
}

export function buildSampleColumns({
  labId,
  t,
  tEnums,
  itemTypes,
  cfds,
  locationPath,
}: SampleColumnsOptions): TableColumn<Sample>[] {
  const itemTypesById = new Map(itemTypes.map((itemType) => [itemType.id, itemType]));

  const placement = (sample: Sample): string => {
    const path = locationPath(sample.boxId ?? '', sample.positionLabel ?? '');
    switch (placementKind(path)) {
      case 'placed':
        return placementPath(path);
      case 'unknown':
        return t('placement.unknown');
      case 'unplaced':
        return t('placement.unplaced');
    }
  };

  return [
    {
      id: 'name',
      accessorKey: 'name',
      header: t('columns.name'),
      cell: ({ row }) => (
        <Link to={`/labs/${encodeURIComponent(labId)}/samples/${encodeURIComponent(row.original.id)}`}>
          {row.original.name}
        </Link>
      ),
    },
    {
      id: 'barcode',
      accessorKey: 'barcode',
      header: t('columns.barcode'),
      cell: ({ row }) => row.original.barcode ?? EMPTY_CELL,
    },
    {
      id: 'status',
      accessorKey: 'status',
      header: t('columns.status'),
      cell: ({ row }) => (
        <Badge tone={STATUS_TONE[row.original.status] ?? 'neutral'}>
          {enumLabel(tEnums, SampleStatusSchema, row.original.status)}
        </Badge>
      ),
    },
    {
      id: 'itemType',
      accessorKey: 'itemTypeId',
      header: t('columns.itemType'),
      // The id is the honest fallback: the item-type list is a separate query
      // and may have failed or not loaded yet, and a blank cell would read as
      // "this sample has no item type".
      cell: ({ row }) =>
        itemTypesById.get(row.original.itemTypeId)?.name ?? row.original.itemTypeId,
    },
    {
      id: 'location',
      header: t('columns.location'),
      cell: ({ row }) => placement(row.original),
    },
    {
      id: 'volume',
      header: t('columns.volume'),
      cell: ({ row }) => {
        const { volumeValue, volumeUnit } = row.original;
        if (volumeValue === undefined) {
          return EMPTY_CELL;
        }
        return `${String(volumeValue)} ${volumeUnit}`.trim();
      },
    },
    {
      id: 'created',
      header: t('columns.created'),
      cell: ({ row }) => formatTimestamp(row.original.createdAt?.unixMicros) ?? EMPTY_CELL,
    },
    ...cfds.map((cfd) => ({
      id: customFieldColumnId(cfd.key),
      header: cfd.isPhi ? t('columns.phi', { label: cfd.label }) : cfd.label,
      cell: ({ row }: { row: { original: Sample } }) => {
        const value = formatCustomFieldValue(parseCustomFields(row.original.customFieldsJson)[cfd.key]);
        return value === '' ? EMPTY_CELL : value;
      },
    })),
  ];
}
