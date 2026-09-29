// SPDX-License-Identifier: AGPL-3.0-or-later
import type { DragEvent, KeyboardEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { enumLabel } from '../../api/helpers';
import { SampleStatus, SampleStatusSchema, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { Badge, classNames, type BadgeTone } from '../../ui';
import { type BoxGrid, type BoxGridCell } from './boxGridModel';
import styles from './BoxScreen.module.css';

/**
 * The grid of positions (TODO.md G3.4).
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
 * **A cell is a `<button>`, and the `role="grid"` pattern is deliberately not
 * used.** This is a sparse mixed layout with holes, not a rectangle of
 * `gridcell`s, and a roving-tabindex grid is a bigger change than this issue
 * needs. Instead each position is a labelled button in a `role="group"`, which
 * is honest about what it is and reachable with Tab.
 *
 * **Why free positions are disabled until something is picked up.** 96 focus
 * stops for the 95 positions a keyboard user cannot do anything with is not
 * navigation. With a payload in hand they are enabled, and the drop handlers
 * live on the wrapper, which is never disabled — a browser will not deliver a
 * drop to a disabled control.
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
  return (
    <div
      className={styles.grid}
      role="group"
      aria-label={boxLabel}
      style={{
        gridTemplateColumns: `repeat(${String(Math.max(grid.cols.length, 1))}, minmax(0, 1fr))`,
      }}
    >
      {grid.cells.map((cell, index) =>
        cell === null ? (
          // A hole in a mixed box type: the cell keeps the columns aligned and
          // is otherwise nothing — not a position, not a target, no label.
          <div key={`gap-${String(index)}`} className={styles.gap} />
        ) : (
          <PositionCell
            key={cell.position.label}
            cell={cell}
            canMove={canMove}
            payload={payload}
            onPayloadChange={onPayloadChange}
            onTarget={onTarget}
            onOpen={onOpen}
          />
        ),
      )}
    </div>
  );
}

interface PositionCellProps {
  readonly cell: BoxGridCell;
  readonly canMove: boolean;
  readonly payload: Sample | null;
  readonly onPayloadChange: (sample: Sample | null) => void;
  readonly onTarget: (cell: BoxGridCell) => void;
  readonly onOpen: (sample: Sample) => void;
}

function PositionCell({
  cell,
  canMove,
  payload,
  onPayloadChange,
  onTarget,
  onOpen,
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
    if (event.key !== ' ') {
      return;
    }
    // Suppressing the default is what stops the browser turning this Space into
    // a click as well: pick up and put down would otherwise both fire.
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
        className={classNames(
          styles.position,
          sample === undefined ? styles.free : styles.occupied,
          isPicked ? styles.picked : undefined,
        )}
        aria-label={label}
        aria-pressed={isPicked ? true : undefined}
        disabled={sample === undefined && payload === null}
        draggable={canMove && sample !== undefined}
        onClick={activate}
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
