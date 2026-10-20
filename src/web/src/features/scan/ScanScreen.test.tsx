// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { fireEvent, screen, waitFor, within } from '@testing-library/react';
import { beforeEach, describe, expect, it } from 'vitest';
import scanCopy from '../../../locales/en/scan.json';
import type { GrpcCode } from '../../api/errors';
import type { RpcName } from '../../api/routes';
import { LabProvider } from '../../app/labs';
import type { CurrentUser } from '../../app/session';
import { ALL_PERMISSIONS } from '../../app/permissions';
import { currentUserWith } from '../../test/session';
import { SampleSchema, SampleStatus } from '../../gen/fmgr/v1/sample_pb';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { renderWithProviders } from '../../test/render';
import { server } from '../../test/server';
import { axe } from '../../test/setup';
import { AUTO_SUBMIT_GAP_MS } from './scanSession';
import { ScanScreen } from './ScanScreen';

/**
 * Bulk check-in/out scan mode (TODO.md G3.6, F6.4).
 *
 * A single scan is easy. What this file is actually about is the loop the
 * operator runs a hundred times an hour, and every part of it fails silently:
 *
 *  - the four outcomes are four *different instructions* — scan the next tube,
 *    check the label, look at the tube's state, ask an administrator — so a
 *    screen that renders them all as "failed" is useless mid-stack even though
 *    every individual scan still "worked";
 *  - a field that does not come back focused makes the *second* scan type into
 *    whatever took the focus instead, and the first scan still looks perfect;
 *  - a duplicate scan that is re-sent can double-apply, and nothing in a green
 *    happy-path suite would say so.
 *
 * So the assertions are about request order, request count and the log's own
 * words — not only about what a single scan renders.
 */

const LAB_ID = 'lab-demo';

/** Seeded demo barcodes: 0001 is active, 0002 is active, 0003 is checked out. */
const ACTIVE_BARCODE = 'DEMO-0001';
const ACTIVE_NAME = 'Serum A';
const CHECKED_OUT_BARCODE = 'DEMO-0003';

/** Every permission the catalog has, in the demo lab. */
const ADMIN: CurrentUser = currentUserWith(ALL_PERMISSIONS, { labId: LAB_ID });

/** Signed in with a read grant but no lab membership: nothing to scan into. */
const NO_LAB: CurrentUser = currentUserWith([], { labs: [], globalPermissions: ['sample.read'] });

/** Every request this file caused, cloned before MSW consumed it. */
let calls: { path: string; body: () => Promise<Record<string, unknown>> }[] = [];

server.events.on('request:start', ({ request }) => {
  // One clone per request, parsed at most once: a test that asks twice (before
  // and after the next scan) must not read the same body stream twice.
  const cloned = request.clone();
  let parsed: Promise<Record<string, unknown>> | null = null;
  calls.push({
    path: new URL(request.url).pathname,
    body: () => (parsed ??= cloned.json() as Promise<Record<string, unknown>>),
  });
});

const LIST_PATH = '/api/v1/sample/list';
const CHECKOUT_PATH = '/api/v1/sample/checkout';

function paths(): string[] {
  return calls.map((call) => call.path);
}

async function bodiesFor(path: string): Promise<Record<string, unknown>[]> {
  return Promise.all(calls.filter((call) => call.path === path).map(async (call) => call.body()));
}

function renderScreen(
  options: {
    demo?: DemoLab;
    user?: CurrentUser;
    fail?: Partial<Record<RpcName, GrpcCode>>;
  } = {},
) {
  server.use(
    ...fakeApi({
      lab: options.demo ?? createDemoLab(),
      ...(options.fail ? { fail: options.fail } : {}),
    }),
  );

  return renderWithProviders(
    <LabProvider>
      <ScanScreen />
    </LabProvider>,
    { user: options.user ?? ADMIN, route: `/labs/${LAB_ID}/scan` },
  );
}

/** The scan field: a plain search box, because there is no pick list here. */
function field(): HTMLInputElement {
  return screen.getByRole('searchbox', { name: scanCopy.field.label });
}

function form(): HTMLFormElement {
  const element = field().closest('form');
  if (element === null) {
    throw new Error('the scan field is not inside a form');
  }
  return element;
}

function actionSelect(): HTMLSelectElement {
  return screen.getByRole('combobox', { name: scanCopy.action.label });
}

function chooseAction(value: 'out' | 'in' | 'discard'): void {
  fireEvent.change(actionSelect(), { target: { value } });
}

/** One scan: the whole value arrives at once, then the scanner's Enter. */
async function scan(term: string): Promise<void> {
  const input = await scanField();
  fireEvent.change(input, { target: { value: term } });
  fireEvent.submit(form());
}

/** The same, one keystroke at a time with no pause in between. */
async function typeScan(term: string): Promise<void> {
  const input = await scanField();
  for (const character of term) {
    fireEvent.change(input, { target: { value: input.value + character } });
  }
}

/**
 * The field, once the session's lab has arrived.
 *
 * The session is fetched asynchronously, so the field renders disabled and a
 * scan typed into it would be dropped — which is the screen's own guard, and
 * the reason every scan below waits for the field to be usable first.
 */
async function scanField(): Promise<HTMLInputElement> {
  await waitFor(() => {
    expect(field()).not.toBeDisabled();
  });
  return field();
}

/** The session log's lines, in the order they were appended. Empty when none. */
function logLines(): HTMLElement[] {
  const list = screen.queryByRole('list', { name: scanCopy.log.title });
  return list === null ? [] : within(list).queryAllByRole('listitem');
}

/** The n-th log line, once it exists. */
async function logLine(index: number): Promise<HTMLElement> {
  await waitFor(() => {
    expect(logLines().length).toBeGreaterThan(index);
  });
  // `waitFor` above is what makes this present; the cast-free index is the
  // same expression the assertion just checked.
  return logLines()[index];
}

beforeEach(() => {
  calls = [];
  globalThis.localStorage.clear();
});

describe('ScanScreen — the per-scan loop', () => {
  it('looks the barcode up first and only then applies the action', async () => {
    renderScreen();

    await scan(ACTIVE_BARCODE);

    expect(await logLine(0)).toHaveTextContent(scanCopy.outcome.done);
    // One request per step, in order: resolve the tube, then act on it.
    expect(paths()).toEqual([LIST_PATH, CHECKOUT_PATH]);
    expect(await bodiesFor(LIST_PATH)).toMatchObject([{ barcode: ACTIVE_BARCODE }]);
    expect(await bodiesFor(CHECKOUT_PATH)).toMatchObject([
      { sample_id: 'sample-1', action: 'CHECKOUT_ACTION_CHECKOUT' },
    ]);
  });

  it('names the sample it acted on and its new state', async () => {
    renderScreen();

    await scan(ACTIVE_BARCODE);

    const line = await logLine(0);
    expect(line).toHaveTextContent(ACTIVE_NAME);
    expect(line).toHaveTextContent('Checked out');
  });

  it('hands the field back focused with its text selected after every scan', async () => {
    renderScreen();

    await scan(ACTIVE_BARCODE);
    await logLine(0);

    // No click, no second look: the next scan is typed straight over this one.
    expect(document.activeElement).toBe(field());
    expect(field().value).toBe(ACTIVE_BARCODE);
    expect(field().selectionStart).toBe(0);
    expect(field().selectionEnd).toBe(ACTIVE_BARCODE.length);

    // And after a scan that changed nothing, which is the case a "focus on
    // success" implementation gets wrong.
    await scan('NOPE-0000');
    await logLine(1);

    expect(document.activeElement).toBe(field());
    expect(field().value).toBe('NOPE-0000');
    expect(field().selectionStart).toBe(0);
    expect(field().selectionEnd).toBe('NOPE-0000'.length);
  });

  it('is not swallowed by a scanner burst, and sends the whole barcode once', async () => {
    renderScreen();

    await typeScan(ACTIVE_BARCODE);
    fireEvent.submit(form());

    await logLine(0);
    const lists = await bodiesFor(LIST_PATH);
    expect(lists).toHaveLength(1);
    expect(lists[0]).toMatchObject({ barcode: ACTIVE_BARCODE });
  });

  it('never falls back to a name search: an action needs an exact barcode', async () => {
    renderScreen();

    // "Serum A" is a real sample name, and G3.5's lookup would find it. Here it
    // must not: applying a check-out to a fuzzy match changes a tube the
    // operator is not holding.
    await scan(ACTIVE_NAME);

    expect(await logLine(0)).toHaveTextContent(scanCopy.outcome.notFound);
    expect(paths()).toEqual([LIST_PATH]);
    const [probe] = await bodiesFor(LIST_PATH);
    expect(probe).toMatchObject({ barcode: ACTIVE_NAME });
    // Not `query`: a name match is a lookup's answer, never an action's.
    expect(probe).not.toHaveProperty('query');
  });
});

describe('ScanScreen — the four outcomes stay distinct', () => {
  it('records a completed action as done', async () => {
    renderScreen();

    await scan(ACTIVE_BARCODE);

    const line = await logLine(0);
    expect(line).toHaveTextContent(scanCopy.outcome.done);
    expect(line).not.toHaveTextContent(scanCopy.outcome.notFound);
    expect(line).not.toHaveTextContent(scanCopy.outcome.wrongState);
    expect(line).not.toHaveTextContent(scanCopy.outcome.denied);
  });

  it('records a barcode that matches nothing as not found, with no action call', async () => {
    renderScreen();

    await scan('NOPE-0000');

    const line = await logLine(0);
    expect(line).toHaveTextContent(scanCopy.outcome.notFound);
    expect(line).toHaveTextContent('NOPE-0000');
    // Nothing was resolved, so nothing may be acted on.
    expect(paths()).toEqual([LIST_PATH]);
  });

  it('records a sample the action does not apply to as the wrong state', async () => {
    renderScreen();

    // DEMO-0003 is already checked out; checking it out again is not a crime
    // and not a miss — it is a tube whose state says something to the operator.
    await scan(CHECKED_OUT_BARCODE);

    const line = await logLine(0);
    expect(line).toHaveTextContent(scanCopy.outcome.wrongState);
    expect(line).toHaveTextContent('Plasma A');
    // The state is the actionable part, so the line carries it.
    expect(line).toHaveTextContent('Checked out');
    expect(line).not.toHaveTextContent(scanCopy.outcome.notFound);
  });

  it('records a refusal as denied, not as a wrong state', async () => {
    renderScreen({ fail: { 'sample/checkout': 'PERMISSION_DENIED' } });

    await scan(ACTIVE_BARCODE);

    const line = await logLine(0);
    expect(line).toHaveTextContent(scanCopy.outcome.denied);
    expect(line).not.toHaveTextContent(scanCopy.outcome.wrongState);
    expect(line).not.toHaveTextContent(scanCopy.outcome.notFound);
  });

  it('records an unreachable server as retryable, never as a missing barcode', async () => {
    renderScreen({ fail: { 'sample/list': 'UNAVAILABLE' } });

    await scan(ACTIVE_BARCODE);

    const line = await logLine(0);
    expect(line).toHaveTextContent(scanCopy.outcome.unavailable);
    expect(line).not.toHaveTextContent(scanCopy.outcome.notFound);
    // The loop survives a broken request: the field is back for the next one.
    expect(document.activeElement).toBe(field());
  });

  it('refuses to guess which tube a shared barcode means', async () => {
    const demo = createDemoLab();
    demo.samples.push(
      create(SampleSchema, {
        id: 'sample-twin',
        labId: LAB_ID,
        itemTypeId: 'it-serum',
        name: 'Serum A twin',
        barcode: ACTIVE_BARCODE,
        status: SampleStatus.ACTIVE,
      }),
    );
    renderScreen({ demo });

    await scan(ACTIVE_BARCODE);

    expect(await logLine(0)).toHaveTextContent(scanCopy.outcome.ambiguous);
    // Two samples carry this barcode: acting on either would be a coin flip.
    expect(paths()).toEqual([LIST_PATH]);
  });
});

describe('ScanScreen — the duplicate scan', () => {
  it('skips a repeat of the same action instead of applying it twice', async () => {
    renderScreen();

    await scan(ACTIVE_BARCODE);
    await logLine(0);
    await scan(ACTIVE_BARCODE);

    const second = await logLine(1);
    expect(second).toHaveTextContent(scanCopy.outcome.duplicate);
    expect(second).toHaveTextContent(ACTIVE_BARCODE);
    // The decisive assertion: the session decided, so the server was never
    // asked to apply the same action to the same tube again.
    expect(paths()).toEqual([LIST_PATH, CHECKOUT_PATH]);
  });

  it('is not a duplicate when the action changed, because that is the reversal', async () => {
    renderScreen();

    await scan(ACTIVE_BARCODE);
    await logLine(0);

    chooseAction('in');
    await scan(ACTIVE_BARCODE);

    const second = await logLine(1);
    expect(second).toHaveTextContent(scanCopy.outcome.done);
    expect(await bodiesFor(CHECKOUT_PATH)).toMatchObject([
      { action: 'CHECKOUT_ACTION_CHECKOUT' },
      { action: 'CHECKOUT_ACTION_CHECKIN' },
    ]);
  });

  it('forgets the session when a new one is started, so a re-scan is sent again', async () => {
    renderScreen();

    await scan(ACTIVE_BARCODE);
    await logLine(0);
    await scan(ACTIVE_BARCODE);
    await logLine(1);

    fireEvent.click(screen.getByRole('button', { name: scanCopy.session.new }));

    // Starting a new session clears the log — it does not undo anything, and
    // the screen says so where the button is.
    expect(logLines()).toHaveLength(0);

    await scan(ACTIVE_BARCODE);
    const third = await logLine(0);
    // The session's guard is gone, so the scan is sent again — and this time
    // the *server* is the one saying no, because the first scan already
    // checked the tube out. Two independent reasons a double-apply cannot
    // happen, and the log tells them apart.
    expect(third).toHaveTextContent(scanCopy.outcome.wrongState);
    expect(paths()).toEqual([LIST_PATH, CHECKOUT_PATH, LIST_PATH, CHECKOUT_PATH]);
  });
});

describe('ScanScreen — what a scan does', () => {
  it('checks out, checks in and discards, and only check-in consumes volume', async () => {
    renderScreen();

    await scan(ACTIVE_BARCODE);
    await logLine(0);

    // The volume belongs to the check-in: it is what gets subtracted, and a
    // check-out consumes nothing. The form must not offer it for the check-out
    // that just ran.
    expect(screen.queryByLabelText(scanCopy.volume.label)).not.toBeInTheDocument();

    chooseAction('in');
    fireEvent.change(screen.getByLabelText(scanCopy.volume.label), { target: { value: '40' } });
    fireEvent.change(screen.getByLabelText(scanCopy.reason.label), {
      target: { value: 'aliquot taken' },
    });
    await scan(CHECKED_OUT_BARCODE);
    await logLine(1);

    expect(await bodiesFor(CHECKOUT_PATH)).toMatchObject([
      { action: 'CHECKOUT_ACTION_CHECKOUT' },
      {
        sample_id: 'sample-3',
        action: 'CHECKOUT_ACTION_CHECKIN',
        volume_used: 40,
        volume_unit: 'µL',
        reason: 'aliquot taken',
      },
    ]);

    chooseAction('discard');
    await scan('DEMO-0002');
    await logLine(2);

    expect(await bodiesFor(CHECKOUT_PATH)).toMatchObject([
      { action: 'CHECKOUT_ACTION_CHECKOUT' },
      { action: 'CHECKOUT_ACTION_CHECKIN' },
      { sample_id: 'sample-2', action: 'CHECKOUT_ACTION_DISCARD' },
    ]);
  });

  it('does not send a volume the operator did not type', async () => {
    renderScreen();

    chooseAction('in');
    await scan(CHECKED_OUT_BARCODE);
    await logLine(0);

    const [checkout] = await bodiesFor(CHECKOUT_PATH);
    expect(checkout).toMatchObject({ action: 'CHECKOUT_ACTION_CHECKIN' });
    expect(checkout).not.toHaveProperty('volume_used');
    expect(checkout).not.toHaveProperty('volume_unit');
  });
});

describe('ScanScreen — no undo', () => {
  it('explains how to reverse an action instead of offering one', () => {
    renderScreen();

    expect(screen.getByText(scanCopy.noUndo.title)).toBeInTheDocument();
    expect(screen.getByText(scanCopy.noUndo.reverse)).toBeInTheDocument();
    expect(screen.getByText(scanCopy.noUndo.session)).toBeInTheDocument();
    // A button that does not exist would be worse than no button: there is no
    // undo in the API, and the audit trail is the record.
    expect(screen.queryByRole('button', { name: /undo|reverse/i })).not.toBeInTheDocument();
  });
});

describe('ScanScreen — scanners that send no Enter', () => {
  it('never submits on its own while the option is off, which is the default', async () => {
    renderScreen();

    await typeScan(ACTIVE_BARCODE);
    await new Promise((resolve) => setTimeout(resolve, AUTO_SUBMIT_GAP_MS * 4));

    expect(calls).toHaveLength(0);
    expect(screen.getByLabelText(scanCopy.autoSubmit.label)).not.toBeChecked();
  });

  it('submits after an inactivity gap once the operator turns it on', async () => {
    renderScreen();

    fireEvent.click(screen.getByLabelText(scanCopy.autoSubmit.label));
    await typeScan(ACTIVE_BARCODE);

    // No Enter anywhere in this test: the pause is the submit.
    expect(await logLine(0)).toHaveTextContent(scanCopy.outcome.done);

    // The option does not resurrect a no-Enter submit for an empty field.
    fireEvent.change(field(), { target: { value: '' } });
    await new Promise((resolve) => setTimeout(resolve, AUTO_SUBMIT_GAP_MS * 4));
    expect(paths()).toEqual([LIST_PATH, CHECKOUT_PATH]);
  });
});

describe('ScanScreen — no lab', () => {
  it('disables the field and says why', () => {
    renderScreen({ user: NO_LAB });

    expect(field()).toBeDisabled();
    expect(screen.getByText(scanCopy.field.noLab)).toBeInTheDocument();
  });
});

describe('ScanScreen — accessibility', () => {
  it('has no axe violations with a session log on screen', async () => {
    const { container } = renderScreen();

    await scan(ACTIVE_BARCODE);
    await logLine(0);
    await scan('NOPE-0000');
    await logLine(1);

    await waitFor(async () => {
      expect(await axe(container)).toHaveNoViolations();
    });
  });
});
