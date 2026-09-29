// SPDX-License-Identifier: AGPL-3.0-or-later
import { create, toJson, type MessageInitShape } from '@bufbuild/protobuf';
import { act, fireEvent, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { Route, Routes, useLocation } from 'react-router-dom';
import { afterEach, describe, expect, it } from 'vitest';
import boxCopy from '../../../locales/en/box.json';
import type { GrpcCode } from '../../api/errors';
import type { RpcName } from '../../api/routes';
import type { CurrentUser } from '../../app/session';
import { ALL_PERMISSIONS } from '../../app/permissions';
import { currentUserWith } from '../../test/session';
import { BoxPositionSchema, BoxSchema, BoxTypeSchema } from '../../gen/fmgr/v1/box_pb';
import { SampleSchema, SampleStatus, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { TimestampSchema } from '../../gen/fmgr/v1/common/types_pb';
import { FakeEventSource } from '../../test/fakeEventSource';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { renderWithProviders } from '../../test/render';
import { server } from '../../test/server';
import { axe } from '../../test/setup';
import { ToastProvider } from '../../ui';
import { BoxScreen } from './BoxScreen';
import boxCss from './BoxScreen.module.css?raw';
import boxStyles from './BoxScreen.module.css';

/**
 * The box view (TODO.md G3.4, PRD §9 / F6.3): what is in this box, where, and
 * how things move between positions.
 *
 * Four assertions carry the acceptance criteria and are worth reading twice:
 *
 *  - **The mixed template renders 13 cells, not 15.** Everything else here is a
 *    rectangle (`box-1` is the shared fake's 96-well), and a rectangle is
 *    exactly what a wrong grid passes on.
 *  - **Drag and keyboard reach the same `sample/move`.** The test captures the
 *    requests MSW saw and compares the two bodies.
 *  - **"Position taken" and "size mismatch" are two different toasts**, in
 *    different words, at the same time. One generic message loses the only
 *    actionable part.
 *  - **The grid is live.** A `sample/watch?box_id=` frame adds a cell, and a
 *    tombstoned frame drops one out.
 *
 * Issue #92's roving tabindex adds a fifth, in its own block below: **the number
 * of key presses a corner-to-corner move takes**, which is the one assertion a
 * grid that is still 96 tab stops would pass on the outcome alone.
 */

const LAB_ID = 'lab-demo';
const MIXED_BOX_ID = 'box-mixed';
const MIXED_BOX_LABEL = 'Mixed rack';
const WELL_BOX_ID = 'box-1';

/** Every permission the catalog has, in the demo lab. */
const ADMIN: CurrentUser = currentUserWith(ALL_PERMISSIONS, { labId: LAB_ID });

/** A ReadOnly member: `sample.read` and nothing else, so no move is offered. */
const READ_ONLY: CurrentUser = currentUserWith(['sample.read'], { labId: LAB_ID });

/**
 * The D4.2 mixed template's 13 positions — `data/seed/box_types/mixed_eppendorf.json`,
 * mirrored here because the screen test runs in jsdom and cannot read a file.
 * `boxGridModel.test.ts` runs the same geometry against the shipped JSON, so the two
 * cannot drift without a red test.
 *
 * The eight `accepts` values are the *web fake's* size classes (`tube-50`,
 * `tube-15`), not the seed file's (`tube_50ml`, `tube_15ml`): G1.2's
 * `containerTypes` seed uses the hyphenated spelling, and the point of the
 * fixture is that the fake's size-class rule actually fires.
 *
 * Note the rows and columns are 0-based, as the shipped template is: a grid that
 * derives its labels from the index rather than reading the position renders
 * the wrong map here and passes on the fake's 1-based `seedPositions`.
 */
const MIXED_POSITIONS: readonly (readonly [string, number, number, string])[] = [
  ['A1', 0, 0, 'tube-50'],
  ['A2', 0, 1, 'tube-50'],
  ['A3', 0, 2, 'tube-50'],
  ['B1', 1, 0, 'tube-50'],
  ['B2', 1, 1, 'tube-50'],
  ['B3', 1, 2, 'tube-50'],
  ['C1', 2, 0, 'tube-50'],
  ['C2', 2, 1, 'tube-50'],
  ['C3', 2, 2, 'tube-50'],
  ['A4', 0, 3, 'tube-15'],
  ['A5', 0, 4, 'tube-15'],
  ['B4', 1, 3, 'tube-15'],
  ['B5', 1, 4, 'tube-15'],
];

/** The three samples the move tests need, in the mixed box. */
const SIZE_CLASS_TUBE_15 = 'ct-15ml';
const SIZE_CLASS_TUBE_50 = 'ct-50ml';

/**
 * The demo lab plus one mixed box.
 *
 * Built by extending `createDemoLab()` rather than by editing it: this feature's
 * fixtures must not move the rows G3.2 and G3.3 assert on.
 */
function boxViewDemo(): DemoLab {
  const lab = createDemoLab();
  const createdAt = create(TimestampSchema, { unixMicros: 1_758_931_200_000_000n });

  lab.boxTypes.push(
    create(BoxTypeSchema, {
      id: 'bt-mixed',
      labId: LAB_ID,
      name: 'Eppendorf Mixed Tube Rack',
      positions: MIXED_POSITIONS.map(([label, row, col, accepts]) =>
        create(BoxPositionSchema, { label, row, col, accepts: [accepts] }),
      ),
      createdAt,
    }),
  );
  lab.boxes.push(
    create(BoxSchema, {
      id: MIXED_BOX_ID,
      labId: LAB_ID,
      boxTypeId: 'bt-mixed',
      storageContainerId: 'ct-shelf-1',
      label: MIXED_BOX_LABEL,
      createdAt,
    }),
  );
  lab.samples.push(
    // A 50 mL tube in the 3×3 block, which is the block that accepts it.
    sampleInBox('sample-mix-50', 'A1', SIZE_CLASS_TUBE_50),
    // A 15 mL tube in the 2×2 block: free to move to another of its positions.
    sampleInBox('sample-mix-15', 'A5', SIZE_CLASS_TUBE_15),
    // ...and one already sitting at B4, for the "position taken" rejection.
    sampleInBox('sample-mix-b4', 'B4', SIZE_CLASS_TUBE_15),
  );

  return lab;
}

function sampleInBox(id: string, positionLabel: string, containerTypeId: string): Sample {
  return create(SampleSchema, {
    id,
    labId: LAB_ID,
    itemTypeId: 'it-serum',
    name: `Sample ${id}`,
    boxId: MIXED_BOX_ID,
    positionLabel,
    containerTypeId,
    status: SampleStatus.ACTIVE,
  });
}

/** The router's current URL, so a test can assert where a cell click went. */
function LocationProbe() {
  const location = useLocation();
  return <span data-testid="location">{`${location.pathname}${location.search}`}</span>;
}

/** The request bodies MSW saw for `sample/move`, in order. */
const moveBodies: Promise<Record<string, unknown>>[] = [];

function captureMoveRequests(): void {
  server.events.on('request:start', ({ request }) => {
    if (new URL(request.url).pathname === '/api/v1/sample/move') {
      moveBodies.push(request.clone().json() as Promise<Record<string, unknown>>);
    }
  });
}

async function movesSeen(): Promise<Record<string, unknown>[]> {
  return Promise.all(moveBodies);
}

function renderScreen(
  options: {
    boxId?: string;
    demo?: DemoLab;
    user?: CurrentUser;
    /** Per-RPC faults, installed *in* the fake: `server.use` prepends. */
    fail?: Partial<Record<RpcName, GrpcCode>>;
  } = {},
) {
  const lab = options.demo ?? boxViewDemo();
  server.use(...fakeApi({ lab, ...(options.fail ? { fail: options.fail } : {}) }));
  const boxId = options.boxId ?? MIXED_BOX_ID;

  const result = renderWithProviders(
    <ToastProvider defaultDuration={0}>
      <Routes>
        <Route path="/labs/:labId/boxes/:boxId" element={<BoxScreen />} />
        <Route path="/labs/:labId/samples/:sampleId" element={<p>sample detail</p>} />
      </Routes>
      <LocationProbe />
    </ToastProvider>,
    { route: `/labs/${LAB_ID}/boxes/${boxId}`, user: options.user ?? ADMIN },
  );

  return { ...result, lab };
}

/** The cell wrapper for one position, which is what a drop lands on. */
function cell(position: string): HTMLElement {
  const found = document.querySelector<HTMLElement>(`[data-position="${position}"]`);
  if (found === null) {
    throw new Error(`the grid has no cell for position ${position}`);
  }
  return found;
}

function cellButton(position: string): HTMLElement {
  return within(cell(position)).getByRole('button');
}

/** The interactive grid — the print sheet's map is a second grid, and hidden. */
function grid(boxLabel: string): HTMLElement {
  return screen.getByRole('grid', {
    name: boxCopy.grid.label.replace('{{box}}', boxLabel),
  });
}

function occupiedCell(position: string): RegExp {
  return new RegExp(`^${position} —`);
}

/** Every position the grid drew a cell for. */
function drawnPositions(): string[] {
  return [...document.querySelectorAll<HTMLElement>('[data-position]')].map(
    (element) => element.dataset.position ?? '',
  );
}

/** A `dataTransfer` stand-in: jsdom implements no `DataTransfer` at all. */
function dragData(): { dataTransfer: Record<string, unknown> } {
  return { dataTransfer: { setData: () => undefined, effectAllowed: '', types: [] } };
}

function drag(from: string, to: string): void {
  fireEvent.dragStart(cellButton(from), dragData());
  fireEvent.dragOver(cell(to), dragData());
  fireEvent.drop(cell(to), dragData());
}

/** Push one `sample/watch` frame through the open stream. */
function pushFrame(frame: Record<string, unknown>): void {
  act(() => {
    FakeEventSource.current().message(JSON.stringify({ lab_id: LAB_ID, ...frame }));
  });
}

/** A frame as the gateway serialises a sample: proto field names, JSON values. */
function frameFor(init: MessageInitShape<typeof SampleSchema>): Record<string, unknown> {
  return toJson(SampleSchema, create(SampleSchema, { labId: LAB_ID, ...init }), {
    useProtoFieldName: true,
  }) as Record<string, unknown>;
}

afterEach(() => {
  moveBodies.length = 0;
  server.events.removeAllListeners();
  FakeEventSource.reset();
});

describe('BoxScreen — the grid', () => {
  it('draws one cell per position of the box type, not per well of a rectangle', async () => {
    renderScreen({ boxId: WELL_BOX_ID });

    expect(await screen.findByRole('heading', { level: 1, name: 'Box Box A' })).toBeInTheDocument();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    expect(drawnPositions()).toHaveLength(96);
    expect(boxCopy.grid.summary).toBeTruthy();
  });

  it('draws the mixed template as 13 cells with its two holes left empty', async () => {
    renderScreen();

    await screen.findByRole('button', { name: occupiedCell('A1') });

    expect(drawnPositions()).toHaveLength(13);
    expect(drawnPositions()).toContain('A4');
    expect(drawnPositions()).toContain('B5');
    // The two cells a 3×5 rectangle would invent: no label, no drop target.
    expect(document.querySelector('[data-position="C4"]')).toBeNull();
    expect(document.querySelector('[data-position="C5"]')).toBeNull();
  });

  it('takes the label of a cell from the position, not from its index', async () => {
    renderScreen();

    // 0-based rows in the shipped template: `A4` sits at row 0, column 3.
    expect(await screen.findByRole('button', { name: occupiedCell('A4') })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: occupiedCell('A5') })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: /^B5 — empty$/ })).toBeInTheDocument();
  });

  it('shows the sample name and status on an occupied cell', async () => {
    renderScreen();

    const occupied = await screen.findByRole('button', { name: occupiedCell('A1') });

    expect(occupied).toHaveTextContent('A1');
    expect(occupied).toHaveTextContent('Sample sample-mix-50');
    expect(occupied).toHaveTextContent('Active');
  });

  it('opens the sample detail when an occupied cell is activated', async () => {
    const user = userEvent.setup();
    renderScreen();

    await user.click(await screen.findByRole('button', { name: occupiedCell('A1') }));

    expect(screen.getByTestId('location')).toHaveTextContent(
      `/labs/${LAB_ID}/samples/sample-mix-50`,
    );
  });

  it('says the box is not in this lab rather than drawing an empty map', async () => {
    renderScreen({ boxId: 'box-that-does-not-exist' });

    expect(
      await screen.findByText(boxCopy.notFoundTitle, undefined, { timeout: 5000 }),
    ).toBeInTheDocument();
    expect(drawnPositions()).toHaveLength(0);
  });

  it('says the box type is unavailable rather than drawing an empty map', async () => {
    const lab = boxViewDemo();
    // A box whose type is not in the loaded set: archived, or another lab's.
    lab.boxes.push(
      create(BoxSchema, {
        id: 'box-orphan',
        labId: LAB_ID,
        boxTypeId: 'bt-not-loaded',
        storageContainerId: 'ct-shelf-1',
        label: 'Orphan box',
      }),
    );
    renderScreen({ boxId: 'box-orphan', demo: lab });

    expect(await screen.findByText(boxCopy.noBoxTypeTitle)).toBeInTheDocument();
    expect(drawnPositions()).toHaveLength(0);
  });

  it('warns about a sample the box type does not place instead of hiding it', async () => {
    const lab = boxViewDemo();
    lab.samples.push(sampleInBox('sample-drifted', 'Z99', SIZE_CLASS_TUBE_15));
    renderScreen({ demo: lab });

    expect(
      await screen.findByText(boxCopy.grid.unplaced_one.replace('{{count}}', '1')),
    ).toBeInTheDocument();
  });

  it('shows the layout failure with a retry rather than a half-drawn grid', async () => {
    renderScreen({ fail: { 'box/list': 'PERMISSION_DENIED' } });

    expect(await screen.findByText(boxCopy.errorTitle)).toBeInTheDocument();
    expect(drawnPositions()).toHaveLength(0);
  });

  it('has no accessibility violations with the mixed template on screen', async () => {
    const { container } = renderScreen();

    await screen.findByRole('button', { name: occupiedCell('A1') });

    expect(await axe(container)).toHaveNoViolations();
  });
});

describe('BoxScreen — moving a sample', () => {
  it('moves a sample by drag and drop, and says so', async () => {
    const { lab } = renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A5') });

    drag('A5', 'B5');

    await waitFor(() => {
      expect(lab.samples.find((sample) => sample.id === 'sample-mix-15')).toHaveProperty(
        'positionLabel',
        'B5',
      );
    });
    expect(
      await screen.findByText(
        boxCopy.move.success
          .replace('{{name}}', 'Sample sample-mix-15')
          .replace('{{position}}', 'B5'),
      ),
    ).toBeInTheDocument();
    expect(
      await screen.findByRole('button', { name: /^B5 — Sample sample-mix-15/ }),
    ).toBeInTheDocument();
  });

  it('moves a sample from the keyboard: pick up with Space, put down with Enter', async () => {
    const user = userEvent.setup();
    const { lab } = renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A5') });

    cellButton('A5').focus();
    await user.keyboard(' ');
    expect(
      screen.getByText(
        new RegExp(
          boxCopy.selection.picked
            .replace('{{name}}', 'Sample sample-mix-15')
            .replace('{{position}}', 'A5'),
        ),
      ),
    ).toBeInTheDocument();

    cellButton('B5').focus();
    await user.keyboard('{Enter}');

    await waitFor(() => {
      expect(lab.samples.find((sample) => sample.id === 'sample-mix-15')).toHaveProperty(
        'positionLabel',
        'B5',
      );
    });
  });

  it('reaches the same sample/move call from both paths', async () => {
    const user = userEvent.setup();
    const { lab } = renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A5') });
    captureMoveRequests();

    // Mouse: A5 → B5.
    drag('A5', 'B5');
    await waitFor(() => {
      expect(lab.samples.find((sample) => sample.id === 'sample-mix-15')).toHaveProperty(
        'positionLabel',
        'B5',
      );
    });

    // Keyboard, the same sample back: B5 → A5.
    cellButton('B5').focus();
    await user.keyboard(' ');
    cellButton('A5').focus();
    await user.keyboard('{Enter}');
    await waitFor(() => {
      expect(lab.samples.find((sample) => sample.id === 'sample-mix-15')).toHaveProperty(
        'positionLabel',
        'A5',
      );
    });

    const bodies = await movesSeen();
    expect(bodies).toHaveLength(2);
    expect(bodies[0]).toEqual({
      sample_id: 'sample-mix-15',
      dest_box_id: MIXED_BOX_ID,
      dest_position: 'B5',
    });
    expect(bodies[1]).toEqual({
      sample_id: 'sample-mix-15',
      dest_box_id: MIXED_BOX_ID,
      dest_position: 'A5',
    });
    // Same message, same field set: only the destination differs.
    expect(Object.keys(bodies[1]).sort()).toEqual(Object.keys(bodies[0]).sort());
  });

  it('cancels a picked-up sample with Escape, leaving it where it was', async () => {
    const user = userEvent.setup();
    const { lab } = renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A5') });

    cellButton('A5').focus();
    await user.keyboard(' ');
    await user.keyboard('{Escape}');

    expect(screen.getByText(boxCopy.selection.none)).toBeInTheDocument();

    cellButton('B5').focus();
    await user.keyboard('{Enter}');

    expect(lab.samples.find((sample) => sample.id === 'sample-mix-15')).toHaveProperty(
      'positionLabel',
      'A5',
    );
  });

  it('distinguishes a taken position from a size mismatch in two toasts', async () => {
    renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A5') });

    // A 15 mL tube onto B4, which already holds one: ALREADY_EXISTS.
    drag('A5', 'B4');
    // A 50 mL tube onto A4, which takes only 15 mL: INVALID_ARGUMENT.
    drag('A1', 'A4');

    expect(await screen.findByText(boxCopy.move.positionTakenTitle)).toBeInTheDocument();
    expect(screen.getByText(boxCopy.move.sizeMismatchTitle)).toBeInTheDocument();

    // Different words, and each names the fix for its own failure.
    expect(boxCopy.move.positionTakenTitle).not.toBe(boxCopy.move.sizeMismatchTitle);
    expect(
      screen.getByText(boxCopy.move.positionTakenBody.replace('{{position}}', 'B4')),
    ).toBeInTheDocument();
    expect(
      screen.getByText(
        boxCopy.move.sizeMismatchBody
          .replace('{{name}}', 'Sample sample-mix-50')
          .replace('{{position}}', 'A4'),
      ),
    ).toBeInTheDocument();

    // Neither rejection may be reported as if it were the other.
    expect(screen.getAllByText(boxCopy.move.positionTakenTitle)).toHaveLength(1);
    expect(screen.getAllByText(boxCopy.move.sizeMismatchTitle)).toHaveLength(1);
  });

  it('reports an unrecognised rejection without inventing a cause', async () => {
    renderScreen({ fail: { 'sample/move': 'PERMISSION_DENIED' } });
    await screen.findByRole('button', { name: occupiedCell('A5') });

    drag('A5', 'B5');

    expect(await screen.findByText(boxCopy.move.failedTitle)).toBeInTheDocument();
    expect(screen.queryByText(boxCopy.move.positionTakenTitle)).toBeNull();
    expect(screen.queryByText(boxCopy.move.sizeMismatchTitle)).toBeNull();
  });

  it('offers no move at all without sample.write', async () => {
    const user = userEvent.setup();
    const { lab } = renderScreen({ user: READ_ONLY });
    const source = await screen.findByRole('button', { name: occupiedCell('A5') });

    expect(source).not.toHaveAttribute('draggable', 'true');

    source.focus();
    await user.keyboard(' ');
    expect(
      screen.queryByText(
        boxCopy.selection.picked
          .replace('{{name}}', 'Sample sample-mix-15')
          .replace('{{position}}', 'A5'),
      ),
    ).toBeNull();

    // The cell is still the way to the sample's detail.
    await user.click(source);
    expect(screen.getByTestId('location')).toHaveTextContent(
      `/labs/${LAB_ID}/samples/sample-mix-15`,
    );
    expect(lab.samples.find((sample) => sample.id === 'sample-mix-15')).toHaveProperty(
      'positionLabel',
      'A5',
    );
  });
});

describe('BoxScreen — the roving tabindex', () => {
  it('is one tab stop, entered at the cell the keyboard was last on', async () => {
    const user = userEvent.setup();
    renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    // Ninety-six cells, one tab stop.
    const stops = within(grid(MIXED_BOX_LABEL))
      .getAllByRole('button')
      .filter((candidate) => candidate.tabIndex === 0);
    expect(stops).toHaveLength(1);
    expect(stops[0]).toBe(cellButton('A1'));

    await user.tab();
    expect(cellButton('A1')).toHaveFocus();

    await user.keyboard('{ArrowRight}{ArrowRight}');
    expect(cellButton('A3')).toHaveFocus();

    // ...and Tab leaves the grid rather than walking its cells.
    await user.tab();
    expect(grid(MIXED_BOX_LABEL).contains(document.activeElement)).toBe(false);

    // Coming back lands on A3, not on the first cell again.
    await user.tab({ shift: true });
    expect(cellButton('A3')).toHaveFocus();
  });

  /**
   * The decisive test for issue #92, and the reason it counts key presses: a
   * grid that is still 96 tab stops keeps a sample at A1 and passes any test
   * that only asserts where the sample ended up.
   *
   * `box-1` is the fake's 96-well rack, so the far corner is H12: seven rows
   * down and eleven columns along.
   */
  it('moves a sample from A1 to H12 in twenty key presses, not ninety-six Tabs', async () => {
    const user = userEvent.setup();
    const { lab } = renderScreen({ boxId: WELL_BOX_ID });
    await screen.findByRole('button', { name: occupiedCell('A1') });

    await user.tab();
    expect(cellButton('A1')).toHaveFocus();

    // Every key the test presses is counted, the two that do the move included.
    let presses = 0;
    const press = async (keys: string): Promise<void> => {
      presses += 1;
      await user.keyboard(keys);
    };

    await press(' '); // pick the sample up
    for (let step = 0; step < 7; step += 1) {
      await press('{ArrowDown}'); // A → H
    }
    for (let step = 0; step < 11; step += 1) {
      await press('{ArrowRight}'); // 1 → 12
    }
    expect(cellButton('H12')).toHaveFocus();
    await press('{Enter}'); // put it down

    await waitFor(() => {
      expect(lab.samples.find((sample) => sample.id === 'sample-1')).toHaveProperty(
        'positionLabel',
        'H12',
      );
    });
    expect(presses).toBe(20);
    expect(presses).toBeLessThan(96);
  });

  it('walks the mixed template one declared position at a time, holes and all', async () => {
    const user = userEvent.setup();
    renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    // Row A ends at A5, and the next position in reading order is B1 — a row
    // down. The cell to the right of A5 does not exist.
    cellButton('A5').focus();
    await user.keyboard('{ArrowRight}');
    expect(cellButton('A5')).toHaveFocus();

    // A5 → B5 is the one step down that exists; below B5 is the hole C5.
    await user.keyboard('{ArrowDown}');
    expect(cellButton('B5')).toHaveFocus();
    await user.keyboard('{ArrowDown}');
    expect(cellButton('B5')).toHaveFocus();

    // B4 has the other hole below it, C4...
    cellButton('B4').focus();
    await user.keyboard('{ArrowDown}');
    expect(cellButton('B4')).toHaveFocus();
    // ...while its own row continues both ways, and C3 is the end of the map.
    await user.keyboard('{ArrowLeft}');
    expect(cellButton('B3')).toHaveFocus();
    await user.keyboard('{ArrowDown}');
    expect(cellButton('C3')).toHaveFocus();
    await user.keyboard('{ArrowRight}');
    expect(cellButton('C3')).toHaveFocus();

    // Nothing that took focus is a hole or a cell outside the box type.
    for (const focused of [
      cellButton('A5'),
      cellButton('B5'),
      cellButton('B3'),
      cellButton('C3'),
    ]) {
      expect(focused.closest('[data-position]')).not.toBeNull();
    }
  });

  it('moves a sample with the arrow keys alone in the mixed template', async () => {
    const user = userEvent.setup();
    const { lab } = renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A5') });

    cellButton('A5').focus();
    await user.keyboard(' '); // pick up a 15 mL tube
    await user.keyboard('{ArrowDown}'); // A5 → B5, the only position below it
    expect(cellButton('B5')).toHaveFocus();
    await user.keyboard('{Enter}');

    await waitFor(() => {
      expect(lab.samples.find((sample) => sample.id === 'sample-mix-15')).toHaveProperty(
        'positionLabel',
        'B5',
      );
    });
  });

  it('jumps to a row’s ends with Home and End, and to the grid’s with Control', async () => {
    const user = userEvent.setup();
    renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    cellButton('B5').focus();
    await user.keyboard('{Home}');
    expect(cellButton('B1')).toHaveFocus();
    await user.keyboard('{End}');
    expect(cellButton('B5')).toHaveFocus();

    // The last declared position is C3: C5 is a hole and B5 is a row up.
    await user.keyboard('{Control>}{Home}{/Control}');
    expect(cellButton('A1')).toHaveFocus();
    await user.keyboard('{Control>}{End}{/Control}');
    expect(cellButton('C3')).toHaveFocus();
  });

  it('announces every cell’s row and column, holes included', async () => {
    renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    const mixed = grid(MIXED_BOX_LABEL);
    expect(mixed).toHaveAttribute('aria-rowcount', '3');
    expect(mixed).toHaveAttribute('aria-colcount', '5');

    const rows = within(mixed).getAllByRole('row');
    expect(rows.map((row) => row.getAttribute('aria-rowindex'))).toEqual(['1', '2', '3']);

    // Five declared columns, so A4 is column 4 of row 1...
    const first = within(rows[0]).getAllByRole('gridcell');
    expect(first).toHaveLength(5);
    expect(first[3]).toHaveAttribute('aria-colindex', '4');
    expect(first[3]).toHaveAttribute('data-position', 'A4');
    // ...and the two holes of row 3 are absent cells, not invented ones.
    expect(within(rows[2]).getAllByRole('gridcell')).toHaveLength(3);
  });

  it('keeps free positions focusable but announced as disabled until a sample is in hand', async () => {
    const user = userEvent.setup();
    renderScreen();
    const free = await screen.findByRole('button', { name: /^C3 — empty$/ });

    // A `disabled` control cannot take focus at all, and the cell below the
    // holes is exactly where the keyboard has to be able to stand.
    expect(free).toHaveAttribute('aria-disabled', 'true');
    free.focus();
    expect(free).toHaveFocus();

    cellButton('A5').focus();
    await user.keyboard(' ');

    expect(screen.getByRole('button', { name: /^C3 — empty$/ })).not.toHaveAttribute(
      'aria-disabled',
    );
  });

  it('styles the cell the keyboard is on with a visible focus ring', () => {
    // jsdom applies no stylesheet, so the ring is checked where it lives.
    expect(boxCss).toMatch(
      /\.position:focus-visible\s*\{[^}]*outline:\s*var\(--fmgr-focus-ring-width\)/,
    );
  });
});

describe('BoxScreen — live updates', () => {
  it('subscribes to sample/watch for this box', async () => {
    renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    expect(FakeEventSource.current().url).toBe(
      `/api/v1/sample/watch?lab_id=${LAB_ID}&box_id=${MIXED_BOX_ID}`,
    );
  });

  it('adds a cell when the feed reports a sample that arrived in this box', async () => {
    renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    pushFrame(
      frameFor({
        id: 'sample-live',
        name: 'Sample arrived',
        boxId: MIXED_BOX_ID,
        positionLabel: 'B1',
        status: SampleStatus.ACTIVE,
      }),
    );

    expect(
      await screen.findByRole('button', { name: /^B1 — Sample arrived, Active$/ }),
    ).toBeInTheDocument();
  });

  it('drops a tombstoned row out of the grid', async () => {
    renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    pushFrame(
      frameFor({
        id: 'sample-mix-15',
        name: 'Sample sample-mix-15',
        boxId: MIXED_BOX_ID,
        positionLabel: 'A5',
        status: SampleStatus.TOMBSTONED,
      }),
    );

    await waitFor(() => {
      expect(screen.getByRole('button', { name: /^A5 — empty$/ })).toBeInTheDocument();
    });
    expect(screen.queryByRole('button', { name: /^A5 — Sample sample-mix-15/ })).toBeNull();
  });
});

/**
 * The printable sheet (TODO.md G3.4, PRD F6.3).
 *
 * Print CSS cannot be *executed* here: jsdom has no print engine, `matchMedia`
 * does not evaluate `print`, and nothing applies the `@media print` block. So
 * the guard is in two halves, and the second is the one that matters:
 *
 *  1. the raw stylesheet text is checked for a print block that hides the
 *     interactive screen and shows the sheet — the criterion itself;
 *  2. every class that block names must exist in the stylesheet's exports *and*
 *     be on an element the component renders. A block that styles `.noPrint`
 *     while the component says `noPrint` (or the reverse) fails silently in a
 *     browser: the page still prints, it is just wrong.
 */
describe('BoxScreen — the printable sheet', () => {
  it('prints one entry per position of the mixed template, holes included', async () => {
    const { container } = renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    const sheet = printedSheet(container);
    expect(sheet.querySelectorAll('[data-print-position]')).toHaveLength(13);

    const labels = [...sheet.querySelectorAll<HTMLElement>('[data-print-label]')].map(
      (entry) => entry.dataset.printLabel ?? '',
    );
    expect(labels).toHaveLength(13);
    expect(labels).toContain('C3');
    expect(labels).toContain('B5');
    // The holes are not printed as positions either.
    expect(labels).not.toContain('C4');
    expect(labels).not.toContain('C5');
    expect(sheet.querySelector('[data-print-position="C4"]')).toBeNull();
  });

  it('prints the sample name against the cell it sits in', async () => {
    const { container } = renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    const cellA1 = printedSheet(container).querySelector('[data-print-position="A1"]');
    expect(cellA1).not.toBeNull();
    expect(cellA1).toHaveTextContent('Sample sample-mix-50');
    expect(printedSheet(container).querySelector('[data-print-position="B5"]')).toHaveTextContent(
      boxCopy.print.empty,
    );
  });

  it('prints every position of a 96-well box', async () => {
    const { container } = renderScreen({ boxId: WELL_BOX_ID });
    await screen.findByRole('button', { name: occupiedCell('A1') });

    expect(printedSheet(container).querySelectorAll('[data-print-position]')).toHaveLength(96);
  });

  it('hides the interactive screen and shows the sheet in print', () => {
    const block = printMediaBlock(boxCss);

    expect(block).not.toBe('');
    expect(block).toMatch(/\.noPrint\s*\{[^}]*display:\s*none/);
    expect(block).toMatch(/\.printSheet\s*\{[^}]*display:\s*(block|grid)/);
  });

  it('styles only classes the component actually renders', async () => {
    const { container } = renderScreen();
    await screen.findByRole('button', { name: occupiedCell('A1') });

    const named = [...printMediaBlock(boxCss).matchAll(/\.([A-Za-z][A-Za-z0-9_]*)/g)].map(
      (match) => match[1],
    );
    expect(named.length).toBeGreaterThan(0);

    for (const name of new Set(named)) {
      const scoped = boxStyles[name];
      expect(scoped).toBeDefined();
      expect(container.querySelector(`.${scoped}`)).not.toBeNull();
    }
  });
});

function printedSheet(container: HTMLElement): HTMLElement {
  const sheet = container.querySelector<HTMLElement>(`.${boxStyles.printSheet}`);
  if (sheet === null) {
    throw new Error('the screen renders no print sheet');
  }
  return sheet;
}

/** The `@media print { … }` block of a stylesheet, braces balanced. */
function printMediaBlock(css: string): string {
  const start = css.indexOf('@media print');
  if (start < 0) {
    return '';
  }
  let depth = 0;
  for (let index = css.indexOf('{', start); index < css.length; index += 1) {
    const character = css[index];
    if (character === '{') {
      depth += 1;
    } else if (character === '}') {
      depth -= 1;
      if (depth === 0) {
        return css.slice(start, index + 1);
      }
    }
  }
  return '';
}
