// SPDX-License-Identifier: AGPL-3.0-or-later
import { useRef, useState, type DragEvent, type KeyboardEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { enumLabel } from '../../api/helpers';
import { SampleStatus, SampleStatusSchema, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { Badge, classNames, type BadgeTone } from '../../ui';
import {
  cellAt,
  cellAtRowEdge,
  cellInDirection,
  gridEdgeCell,
  type BoxGrid,
  type BoxGridCell,
  type GridDirection,
} from './boxGridModel';
import styles from './BoxScreen.module.css';

/**
 * The grid of positions (TODO.md G3.4, issue #92).
 *
 * **One payload, two ways to aim it.** Both paths set the same `payload` — the
 * sample being moved — and both then activate a *target cell*, which is the
 * single call the screen makes. Drag and drop is the mouse path; Space picks a
 * sample up and Enter puts it down for the keyboard path. There is no second
 * move code path to keep in step.
 *
 * **Every cell is a target, including an occupied one.** The grid does not
 * pre-empt the server's two rejections (`ALREADY_EXISTS` on a taken position,
 * `INVALID_ARGUMENT` on a container that does not fit): its picture of the box
 * comes from a cache that a live frame may not have caught up with yet, and a
 * move silently swallowed client-side would be far worse than a toast. It is
 * also the only way either rejection is reachable — see `moveFailure.ts`.
 *
 * **The grid is one tab stop with a roving tabindex (issue #92).** Ninety-six
 * cells used to be ninety-six tab stops, so reaching `H12` of a 96-well rack
 * from the keyboard meant tabbing through the whole box. Exactly one cell is in
 * the tab order now — the one the keyboard was last on, and `A1` before
 * anything has had focus. `ArrowUp`/`Down`/`Left`/`Right` step to the
 * neighbouring position, `Home`/`End` jump to the ends of the current row and
 * `Control+Home`/`Control+End` to the first and last position of the box. A
 * corner-to-corner move is then twenty key presses instead of ninety-six Tabs.
 *
 * **The steps follow the box type's declared positions, not the rectangle
 * around them.** `cellInDirection` is the rule; `boxGridModel.test.ts` pins it
 * against the shipped templates. The mixed 3×3 + 2×2 box is a 3×5 rectangle
 * with two holes, so there is no cell to the right of `C3` and none below `B4`
 * or `B5` — a step that walks the rectangle lands on a hole or on the wrong row
 * instead. Where a step does not exist, focus stays where it is rather than
 * wrapping to a cell a row away.
 *
 * **`role="grid"`, rows, and cells that say where they are.** Each row carries
 * `aria-rowindex` and each cell `aria-colindex`, with `aria-rowcount` and
 * `aria-colcount` on the grid, so a screen reader announces "row 3, column 3"
 * for `C3` instead of a flat list of buttons. The holes are `aria-hidden` cells
 * that keep the columns aligned. Focus is visible on the ring the cells always
 * had: `.position:focus-visible` in `BoxScreen.module.css`.
 *
 * **Why free positions are announced disabled until something is picked up.**
 * Ninety-six focus stops for the ninety-five positions a keyboard user cannot
 * do anything with is not navigation — but `disabled` is the wrong tool for it
 * here: a disabled button cannot hold focus at all, and the roving tabindex
 * needs an arrow key to be able to stand on a free position so that Enter can
 * put a sample down there. `aria-disabled` says the same thing to a screen
 * reader and still lets focus in. The drop handlers live on the wrapper, which
 * was never disabled — a browser will not deliver a drop to a disabled control.
 */

/** Lifecycle state to badge tone; an unknown state stays neutral. */
const STATUS_TONE: Readonly<Partial<Record<number, BadgeTone>>> = {
  [SampleStatus.ACTIVE]: 'success',
  [SampleStatus.CHECKED_OUT]: 'info',
  [SampleStatus.DEPLETED]: 'warning',
  [SampleStatus.DESTROYED]: 'neutral',
  [SampleStatus.TOMBSTONED]: 'danger',
};

const toneFor = (status: number): BadgeTone => STATUS_TONE[status] ?? 'neutral';

/** The arrow keys the roving tabindex moves focus with. */
const ARROW_DIRECTIONS = new Map<string, GridDirection>([
  ['ArrowUp', 'up'],
  ['ArrowDown', 'down'],
  ['ArrowLeft', 'left'],
  ['ArrowRight', 'right'],
]);

/** The cell a position label names, or `null` if this grid does not declare it. */
function cellForLabel(grid: BoxGrid, label: string | null): BoxGridCell | null {
  if (label === null) {
    return null;
  }
  const position = grid.positions.find((candidate) => candidate.label === label);
  return position === undefined ? null : cellAt(grid, position.row, position.col);
}

export interface BoxGridProps {
  readonly grid: BoxGrid;
  /** The box's human label, for the group's accessible name. */
  readonly boxLabel: string;
  /** False for a caller without `sample.write`: no drag, no pick-up. */
  readonly canMove: boolean;
  /** The sample being moved, or `null`. Owned by the screen, shared by both paths. */
  readonly payload: Sample | null;
  readonly onPayloadChange: (sample: Sample | null) => void;
  /** A target cell was activated — by drop, by Enter, or by Space. */
  readonly onTarget: (cell: BoxGridCell) => void;
  /** An occupied cell was activated with nothing in hand. */
  readonly onOpen: (sample: Sample) => void;
}

export function BoxGrid({
  grid,
  boxLabel,
  canMove,
  payload,
  onPayloadChange,
  onTarget,
  onOpen,
}: BoxGridProps) {
  // The roving tabindex's single tab stop: the position label of the cell the
  // keyboard was last on, or the first declared position before any has focus.
  const [focusedLabel, setFocusedLabel] = useState<string | null>(null);
  // The buttons, by position label, so a key press can put DOM focus on the
  // cell the model just chose. State alone cannot: the tab stop moves with the
  // key press, and the element is already in the DOM when it does.
  const buttons = useRef(new Map<string, HTMLButtonElement>());

  const roving =
    cellForLabel(grid, focusedLabel) ?? grid.cells.find((cell) => cell !== null) ?? null;

  /** Move focus to a cell: the tab stop for the next Tab, and the DOM now. */
  function focusCell(cell: BoxGridCell): void {
    setFocusedLabel(cell.position.label);
    buttons.current.get(cell.position.label)?.focus();
  }

  /** Arrow, Home and End keys, resolved against the declared positions. */
  function onNavigate(event: KeyboardEvent<HTMLButtonElement>, cell: BoxGridCell): void {
    if (event.key === 'Home' || event.key === 'End') {
      event.preventDefault();
      const edge = event.key === 'Home' ? 'start' : 'end';
      const target =
        event.ctrlKey || event.metaKey ? gridEdgeCell(grid, edge) : cellAtRowEdge(grid, cell, edge);
      if (target !== null) {
        focusCell(target);
      }
      return;
    }

    const direction = ARROW_DIRECTIONS.get(event.key);
    if (direction === undefined) {
      return;
    }
    // Swallowed whether or not the step exists: an arrow key that cannot move
    // must not scroll the page out from under the cell that has focus.
    event.preventDefault();
    const next = cellInDirection(grid, cell, direction);
    if (next !== null) {
      focusCell(next);
    }
  }

  function registerButton(label: string, node: HTMLButtonElement | null): void {
    if (node === null) {
      buttons.current.delete(label);
      return;
    }
    buttons.current.set(label, node);
  }

  // The rectangle split into rows: `role="row"` has to be a real element, and
  // each row declares the same columns, so the map still lines up.
  const columnCount = Math.max(grid.cols.length, 1);
  const rowCells: (BoxGridCell | null)[][] = [];
  for (let start = 0; start < grid.cells.length; start += grid.cols.length) {
    rowCells.push(grid.cells.slice(start, start + grid.cols.length));
  }

  return (
    <div
      className={styles.grid}
      role="grid"
      aria-label={boxLabel}
      aria-rowcount={grid.rows.length}
      aria-colcount={grid.cols.length}
    >
      {rowCells.map((cells, rowIndex) => (
        <div
          key={`row-${String(rowIndex)}`}
          className={styles.row}
          role="row"
          aria-rowindex={rowIndex + 1}
          style={{ gridTemplateColumns: `repeat(${String(columnCount)}, minmax(0, 1fr))` }}
        >
          {cells.map((cell, colIndex) =>
            cell === null ? (
              // A hole in a mixed box type: the cell keeps the columns aligned
              // and is otherwise nothing — not a position, not a target, no
              // label, and not something a screen reader has to hear about.
              <div
                key={`gap-${String(colIndex)}`}
                className={styles.gap}
                aria-hidden="true"
                role="presentation"
              />
            ) : (
              <PositionCell
                key={cell.position.label}
                cell={cell}
                columnIndex={colIndex + 1}
                tabbable={roving !== null && cell.position.label === roving.position.label}
                canMove={canMove}
                payload={payload}
                onPayloadChange={onPayloadChange}
                onTarget={onTarget}
                onOpen={onOpen}
                onFocused={setFocusedLabel}
                onNavigate={onNavigate}
                registerButton={registerButton}
              />
            ),
          )}
        </div>
      ))}
    </div>
  );
}

interface PositionCellProps {
  readonly cell: BoxGridCell;
  /** The cell's 1-based column, for `aria-colindex`. */
  readonly columnIndex: number;
  /** True for the one cell the roving tabindex keeps in the tab order. */
  readonly tabbable: boolean;
  readonly canMove: boolean;
  readonly payload: Sample | null;
  readonly onPayloadChange: (sample: Sample | null) => void;
  readonly onTarget: (cell: BoxGridCell) => void;
  readonly onOpen: (sample: Sample) => void;
  /** This cell took focus, so it becomes the grid's tab stop. */
  readonly onFocused: (label: string) => void;
  readonly onNavigate: (event: KeyboardEvent<HTMLButtonElement>, cell: BoxGridCell) => void;
  readonly registerButton: (label: string, node: HTMLButtonElement | null) => void;
}

function PositionCell({
  cell,
  columnIndex,
  tabbable,
  canMove,
  payload,
  onPayloadChange,
  onTarget,
  onOpen,
  onFocused,
  onNavigate,
  registerButton,
}: PositionCellProps) {
  const { t } = useTranslation('box');
  // Status labels are shared with the rest of the app, so they come from the
  // default namespace's `enums.SampleStatus.*`.
  const { t: tEnums } = useTranslation();

  const { sample } = cell;
  const isPicked = sample !== undefined && payload !== null && payload.id === sample.id;
  const label =
    sample === undefined
      ? t('grid.cellFree', { position: cell.position.label })
      : t('grid.cellOccupied', {
          position: cell.position.label,
          name: sample.name,
          status: enumLabel(tEnums, SampleStatusSchema, sample.status),
        });

  /** Enter lands here as a click (a browser activates a focused button); Space
   * does not, because it is handled below and its default is suppressed. */
  function activate(): void {
    if (payload !== null) {
      onTarget(cell);
      return;
    }
    if (sample !== undefined) {
      onOpen(sample);
    }
  }

  function onKeyDown(event: KeyboardEvent<HTMLButtonElement>): void {
    if (event.key === 'Escape') {
      onPayloadChange(null);
      return;
    }
    if (event.key === ' ') {
      // Suppressing the default is what stops the browser turning this Space
      // into a click as well: pick up and put down would otherwise both fire.
      event.preventDefault();
      if (!canMove) {
        return;
      }
      if (payload !== null) {
        onTarget(cell);
        return;
      }
      if (sample !== undefined) {
        onPayloadChange(sample);
      }
      return;
    }
    onNavigate(event, cell);
  }

  function onDragStart(event: DragEvent<HTMLButtonElement>): void {
    if (sample === undefined) {
      return;
    }
    onPayloadChange(sample);
    // The payload rides in React state, which is what the drop reads; this is
    // for the browser, which needs a data transfer to start a drag at all (and
    // Firefox refuses without one).
    event.dataTransfer.setData('text/plain', sample.id);
    event.dataTransfer.effectAllowed = 'move';
  }

  return (
    <div
      className={styles.cell}
      role="gridcell"
      aria-colindex={columnIndex}
      // The cell is programmatically focusable and the button inside it is what
      // Tab and the arrow keys actually move to. A `gridcell` is an interactive
      // role, so `jsx-a11y/interactive-supports-focus` wants this attribute
      // even though nothing here ever calls focus() on the wrapper.
      tabIndex={-1}
      data-position={cell.position.label}
      onDragOver={(event) => {
        // Without this the browser treats the cell as "not a drop target" and
        // never fires `drop` at all.
        event.preventDefault();
      }}
      onDrop={(event) => {
        event.preventDefault();
        onTarget(cell);
      }}
    >
      <button
        type="button"
        ref={(node) => {
          registerButton(cell.position.label, node);
        }}
        className={classNames(
          styles.position,
          sample === undefined ? styles.free : styles.occupied,
          isPicked ? styles.picked : undefined,
        )}
        aria-label={label}
        aria-pressed={isPicked ? true : undefined}
        // Announced, not enforced: a free position is a target only while a
        // sample is in hand, but it stays focusable so the keyboard can reach it.
        aria-disabled={sample === undefined && payload === null ? true : undefined}
        tabIndex={tabbable ? 0 : -1}
        draggable={canMove && sample !== undefined}
        onClick={activate}
        onFocus={() => {
          onFocused(cell.position.label);
        }}
        onKeyDown={onKeyDown}
        onDragStart={onDragStart}
        onDragEnd={() => {
          onPayloadChange(null);
        }}
      >
        <span className={styles.positionLabel}>{cell.position.label}</span>
        {sample === undefined ? null : (
          <>
            <span className={styles.sampleName}>{sample.name}</span>
            <Badge tone={toneFor(sample.status)}>
              {enumLabel(tEnums, SampleStatusSchema, sample.status)}
            </Badge>
          </>
        )}
      </button>
    </div>
  );
}
