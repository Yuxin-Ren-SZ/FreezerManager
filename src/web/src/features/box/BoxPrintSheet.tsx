// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { type BoxGrid } from './boxGridModel';
import styles from './BoxScreen.module.css';

/**
 * The printable box map and label sheet (TODO.md G3.4, PRD F6.3).
 *
 * Printed with print CSS rather than the Qt client's PDF export: the sheet is
 * ordinary DOM, `@media print` hides the interactive screen, and the user's
 * "Save as PDF" produces the file. Nothing here is generated twice — the map
 * and the labels both come from the same `BoxGrid` the screen renders, so the
 * paper cannot disagree with the screen about which positions exist.
 *
 * **`aria-hidden`.** Every position, sample name and status is already in the
 * accessibility tree as a labelled button in the grid above; announcing a
 * second copy of 96 wells would be noise, not access. The sheet is a visual
 * artifact for paper, and it is present in the DOM at all times so the CSS —
 * not a conditional render — decides when it is seen.
 */
export interface BoxPrintSheetProps {
  readonly grid: BoxGrid;
  readonly boxLabel: string;
  /** Freezer → … → box, as one line. */
  readonly path: string;
}

export function BoxPrintSheet({ grid, boxLabel, path }: BoxPrintSheetProps) {
  const { t } = useTranslation('box');
  const columns = Math.max(grid.cols.length, 1);

  return (
    <div className={styles.printSheet} aria-hidden="true">
      <h2 className={styles.printTitle}>{t('print.mapTitle', { box: boxLabel })}</h2>
      <p className={styles.printPath}>{path}</p>

      <div
        className={styles.printMap}
        style={{ gridTemplateColumns: `repeat(${String(columns)}, minmax(0, 1fr))` }}
      >
        {grid.cells.map((cell, index) =>
          cell === null ? (
            <div key={`print-gap-${String(index)}`} className={styles.printGap} />
          ) : (
            <div
              key={cell.position.label}
              className={styles.printCell}
              data-print-position={cell.position.label}
            >
              <span className={styles.printPosition}>{cell.position.label}</span>
              <span className={styles.printSample}>
                {cell.sample === undefined ? t('print.empty') : cell.sample.name}
              </span>
            </div>
          ),
        )}
      </div>

      <h2 className={styles.printTitle}>{t('print.labelsTitle')}</h2>
      <ul className={styles.labelSheet}>
        {grid.positions.map((position) => {
          const sample = grid.samplesByPosition.get(position.label);
          return (
            <li key={position.label} className={styles.label} data-print-label={position.label}>
              <span className={styles.printPosition}>{position.label}</span>
              {sample === undefined ? null : (
                <span className={styles.printSample}>{sample.name}</span>
              )}
            </li>
          );
        })}
      </ul>
    </div>
  );
}
