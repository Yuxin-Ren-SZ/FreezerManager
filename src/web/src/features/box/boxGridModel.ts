// SPDX-License-Identifier: AGPL-3.0-or-later
import type { BoxPosition, BoxType } from '../../gen/fmgr/v1/box_pb';
import { SampleStatus, type Sample } from '../../gen/fmgr/v1/sample_pb';

/**
 * The box grid as a value (TODO.md G3.4, PRD §9 / F6.3).
 *
 * Deliberately pure and React-free, like G3.1's `layoutModel`: the box screen
 * renders it, the print sheet renders it again, and the rules that are easy to
 * get wrong live where a unit test can reach them without mounting anything.
 *
 * **Positions come from the box type, and only from it.** A `BoxPosition`
 * carries its own `label`, `row` and `col`, and they are taken as given:
 *
 *  - the D4.2 mixed template (`data/seed/box_types/mixed_eppendorf.json`) is a
 *    3×3 block next to a 2×2 one at columns 3–4, so its rectangle is 3×5 with
 *    **two holes**. Those holes are cells in the layout — the columns have to
 *    line up — but they are `null`, not positions: nothing can be dropped on
 *    them and no label is invented for them;
 *  - rows and columns are numbered from **0** in the shipped templates and from
 *    **1** in `src/test/fakeApi.ts`'s `seedPositions`. Deriving a label from an
 *    index (`String.fromCharCode(64 + row)`) therefore renders the wrong map for
 *    one of the two, which is why the label is never derived;
 *  - a sample whose `position_label` the box type does not declare is **not
 *    dropped on the floor**. It comes back in `unplaced` so the screen can say
 *    so. A box view that silently hides a stored sample is worse than one that
 *    admits it cannot place it.
 */

/** One position of the box, with whatever is standing in it. */
export interface BoxGridCell {
  readonly position: BoxPosition;
  /** The position's own row, as declared — not an index into `rows`. */
  readonly row: number;
  /** The position's own column, as declared — not an index into `cols`. */
  readonly col: number;
  readonly sample: Sample | undefined;
}

export interface BoxGrid {
  /** Every row number the box type declares, ascending. */
  readonly rows: readonly number[];
  /** Every column number the box type declares, ascending. */
  readonly cols: readonly number[];
  /**
   * The rectangle, row-major: `rows.length * cols.length` entries, `null` where
   * the box type declares no position.
   */
  readonly cells: readonly (BoxGridCell | null)[];
  /** The declared positions, in (row, col) order. */
  readonly positions: readonly BoxPosition[];
  /** The sample standing at each occupied position label. */
  readonly samplesByPosition: ReadonlyMap<string, Sample>;
  /** Occupied positions. */
  readonly sampleCount: number;
  /** Declared positions with nothing in them. */
  readonly freeCount: number;
  /**
   * Samples in this box that no declared position accounts for: an empty or
   * unknown `position_label`, a label two rows claim at once, or any sample at
   * all when the box type could not be loaded.
   */
  readonly unplaced: readonly Sample[];
}

const coordinateKey = (row: number, col: number): string => `${String(row)}:${String(col)}`;

/** Reading order, so `positions` is stable and the label sheet prints in order. */
function byRowThenCol(a: BoxPosition, b: BoxPosition): number {
  return a.row - b.row || a.col - b.col || a.label.localeCompare(b.label);
}

export function buildBoxGrid(boxType: BoxType | undefined, samples: readonly Sample[]): BoxGrid {
  const positions = [...(boxType?.positions ?? [])].sort(byRowThenCol);

  const byCoordinate = new Map<string, BoxPosition>();
  const byLabel = new Map<string, BoxPosition>();
  for (const position of positions) {
    byCoordinate.set(coordinateKey(position.row, position.col), position);
    byLabel.set(position.label, position);
  }

  const samplesByPosition = new Map<string, Sample>();
  const unplaced: Sample[] = [];
  for (const sample of samples) {
    // A tombstoned row is a deleted sample: the box no longer holds it, and it
    // is not "unaccounted for" either. (`sample/list` hides these by default;
    // this is the guard for a caller that asked for them.)
    if (sample.status === SampleStatus.TOMBSTONED) {
      continue;
    }
    const label = sample.positionLabel ?? '';
    // The second of two rows claiming one position goes to `unplaced` rather
    // than replacing the first: `samples_position_unique` makes it impossible
    // on the server, and a fake or a stale cache that produces it must not make
    // a stored sample invisible.
    if (!byLabel.has(label) || samplesByPosition.has(label)) {
      unplaced.push(sample);
      continue;
    }
    samplesByPosition.set(label, sample);
  }

  const rows = [...new Set(positions.map((position) => position.row))].sort((a, b) => a - b);
  const cols = [...new Set(positions.map((position) => position.col))].sort((a, b) => a - b);

  const cells: (BoxGridCell | null)[] = [];
  for (const row of rows) {
    for (const col of cols) {
      const position = byCoordinate.get(coordinateKey(row, col));
      if (position === undefined) {
        cells.push(null);
        continue;
      }
      cells.push({ position, row, col, sample: samplesByPosition.get(position.label) });
    }
  }

  return {
    rows,
    cols,
    cells,
    positions,
    samplesByPosition,
    sampleCount: samplesByPosition.size,
    freeCount: positions.length - samplesByPosition.size,
    unplaced,
  };
}

/** The cell at a declared row and column, or `null` for a hole or a bad pair. */
export function cellAt(grid: BoxGrid, row: number, col: number): BoxGridCell | null {
  const rowIndex = grid.rows.indexOf(row);
  const colIndex = grid.cols.indexOf(col);
  if (rowIndex < 0 || colIndex < 0) {
    return null;
  }
  return grid.cells[rowIndex * grid.cols.length + colIndex] ?? null;
}

/** The holes a mixed box type leaves in its rectangle. */
export function gapCount(grid: BoxGrid): number {
  return grid.cells.length - grid.positions.length;
}

/** The four arrow-key directions a roving tabindex moves focus in (issue #92). */
export type GridDirection = 'up' | 'down' | 'left' | 'right';

/** Which end of a row — or of the whole grid — Home and End jump to. */
export type GridEdge = 'start' | 'end';

/**
 * The declared position one step from `from` in `direction`, or `null` when the
 * grid ends there (issue #92's arrow-key movement).
 *
 * **A step is taken over the declared positions, never over the rectangle and
 * never over reading order.** Three tempting implementations are all wrong on
 * the mixed template, whose 3×5 rectangle has holes at (2,3) and (2,4):
 *
 *  - `cells[index + 1]` hands back the hole to the right of `C3`, and `B4`/`B5`
 *    have a hole below them;
 *  - `col + 1` is not a position at all for those three cells;
 *  - `positions[index + 1]` is `A4` to the right of `C3` — a row up and a column
 *    along, a jump no keyboard user can see coming.
 *
 * The correct answer for all three is `null`: the step stops at the end of the
 * row or column. `null` is therefore a normal result, not an error — the caller
 * leaves focus where it is.
 */
export function cellInDirection(
  grid: BoxGrid,
  from: BoxGridCell,
  direction: GridDirection,
): BoxGridCell | null {
  const rowIndex = grid.rows.indexOf(from.row);
  const colIndex = grid.cols.indexOf(from.col);
  if (rowIndex < 0 || colIndex < 0) {
    return null;
  }

  // Left/right stay inside the row and walk the declared columns; up/down stay
  // inside the column and walk the declared rows. Both therefore skip a hole
  // that sits between two positions of the same line.
  const horizontal = direction === 'right' || direction === 'left';
  const forward = direction === 'right' || direction === 'down';
  const step = forward ? 1 : -1;
  const length = horizontal ? grid.cols.length : grid.rows.length;
  const start = (horizontal ? colIndex : rowIndex) + step;

  for (let index = start; forward ? index < length : index >= 0; index += step) {
    const cell = horizontal
      ? cellAt(grid, from.row, grid.cols[index])
      : cellAt(grid, grid.rows[index], from.col);
    if (cell !== null) {
      return cell;
    }
  }
  return null;
}

/**
 * The first (`start`) or last (`end`) declared position of `from`'s row — what
 * Home and End jump to. Always a cell: the row holds `from` itself at worst.
 */
export function cellAtRowEdge(grid: BoxGrid, from: BoxGridCell, edge: GridEdge): BoxGridCell {
  const forward = edge === 'start';
  for (
    let index = forward ? 0 : grid.cols.length - 1;
    forward ? index < grid.cols.length : index >= 0;
    index += forward ? 1 : -1
  ) {
    const cell = cellAt(grid, from.row, grid.cols[index]);
    if (cell !== null) {
      return cell;
    }
  }
  // Only reachable for a cell whose row the grid does not declare.
  return from;
}

/**
 * The first or last declared position of the whole grid — what Control+Home and
 * Control+End jump to. `positions` is in reading order, so "last" is the bottom
 * right of the map as it is *declared*: `H12` of a 96-well rack, and `C3` — not
 * `C5`, which is a hole, nor `B5`, which is a row up — of the mixed template.
 */
export function gridEdgeCell(grid: BoxGrid, edge: GridEdge): BoxGridCell | null {
  const ordered = edge === 'start' ? grid.positions : [...grid.positions].reverse();
  for (const position of ordered) {
    const cell = cellAt(grid, position.row, position.col);
    if (cell !== null) {
      return cell;
    }
  }
  return null;
}
