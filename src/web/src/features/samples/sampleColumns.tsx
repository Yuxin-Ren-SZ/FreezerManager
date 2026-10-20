// SPDX-License-Identifier: AGPL-3.0-or-later
import type { TFunction } from 'i18next';
import { Link } from 'react-router-dom';
import { enumLabel, formatTimestamp } from '../../api/helpers';
import type { CustomFieldDefinition, ItemType } from '../../gen/fmgr/v1/item_type_pb';
import { SampleStatus, SampleStatusSchema, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { Badge, type BadgeTone } from '../../ui';
// Same reason as `Table` itself: it is imported from its module, not the
// barrel, so a table screen does not drag TanStack into the entry chunk.
import type { TableColumn } from '../../ui/Table';
import type { LocationPath } from '../layout/layoutModel';
import { formatCustomFieldValue, parseCustomFields } from './customFields';
import { placementKind, placementPath } from './placement';

/**
 * The columns of the sample browser (TODO.md G3.2).
 *
 * Two decisions worth knowing:
 *
 *  - **Custom fields are columns, and their set comes from the server.** Each
 *    `CustomFieldDefinition` becomes one column keyed by **item type and key**
 *    (`field:<item_type_id>:<key>`), because the server returns a definition
 *    per item type: a key used on five item types is five columns, and keying
 *    by the key alone made React see five children with one id (#139). The
 *    header names the item type when a key is shared, so the user can tell the
 *    columns apart. A row that does not carry the value shows a dash rather
 *    than an empty cell, because "this row has no value" and "this row's value
 *    is the empty string" are different things to a scientist reading the
 *    table.
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
/** Partial on purpose: a newer server may send a status this bundle does not know. */
const STATUS_TONE: Readonly<Partial<Record<number, BadgeTone>>> = {
  [SampleStatus.ACTIVE]: 'success',
  [SampleStatus.CHECKED_OUT]: 'info',
  [SampleStatus.DEPLETED]: 'warning',
  [SampleStatus.DESTROYED]: 'neutral',
  [SampleStatus.TOMBSTONED]: 'danger',
};

export interface SampleColumnsOptions {
  readonly labId: string;
  /** The `samples` namespace, so a missing key is a type error. */
  readonly t: TFunction<'samples'>;
  /** The default namespace, for `enums.SampleStatus.*`. */
  readonly tEnums: TFunction;
  readonly itemTypes: readonly ItemType[];
  /** The lab's field definitions; one column each, in server order. */
  readonly cfds: readonly CustomFieldDefinition[];
  readonly locationPath: (boxId: string, position?: string) => LocationPath;
}

/**
 * `field:<item_type_id>:<key>` — stable, distinct from a built-in column id,
 * and distinct per item type. The item type is part of the identity because the
 * server returns a definition per item type; `field:<key>` collided whenever a
 * key was defined on more than one (#139).
 */
export function customFieldColumnId(itemTypeId: string, key: string): string {
  return `field:${itemTypeId}:${key}`;
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
    ...options.cfds.map((cfd) => customFieldColumnId(cfd.itemTypeId ?? '', cfd.key)),
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

  // A key is shared when more than one definition carries it — normally one per
  // item type. Only a shared key needs its type in the header; a field that
  // exists on one type stays just its label.
  const definitionsByKey = new Map<string, number>();
  for (const cfd of cfds) {
    definitionsByKey.set(cfd.key, (definitionsByKey.get(cfd.key) ?? 0) + 1);
  }

  const itemTypeName = (itemTypeId: string | undefined): string =>
    itemTypesById.get(itemTypeId ?? '')?.name ??
    (itemTypeId === undefined || itemTypeId === '' ? t('columns.allItemTypes') : itemTypeId);

  const customFieldHeader = (cfd: CustomFieldDefinition): string => {
    if ((definitionsByKey.get(cfd.key) ?? 0) === 1) {
      return cfd.isPhi ? t('columns.phi', { label: cfd.label }) : cfd.label;
    }
    return cfd.isPhi
      ? t('columns.phiWithType', { label: cfd.label, itemType: itemTypeName(cfd.itemTypeId) })
      : t('columns.withItemType', { label: cfd.label, itemType: itemTypeName(cfd.itemTypeId) });
  };

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
        <Link
          to={`/labs/${encodeURIComponent(labId)}/samples/${encodeURIComponent(row.original.id)}`}
        >
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
        // A value without a unit is still a value: "12.5", not "12.5 undefined".
        const value = String(volumeValue);
        return volumeUnit === undefined || volumeUnit === '' ? value : `${value} ${volumeUnit}`;
      },
    },
    {
      id: 'created',
      header: t('columns.created'),
      cell: ({ row }) => formatTimestamp(row.original.createdAt?.unixMicros) ?? EMPTY_CELL,
    },
    ...cfds.map((cfd) => ({
      id: customFieldColumnId(cfd.itemTypeId ?? '', cfd.key),
      header: customFieldHeader(cfd),
      cell: ({ row }: { row: { original: Sample } }) => {
        const value = formatCustomFieldValue(
          parseCustomFields(row.original.customFieldsJson)[cfd.key],
        );
        return value === '' ? EMPTY_CELL : value;
      },
    })),
  ];
}
