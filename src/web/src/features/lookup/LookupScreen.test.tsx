// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { fireEvent, screen, waitFor, within } from '@testing-library/react';
import type { ReactNode } from 'react';
import { Link } from 'react-router-dom';
import { beforeEach, describe, expect, it } from 'vitest';
import lookupCopy from '../../../locales/en/lookup.json';
import type { GrpcCode } from '../../api/errors';
import type { RpcName } from '../../api/routes';
import { LabProvider } from '../../app/labs';
import type { CurrentUser } from '../../app/session';
import { STUB_CURRENT_USER, STUB_CURRENT_USER_ALL_PERMISSIONS } from '../../app/stubSession';
import { SampleSchema } from '../../gen/fmgr/v1/sample_pb';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { renderWithProviders } from '../../test/render';
import { server } from '../../test/server';
import { axe } from '../../test/setup';
import { LookupScreen } from './LookupScreen';

/**
 * Single-handed lookup (TODO.md G3.5, PRD §9) — the most common daily flow.
 *
 * The lookup itself is easy; the *loop* is what these tests are really about,
 * because every part of it fails silently:
 *
 *  - a field that does not come back focused with its text selected still
 *    resolves every individual lookup, and only the person doing the *second*
 *    scan notices;
 *  - a search that reads a debounced copy of what was typed still works when a
 *    human types, and swallows a scanner's burst;
 *  - a search that falls back to free text before trying the barcode still
 *    finds the sample, and quietly picks the wrong one when a name contains
 *    the same characters.
 *
 * So the assertions here are deliberately about ordering, focus and request
 * count, not only about what is on screen.
 */

const LAB_ID = 'lab-demo';

/** The demo lab's sample whose barcode the tests scan. */
const BARCODE_TERM = 'DEMO-0001';
const BARCODE_NAME = 'Serum A';

/** Every permission the catalog has, in the demo lab. */
const ADMIN: CurrentUser = {
  ...STUB_CURRENT_USER_ALL_PERMISSIONS,
  labs: STUB_CURRENT_USER_ALL_PERMISSIONS.labs.map((lab) => ({ ...lab, labId: LAB_ID })),
};

/** `sample.read` only: can look up, cannot check anything out. */
const READ_ONLY: CurrentUser = {
  ...STUB_CURRENT_USER,
  permissions: [],
  labs: STUB_CURRENT_USER.labs.map((lab) => ({
    ...lab,
    labId: LAB_ID,
    permissions: ['sample.read'],
  })),
};

/** Signed in with a read grant but no lab membership: nothing to look up in. */
const NO_LAB: CurrentUser = {
  ...STUB_CURRENT_USER,
  permissions: ['sample.read'],
  labs: [],
};

/** The `sample/list` requests this file caused, cloned before MSW consumed them. */
let listRequests: Request[] = [];

/** Every request, so a test can also ask what the *mutations* sent. */
let allRequests: { path: string; body: () => Promise<Record<string, unknown>> }[] = [];

server.events.on('request:start', ({ request }) => {
  const path = new URL(request.url).pathname;
  const cloned = request.clone();
  allRequests.push({
    path,
    body: async () => (await cloned.json()) as Record<string, unknown>,
  });
  if (path === '/api/v1/sample/list') {
    listRequests.push(request.clone());
  }
});

async function requestBodies(): Promise<Record<string, unknown>[]> {
  return Promise.all(
    listRequests.map(async (request) => (await request.json()) as Record<string, unknown>),
  );
}

function renderScreen(
  options: {
    demo?: DemoLab;
    user?: CurrentUser;
    fail?: Partial<Record<RpcName, GrpcCode>>;
    /** Initial URL, for the `?q=` the shell's lookup box writes. */
    route?: string;
    /** Anything to render next to the screen — a link to navigate with, say. */
    extra?: ReactNode;
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
      <LookupScreen />
      {options.extra}
    </LabProvider>,
    { user: options.user ?? ADMIN, route: options.route ?? '/lookup' },
  );
}

/** The scan field. `combobox` because it drives the pick list below it. */
function field(): HTMLInputElement {
  return screen.getByRole('combobox', {
    name: lookupCopy.field.label,
  });
}

function form(): HTMLFormElement {
  const element = field().closest('form');
  if (element === null) {
    throw new Error('the lookup field is not inside a form');
  }
  return element;
}

/** One scan: the whole value arrives, then the scanner's Enter. */
function scan(term: string): void {
  fireEvent.change(field(), { target: { value: term } });
  fireEvent.submit(form());
}

/** The same, one keystroke at a time with no pause in between. */
function scanKeystrokeByKeystroke(term: string): void {
  for (const character of term) {
    fireEvent.change(field(), { target: { value: field().value + character } });
  }
  fireEvent.submit(form());
}

beforeEach(() => {
  listRequests = [];
  allRequests = [];
  globalThis.localStorage.clear();
});

describe('LookupScreen', () => {
  it('takes the focus as soon as the field is usable, so a scan needs no click', async () => {
    // The lab comes from the session, so the field is rendered before it can be
    // focused — and a disabled input silently drops `autoFocus`.
    renderScreen();

    await waitFor(() => {
      expect(document.activeElement).toBe(field());
    });
  });

  it('shows the location card, with status and a check-out button, for one barcode hit', async () => {
    renderScreen();

    scan(BARCODE_TERM);

    const card = await screen.findByRole('region', { name: BARCODE_NAME });
    expect(within(card).getByText(lookupCopy.results.one)).toBeInTheDocument();
    expect(within(card).getByText(BARCODE_TERM)).toBeInTheDocument();
    expect(within(card).getByText('Active')).toBeInTheDocument();
    expect(
      within(card).getByRole('button', { name: lookupCopy.checkout.action }),
    ).toBeInTheDocument();

    // G3.1's chain, reused rather than re-derived: freezer → … → position.
    const path = within(card).getByRole('list', { name: lookupCopy.results.location });
    expect(
      within(path)
        .getAllByRole('listitem')
        .map((item) => item.textContent),
    ).toEqual(['Freezer A', 'Rack 1', 'Top drawer', 'Box A', 'A1']);
  });

  it('takes the exact barcode over a name that contains the same characters', async () => {
    const demo = createDemoLab();
    demo.samples.push(
      create(SampleSchema, {
        id: 'sample-named-like-a-barcode',
        labId: LAB_ID,
        itemTypeId: 'it-serum',
        name: `${BARCODE_TERM} control`,
        barcode: 'DEMO-9999',
      }),
    );
    renderScreen({ demo });

    scan(BARCODE_TERM);

    expect(await screen.findByRole('region', { name: BARCODE_NAME })).toBeInTheDocument();
    // Two hits would mean the name search ran first, or in addition.
    expect(screen.queryByRole('listbox')).not.toBeInTheDocument();
  });

  it('falls back to a name search when no barcode matches', async () => {
    renderScreen();

    scan('Plasma A');

    const card = await screen.findByRole('region', { name: 'Plasma A' });
    expect(within(card).getByText('DEMO-0003')).toBeInTheDocument();
  });

  it('answers the query the shell lookup box put in the URL', async () => {
    // `GlobalLookup` navigates to `/lookup?q=…`; landing on an empty field
    // would silently drop what the user typed in the top bar.
    renderScreen({ route: `/lookup?q=${BARCODE_TERM}` });

    const card = await screen.findByRole('region', { name: BARCODE_NAME });
    expect(within(card).getByText(BARCODE_TERM)).toBeInTheDocument();
    expect(field().value).toBe(BARCODE_TERM);
    // And the loop still holds: the answer arrives ready for the next scan.
    expect(document.activeElement).toBe(field());
    expect(field().selectionEnd).toBe(BARCODE_TERM.length);
  });

  it('answers a new URL query without remounting the screen', async () => {
    renderScreen({
      route: `/lookup?q=${BARCODE_TERM}`,
      extra: <Link to="/lookup?q=DEMO-0002">next query</Link>,
    });
    await screen.findByRole('region', { name: BARCODE_NAME });

    fireEvent.click(screen.getByRole('link', { name: 'next query' }));

    expect(await screen.findByRole('region', { name: 'Serum B' })).toBeInTheDocument();
    expect(field().value).toBe('DEMO-0002');
  });

  it('lists several hits and can be driven with the arrow keys and Enter alone', async () => {
    renderScreen();

    scan('Serum');

    const list = await screen.findByRole('listbox', { name: lookupCopy.results.listLabel });
    expect(within(list).getAllByRole('option')).toHaveLength(2);
    expect(field()).toHaveAttribute('aria-expanded', 'true');

    // The field keeps the focus: that is what makes the next scan work.
    fireEvent.keyDown(field(), { key: 'ArrowDown' });
    expect(screen.getByRole('option', { selected: true })).toHaveAccessibleName(/Serum B/);

    fireEvent.keyDown(field(), { key: 'ArrowUp' });
    expect(screen.getByRole('option', { selected: true })).toHaveAccessibleName(/Serum A/);

    fireEvent.keyDown(field(), { key: 'ArrowDown' });
    fireEvent.keyDown(field(), { key: 'Enter' });

    expect(await screen.findByRole('region', { name: 'Serum B' })).toBeInTheDocument();
    expect(screen.queryByRole('listbox')).not.toBeInTheDocument();
  });

  it('says so when nothing matches, and does not dress it up as an error', async () => {
    renderScreen();

    scan('nothing-like-this');

    expect(await screen.findByText(lookupCopy.empty.title)).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    expect(screen.queryByRole('listbox')).not.toBeInTheDocument();
  });

  it('asks for two characters instead of sending a search the server would refuse', async () => {
    renderScreen();

    scan('D');

    expect(await screen.findByText(lookupCopy.tooShort.title)).toBeInTheDocument();
    expect(screen.queryByText(lookupCopy.empty.title)).not.toBeInTheDocument();
    expect(await requestBodies()).toHaveLength(1);
  });

  it('gives an unplaced sample its own card rather than an empty location', async () => {
    const demo = createDemoLab();
    demo.samples.push(
      create(SampleSchema, {
        id: 'sample-unplaced',
        labId: LAB_ID,
        itemTypeId: 'it-serum',
        name: 'Loose aliquot',
        barcode: 'DEMO-LOOSE',
      }),
    );
    renderScreen({ demo });

    scan('DEMO-LOOSE');

    const card = await screen.findByRole('region', { name: 'Loose aliquot' });
    expect(within(card).getByText(lookupCopy.results.unplacedTitle)).toBeInTheDocument();
    expect(within(card).queryByRole('list')).not.toBeInTheDocument();
  });

  it('says the location is unavailable when the sample names a box that is not there', async () => {
    const demo = createDemoLab();
    demo.samples.push(
      create(SampleSchema, {
        id: 'sample-orphaned-box',
        labId: LAB_ID,
        itemTypeId: 'it-serum',
        name: 'Orphan',
        barcode: 'DEMO-ORPHAN',
        boxId: 'box-deleted-long-ago',
        positionLabel: 'C3',
      }),
    );
    renderScreen({ demo });

    scan('DEMO-ORPHAN');

    const card = await screen.findByRole('region', { name: 'Orphan' });
    // Only reachable once the layout has loaded: while it is still loading the
    // card must not claim the location is missing.
    expect(await within(card).findByText(lookupCopy.results.unknownTitle)).toBeInTheDocument();
    expect(within(card).queryByRole('list')).not.toBeInTheDocument();
  });

  it('hands the field back focused with its text selected after every lookup', async () => {
    renderScreen();

    scan(BARCODE_TERM);
    await screen.findByRole('region', { name: BARCODE_NAME });

    // The second scan is typed straight over the first, with no click: that is
    // the whole difference between one-handed and two-handed lookup.
    expect(document.activeElement).toBe(field());
    expect(field().value).toBe(BARCODE_TERM);
    expect(field().selectionStart).toBe(0);
    expect(field().selectionEnd).toBe(BARCODE_TERM.length);

    fireEvent.change(field(), { target: { value: 'DEMO-0002' } });
    fireEvent.submit(form());

    await screen.findByRole('region', { name: 'Serum B' });
    expect(document.activeElement).toBe(field());
    expect(field().value).toBe('DEMO-0002');
    expect(field().selectionEnd).toBe('DEMO-0002'.length);
  });

  it('is not swallowed by a scanner burst, and searches the whole barcode exactly once', async () => {
    renderScreen();

    scanKeystrokeByKeystroke(BARCODE_TERM);

    await screen.findByRole('region', { name: BARCODE_NAME });

    // One request, carrying every character — not a search per keystroke, and
    // not a search for a prefix the debounce happened to see first.
    expect(listRequests).toHaveLength(1);
    expect((await requestBodies())[0]).toMatchObject({ barcode: BARCODE_TERM });

    // A scanner that delivers the whole string in one event behaves the same.
    scan('DEMO-0002');
    await screen.findByRole('region', { name: 'Serum B' });
    expect(listRequests).toHaveLength(2);
  });

  it('does not search while the user is still typing', () => {
    renderScreen();

    scanKeystrokeByKeystroke('DEMO-000');

    // Six characters, no Enter: the field is not a search-as-you-type box, and
    // a debounce that fires here is what makes a scan race its own Enter.
    expect(listRequests).toHaveLength(0);
    expect(screen.queryByRole('listbox')).not.toBeInTheDocument();
  });

  it('shows the error state, and still returns the field, when the lookup fails', async () => {
    renderScreen({ fail: { 'sample/list': 'UNAVAILABLE' } });

    scan(BARCODE_TERM);

    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent(lookupCopy.errorTitle);
    expect(document.activeElement).toBe(field());
  });

  it('checks the sample out from the card and shows the new status', async () => {
    renderScreen();

    scan(BARCODE_TERM);
    const card = await screen.findByRole('region', { name: BARCODE_NAME });

    fireEvent.click(within(card).getByRole('button', { name: lookupCopy.checkout.action }));

    expect(await within(card).findByText('Checked out')).toBeInTheDocument();
    const checkoutRequests = allRequests.filter(
      (request) => request.path === '/api/v1/sample/checkout',
    );
    expect(checkoutRequests).toHaveLength(1);
    expect(await checkoutRequests[0]?.body()).toMatchObject({
      sample_id: 'sample-1',
      action: 'CHECKOUT_ACTION_CHECKOUT',
    });
    // The next scan, not a second click, is what follows a check-out.
    expect(document.activeElement).toBe(field());
  });

  it('hides the check-out button from a member without the permission', async () => {
    renderScreen({ user: READ_ONLY });

    scan(BARCODE_TERM);

    const card = await screen.findByRole('region', { name: BARCODE_NAME });
    expect(
      within(card).queryByRole('button', { name: lookupCopy.checkout.action }),
    ).not.toBeInTheDocument();
  });

  it('disables the field when the user belongs to no lab', () => {
    renderScreen({ user: NO_LAB });

    expect(field()).toBeDisabled();
    expect(screen.getByText(lookupCopy.field.noLab)).toBeInTheDocument();
  });

  it('has no axe violations once a card is on screen', async () => {
    const { container } = renderScreen();

    scan(BARCODE_TERM);
    await screen.findByRole('region', { name: BARCODE_NAME });

    await waitFor(async () => {
      expect(await axe(container)).toHaveNoViolations();
    });
  });
});
