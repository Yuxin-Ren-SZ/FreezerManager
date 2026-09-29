// @vitest-environment node
/// <reference types="node" />
// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { BoxTypeSchema, type BoxType } from '../../gen/fmgr/v1/box_pb';
import { SampleSchema, SampleStatus, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { buildBoxGrid, cellAt } from './boxGridModel';

/**
 * The box grid as a value (TODO.md G3.4, PRD §9 / F6.3).
 *
 * **Why this file runs in the node environment.** The decisive acceptance
 * criterion is that the grid is drawn from the box type's positions "including
 * mixed formats such as the Eppendorf 3×3 + 2×2 box", and the templates named
 * in `TODO.md` D4.2 are shipped as JSON under `data/seed/box_types/`. Reading
 * those files directly is what makes this test about the *real* templates rather
 * than about a copy of them: a rectangle-only grid passes on the four
 * rectangular templates and fails here, and a copy would have let both drift.
 * `node:fs` needs the node environment; the model itself is pure and React-free,
 * so nothing else in this file wants jsdom. The `reference types="node"`
 * directive above is what puts `node:fs` in scope here: the app's
 * `tsconfig.json` deliberately keeps Node's globals out of `src/`, and this is
 * the one test file that needs them.
 *
 * The mixed template is the one to read twice. It declares **13 positions** at
 * rows 0–2 and columns 0–4 — a 3×5 rectangle with two holes at (2,3) and (2,4).
 * A grid that assumes a rectangle renders 15 cells, invents two positions the
 * box type does not have (`C4`, `C5`), and offers them as drop targets. Note
 * also that its rows and columns are **0-based**, while the demo fake in
 * `src/test/fakeApi.ts` numbers them from 1: the grid must therefore take each
 * position's `row`/`col`/`label` as given and never derive a label from the
 * index, or it renders the wrong thing for one of the two.
 */

const LAB_ID = 'lab-demo';
const BOX_ID = 'box-under-test';

/** Where the shipped D4.2 templates live, relative to this file. */
const TEMPLATE_DIR = new URL('../../../../../data/seed/box_types/', import.meta.url);

interface SeedTemplate {
  readonly name: string;
  readonly positions: readonly {
    readonly label: string;
    readonly row: number;
    readonly col: number;
    readonly z: number | null;
    readonly accepts: readonly string[];
  }[];
}

function seedTemplate(file: string): BoxType {
  const raw = JSON.parse(readFileSync(fileURLToPath(new URL(file, TEMPLATE_DIR)), 'utf8')) as
    SeedTemplate | undefined;
  if (raw === undefined) {
    throw new Error(`${file} did not parse as a BoxType template`);
  }
  return create(BoxTypeSchema, {
    id: `bt-${file}`,
    labId: LAB_ID,
    name: raw.name,
    positions: raw.positions.map((position) => ({
      label: position.label,
      row: position.row,
      col: position.col,
      // `z: null` in the seed means "unset", which is not a number.
      ...(position.z === null ? {} : { z: position.z }),
      accepts: [...position.accepts],
    })),
  });
}

function sampleIn(id: string, positionLabel: string, status = SampleStatus.ACTIVE): Sample {
  return create(SampleSchema, {
    id,
    labId: LAB_ID,
    name: `Sample ${id}`,
    boxId: BOX_ID,
    positionLabel,
    status,
  });
}

describe('buildBoxGrid — the D4.2 templates', () => {
  it('draws the 9×9 cryobox as a 9-by-9 grid of 81 positions', () => {
    const grid = buildBoxGrid(seedTemplate('9x9_cryobox.json'), []);

    expect(grid.rows).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8]);
    expect(grid.cols).toEqual([0, 1, 2, 3, 4, 5, 6, 7, 8]);
    expect(grid.positions).toHaveLength(81);
    expect(grid.cells.filter((cell) => cell !== null)).toHaveLength(81);
    expect(cellAt(grid, 8, 8)?.position.label).toBe('I9');
  });

  it('draws the 10×10 cryobox as 100 positions', () => {
    const grid = buildBoxGrid(seedTemplate('10x10_cryobox.json'), []);

    expect(grid.rows).toHaveLength(10);
    expect(grid.cols).toHaveLength(10);
    expect(grid.positions).toHaveLength(100);
    expect(cellAt(grid, 9, 9)?.position.label).toBe('J10');
  });

  it('draws the 96-well rack as 8 rows by 12 columns', () => {
    const grid = buildBoxGrid(seedTemplate('96_well_rack.json'), []);

    expect(grid.rows).toHaveLength(8);
    expect(grid.cols).toHaveLength(12);
    expect(grid.positions).toHaveLength(96);
    expect(cellAt(grid, 7, 11)?.position.label).toBe('H12');
  });

  it('draws the mixed Eppendorf template at its declared coordinates, holes and all', () => {
    const grid = buildBoxGrid(seedTemplate('mixed_eppendorf.json'), []);

    // The rectangle would be 3×5 = 15; the box type declares 13 positions.
    expect(grid.rows).toEqual([0, 1, 2]);
    expect(grid.cols).toEqual([0, 1, 2, 3, 4]);
    expect(grid.cells).toHaveLength(15);
    expect(grid.positions).toHaveLength(13);
    expect(grid.cells.filter((cell) => cell !== null)).toHaveLength(13);

    // The two holes: cells exist to keep the columns aligned, but they are not
    // positions, so nothing may be dropped on them and no label is invented.
    expect(cellAt(grid, 2, 3)).toBeNull();
    expect(cellAt(grid, 2, 4)).toBeNull();
    expect(grid.positions.map((position) => position.label)).not.toContain('C4');
    expect(grid.positions.map((position) => position.label)).not.toContain('C5');
  });

  it('takes every label from the position itself, never from the row/col index', () => {
    const grid = buildBoxGrid(seedTemplate('mixed_eppendorf.json'), []);
    const template = seedTemplate('mixed_eppendorf.json');

    for (const position of template.positions) {
      // 0-based rows: arithmetic on the index would produce `@4` or `D1` here.
      expect(cellAt(grid, position.row, position.col)?.position.label).toBe(position.label);
    }
    expect(cellAt(grid, 0, 3)?.position.label).toBe('A4');
    expect(cellAt(grid, 2, 2)?.position.label).toBe('C3');
  });
});

describe('buildBoxGrid — occupancy', () => {
  it('puts a sample in the cell its own position label names', () => {
    const grid = buildBoxGrid(seedTemplate('mixed_eppendorf.json'), [sampleIn('sample-1', 'A5')]);

    expect(cellAt(grid, 0, 4)?.sample?.id).toBe('sample-1');
    expect(cellAt(grid, 0, 3)?.sample).toBeUndefined();
    expect(grid.sampleCount).toBe(1);
    expect(grid.freeCount).toBe(12);
  });

  it('keeps a sample the box type does not place out of the cells instead of hiding it', () => {
    const grid = buildBoxGrid(seedTemplate('9x9_cryobox.json'), [
      sampleIn('sample-1', 'A1'),
      sampleIn('sample-drifted', 'Z99'),
      sampleIn('sample-unplaced', ''),
    ]);

    expect(grid.sampleCount).toBe(1);
    expect(grid.unplaced.map((row) => row.id)).toEqual(['sample-drifted', 'sample-unplaced']);
  });

  it('does not let a tombstoned row occupy a position', () => {
    const grid = buildBoxGrid(seedTemplate('9x9_cryobox.json'), [
      sampleIn('sample-deleted', 'A1', SampleStatus.TOMBSTONED),
    ]);

    expect(cellAt(grid, 0, 0)?.sample).toBeUndefined();
    expect(grid.sampleCount).toBe(0);
    // Deleted is not the same as "unaccounted for": it is simply not in the box.
    expect(grid.unplaced).toEqual([]);
  });

  it('shows the second of two samples claiming one position rather than losing it', () => {
    const grid = buildBoxGrid(seedTemplate('9x9_cryobox.json'), [
      sampleIn('sample-first', 'A1'),
      sampleIn('sample-second', 'A1'),
    ]);

    // The unique index makes this impossible on the server; a fake or a stale
    // cache can still produce it, and the row must not vanish from the screen.
    expect(cellAt(grid, 0, 0)?.sample?.id).toBe('sample-first');
    expect(grid.unplaced.map((row) => row.id)).toEqual(['sample-second']);
  });

  it('has no positions and no cells when the box type is missing', () => {
    const grid = buildBoxGrid(undefined, [sampleIn('sample-1', 'A1')]);

    expect(grid.rows).toEqual([]);
    expect(grid.cols).toEqual([]);
    expect(grid.positions).toEqual([]);
    expect(grid.cells).toEqual([]);
    expect(grid.unplaced.map((row) => row.id)).toEqual(['sample-1']);
  });
});
