// SPDX-License-Identifier: AGPL-3.0-or-later
import {
  flexRender,
  stockFeatures,
  useTable,
  type Column,
  type ColumnDef,
  type ColumnVisibilityState,
  type RowData,
} from '@tanstack/react-table';
import { useVirtualizer } from '@tanstack/react-virtual';
import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Checkbox } from './Checkbox';
import { VisuallyHidden } from './VisuallyHidden';
import styles from './Table.module.css';
import { classNames } from './classNames';

/** The feature set G1.3 turns on: sorting, filtering, visibility, sizing. */
export type TableFeatures = typeof stockFeatures;

/** What a feature task writes when it hands columns to `Table`. */
export type TableColumn<TData extends RowData> = ColumnDef<TableFeatures, TData>;

export interface TableProps<TData extends RowData> {
  /** Accessible name of the table, rendered as its `<caption>`. */
  caption: string;
  columns: TableColumn<TData>[];
  data: TData[];
  /** Stable row identity; without it React reuses rows across data changes. */
  getRowId: (row: TData) => string;
  /** Shown in place of the rows when `data` is empty. */
  emptyMessage?: string;
  /** Height of the scrolling viewport in px. */
  maxHeight?: number;
  /** Estimated row height in px; the virtualizer corrects it once it measures. */
  rowHeight?: number;
  /** Turn off only for short, fixed lists that must all be in the DOM. */
  virtualized?: boolean;
  /** Shows the column picker. Off for tables whose shape is fixed. */
  enableColumnVisibility?: boolean;
  /**
   * Called when the windowed rows reach the end of `data` — the seam an
   * infinite list uses to fetch its next page (G3.2). It fires on the
   * *transition* into "the window covers the end", not on every render, and it
   * is only meaningful for a virtualized table: a table with virtualization
   * off has every row on screen by definition.
   *
   * It says nothing about whether more rows exist — that is the caller's
   * `hasNextPage` — so guard the fetch with both that and "not already
   * fetching".
   */
  onEndReached?: () => void;
  /** Rows from the end at which `onEndReached` fires. */
  endReachedThreshold?: number;
  className?: string;
}

const DEFAULT_MAX_HEIGHT = 480;
const DEFAULT_ROW_HEIGHT = 40;
const OVERSCAN = 8;
const DEFAULT_END_REACHED_THRESHOLD = 5;

function columnTitle<TData extends RowData>(column: Column<TableFeatures, TData>): string {
  const header: unknown = column.columnDef.header;
  return typeof header === 'string' ? header : column.id;
}

/**
 * The grid every list screen is built on (G-arch 1: TanStack Table + TanStack
 * Virtual).
 *
 * Three things it deliberately does:
 *
 * - **Virtualizes.** A lab has 100k samples; the Qt client scans them all in
 *   memory and a browser cannot. Only the visible window is in the DOM.
 * - **Keeps a real `<table>`.** The rows are windowed and the gaps are filled
 *   with spacer rows rather than `position: absolute`, so column alignment,
 *   `<th scope>`, the row/column-header roles and `position: sticky` on the
 *   header all keep working. A `div` grid would have to rebuild all of that.
 * - **Column visibility.** Stored in component state, not `localStorage`: the
 *   column layout is per-screen, and G-arch 7 only allows UI preferences —
 *   the selected lab id and column layout — to be persisted, which is a
 *   feature task's call, not the shell's.
 */
export function Table<TData extends RowData>({
  caption,
  columns,
  data,
  getRowId,
  emptyMessage,
  maxHeight = DEFAULT_MAX_HEIGHT,
  rowHeight = DEFAULT_ROW_HEIGHT,
  virtualized = true,
  enableColumnVisibility = true,
  onEndReached,
  endReachedThreshold = DEFAULT_END_REACHED_THRESHOLD,
  className,
}: TableProps<TData>) {
  const { t } = useTranslation('ui');
  const scrollRef = useRef<HTMLDivElement>(null);
  const [columnVisibility, setColumnVisibility] = useState<ColumnVisibilityState>({});

  const table = useTable<TableFeatures, TData>({
    features: stockFeatures,
    columns,
    data,
    getRowId,
    state: { columnVisibility },
    onColumnVisibilityChange: setColumnVisibility,
  });

  const rows = table.getRowModel().rows;
  const leafColumns = table.getAllLeafColumns();

  const virtualizer = useVirtualizer({
    count: rows.length,
    getScrollElement: () => scrollRef.current,
    estimateSize: () => rowHeight,
    overscan: OVERSCAN,
    // jsdom (and the first paint before layout) measures the scroll element as
    // 0 px tall, which would window the list down to nothing. Seeding the rect
    // keeps the virtualization correct in tests and identical in the browser,
    // where the real measurement replaces it on the next frame.
    initialRect: { width: 0, height: maxHeight },
  });

  const virtualItems = virtualized ? virtualizer.getVirtualItems() : undefined;
  const visibleRows =
    virtualItems === undefined
      ? rows
      : virtualItems.flatMap((item) => (rows[item.index] ? [rows[item.index]] : []));
  const paddingTop = virtualItems?.[0]?.start ?? 0;
  const paddingBottom =
    virtualItems && virtualItems.length > 0
      ? Math.max(0, virtualizer.getTotalSize() - (virtualItems.at(-1)?.end ?? 0))
      : 0;

  const lastWindowedIndex = virtualItems?.at(-1)?.index;
  const atEnd =
    lastWindowedIndex !== undefined &&
    rows.length > 0 &&
    lastWindowedIndex >= rows.length - 1 - endReachedThreshold;

  // The callback is read through a ref updated in an effect declared *before*
  // the trigger, so `onEndReached` fires on the transition into "at the end"
  // and not again on every render the caller happens to re-create it on.
  const onEndReachedRef = useRef(onEndReached);
  useEffect(() => {
    onEndReachedRef.current = onEndReached;
  });

  useEffect(() => {
    if (atEnd) {
      onEndReachedRef.current?.();
    }
  }, [atEnd]);

  return (
    <div className={classNames(styles.root, className)}>
      {enableColumnVisibility ? (
        <details className={styles.columnPicker}>
          <summary className={styles.columnPickerSummary}>{t('table.columns')}</summary>
          <div className={styles.columnList} role="group" aria-label={t('table.columns')}>
            {leafColumns.map((column) => (
              <Checkbox
                key={column.id}
                label={t('table.toggleColumn', { column: columnTitle(column) })}
                checked={column.getIsVisible()}
                disabled={!column.getCanHide()}
                onChange={(event) => {
                  column.toggleVisibility(event.target.checked);
                }}
              />
            ))}
          </div>
        </details>
      ) : null}

      <div className={styles.scroll} ref={scrollRef} style={{ maxHeight }}>
        <table className={styles.table}>
          <caption className={styles.caption}>{caption}</caption>
          <thead className={styles.head}>
            {table.getHeaderGroups().map((headerGroup) => (
              <tr key={headerGroup.id}>
                {headerGroup.headers.map((header) => (
                  <th key={header.id} scope="col" className={styles.cell}>
                    {header.isPlaceholder
                      ? null
                      : flexRender(header.column.columnDef.header, header.getContext())}
                  </th>
                ))}
              </tr>
            ))}
          </thead>
          <tbody>
            {rows.length === 0 ? (
              <tr>
                <td className={styles.empty} colSpan={Math.max(leafColumns.length, 1)}>
                  {emptyMessage ?? t('table.empty')}
                </td>
              </tr>
            ) : (
              <>
                {paddingTop > 0 ? (
                  <SpacerRow height={paddingTop} colSpan={leafColumns.length} />
                ) : null}
                {visibleRows.map((row) => (
                  <tr key={row.id} className={styles.row}>
                    {row.getVisibleCells().map((cell) => (
                      <td key={cell.id} className={styles.cell}>
                        {flexRender(cell.column.columnDef.cell, cell.getContext())}
                      </td>
                    ))}
                  </tr>
                ))}
                {paddingBottom > 0 ? (
                  <SpacerRow height={paddingBottom} colSpan={leafColumns.length} />
                ) : null}
              </>
            )}
          </tbody>
        </table>
      </div>
      {virtualized ? (
        <VisuallyHidden>
          {t('table.windowed', { shown: visibleRows.length, total: rows.length })}
        </VisuallyHidden>
      ) : null}
    </div>
  );
}

/** Pads the tbody so the scrollbar reflects the full data set, not the window. */
function SpacerRow({ height, colSpan }: { height: number; colSpan: number }) {
  return (
    <tr aria-hidden="true">
      <td className={styles.spacer} colSpan={Math.max(colSpan, 1)} style={{ height }} />
    </tr>
  );
}
