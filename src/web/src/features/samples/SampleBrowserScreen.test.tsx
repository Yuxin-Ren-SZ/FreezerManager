// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { act, fireEvent, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { Route, Routes, useLocation } from 'react-router-dom';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import samplesCopy from '../../../locales/en/samples.json';
import type { CurrentUser } from '../../app/session';
import { STUB_CURRENT_USER, STUB_CURRENT_USER_ALL_PERMISSIONS } from '../../app/stubSession';
import type { GrpcCode } from '../../api/errors';
import type { RpcName } from '../../api/routes';
import { SampleSchema, SampleStatus } from '../../gen/fmgr/v1/sample_pb';
import { FakeEventSource } from '../../test/fakeEventSource';
import { createDemoLab, fakeApi, seedSamples, type DemoLab } from '../../test/fakeApi';
import { renderWithProviders } from '../../test/render';
import { server } from '../../test/server';
import { axe } from '../../test/setup';
import { SampleBrowserScreen } from './SampleBrowserScreen';

/**
 * The sample browser (TODO.md G3.2): the screen people live in.
 *
 * The assertions worth reading twice are the ones about *not lying*:
 *
 *  - every filter has to survive in the URL, because a filtered view is a link
 *    someone pastes to a colleague;
 *  - a broken location must not read as "never placed";
 *  - a watch frame updates a list but is never a detail view's source of truth;
 *  - 100k rows must never become 100k DOM rows.
 */

const LAB_ID = 'lab-demo';
const SAMPLES_PATH = `/labs/${LAB_ID}/samples`;

/** The stub session, whose global permission list covers the whole catalog. */
const ADMIN: CurrentUser = STUB_CURRENT_USER_ALL_PERMISSIONS;

/**
 * A member who can read samples but not define custom fields.
 *
 * `custom-field-def/list` requires `custom_field.define`, so this is the user
 * whose column chooser has no custom columns — the case the screen has to
 * degrade into rather than fail on.
 */
const READ_ONLY: CurrentUser = {
  ...STUB_CURRENT_USER,
  permissions: ['sample.read'],
  labs: STUB_CURRENT_USER.labs.map((lab) => ({
    ...lab,
    labId: LAB_ID,
    permissions: ['sample.read'],
  })),
};

/**
 * The router's current URL, so a test can assert on what it would share.
 *
 * A `<span>`, not an `<output>`: an `<output>` carries the implicit ARIA role
 * `status`, which collides with the live-updates indicator.
 */
function LocationProbe() {
  const location = useLocation();
  return <span data-testid="location">{`${location.pathname}${location.search}`}</span>;
}

function currentUrl(): string {
  return screen.getByTestId('location').textContent;
}

function renderScreen(
  options: {
    labId?: string;
    demo?: DemoLab;
    route?: string;
    user?: CurrentUser;
    /** Per-RPC faults. They belong *in* the fake: `server.use` prepends, so a
     * later call would take precedence over the handlers installed here. */
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
    <>
      <Routes>
        <Route path="/labs/:labId/samples" element={<SampleBrowserScreen />} />
        <Route path="/labs/:labId/samples/:sampleId" element={<p>sample detail</p>} />
      </Routes>
      <LocationProbe />
    </>,
    { route: options.route ?? SAMPLES_PATH, user: options.user ?? ADMIN },
  );
}

function table(): HTMLElement {
  return screen.getByRole('table', { name: samplesCopy.table.caption });
}

/** The rows in the DOM, excluding the header (the spacers are `aria-hidden`). */
function dataRowCount(): number {
  return within(table()).getAllByRole('row').length - 1;
}

/**
 * Scroll the grid to the bottom, which is what asks for the next page.
 *
 * jsdom has no layout, so the virtualizer's viewport comes from the
 * `offsetHeight` spy below and its offset from `scrollTop`; the total height is
 * `rows × 40 px` (the table's default row height).
 */
function scrollToEnd(loadedRows: number): void {
  const element = table().parentElement;
  if (element === null) {
    throw new Error('the sample table is not inside a scroll container');
  }
  element.scrollTop = loadedRows * 40;
  fireEvent.scroll(element);
}

/** Push one watch frame through the open stream. */
function pushFrame(sample: Record<string, unknown>): void {
  act(() => {
    FakeEventSource.current().message(JSON.stringify({ lab_id: LAB_ID, ...sample }));
  });
}

beforeEach(() => {
  FakeEventSource.reset();
  // jsdom has no EventSource and no layout engine: the two spies
  // `src/ui/Table.test.tsx` documents, plus the global `subscribeSse` needs.
  vi.stubGlobal('EventSource', FakeEventSource);
  vi.spyOn(HTMLElement.prototype, 'offsetHeight', 'get').mockImplementation(function (
    this: HTMLElement,
  ) {
    return Number.parseFloat(this.style.maxHeight) || 0;
  });
  vi.spyOn(HTMLElement.prototype, 'offsetWidth', 'get').mockImplementation(() => 800);
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe('SampleBrowserScreen', () => {
  it('renders the lab samples in a virtualized table', async () => {
    renderScreen();

    expect(
      await screen.findByRole('heading', { level: 1, name: samplesCopy.title }),
    ).toBeInTheDocument();
    await screen.findByRole('table', { name: samplesCopy.table.caption });

    expect(within(table()).getByRole('cell', { name: 'Serum A' })).toBeInTheDocument();
    expect(within(table()).getByRole('cell', { name: 'Plasma A' })).toBeInTheDocument();
    // The other lab's sample is not in this lab's list.
    expect(within(table()).queryByRole('cell', { name: 'DNA A' })).not.toBeInTheDocument();
  });

  it('takes the row name to the sample detail route', async () => {
    renderScreen();

    // `fireEvent`, not `userEvent`: a virtualized row can be replaced between
    // the pointer events of a full user-event gesture in jsdom, and the click
    // then lands on a detached node. The navigation itself is what is asserted.
    fireEvent.click(await screen.findByRole('link', { name: 'Serum A' }));

    expect(await screen.findByText('sample detail')).toBeInTheDocument();
  });

  it('has no accessibility violations', async () => {
    const { container } = renderScreen();

    await screen.findByRole('table', { name: samplesCopy.table.caption });

    expect(await axe(container)).toHaveNoViolations();
  });

  describe('filters', () => {
    it.each([
      {
        name: 'status',
        label: samplesCopy.filters.status,
        value: 'SAMPLE_STATUS_CHECKED_OUT',
        parameter: 'status=SAMPLE_STATUS_CHECKED_OUT',
        visible: 'Plasma A',
        hidden: 'Serum A',
      },
      {
        name: 'item type',
        label: samplesCopy.filters.itemType,
        value: 'it-plasma',
        parameter: 'itemTypeId=it-plasma',
        visible: 'Plasma A',
        hidden: 'Serum A',
      },
      {
        name: 'box',
        label: samplesCopy.filters.box,
        value: 'box-2',
        parameter: 'boxId=box-2',
        visible: 'Plasma A',
        hidden: 'Serum A',
      },
    ])('keeps the $name filter in the URL and applies it', async (filter) => {
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.selectOptions(screen.getByLabelText(filter.label), filter.value);

      await waitFor(() => {
        expect(
          within(table()).queryByRole('cell', { name: filter.hidden }),
        ).not.toBeInTheDocument();
      });
      expect(within(table()).getByRole('cell', { name: filter.visible })).toBeInTheDocument();
      // The URL is the record: a reload, a bookmark or a paste into chat
      // restores exactly this view.
      expect(currentUrl()).toContain(filter.parameter);
    });

    it('keeps a typed barcode in the URL and matches it exactly', async () => {
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.type(screen.getByLabelText(samplesCopy.filters.barcode), 'DEMO-0002');

      await waitFor(() => {
        expect(within(table()).getByRole('cell', { name: 'Serum B' })).toBeInTheDocument();
      });
      expect(within(table()).queryByRole('cell', { name: 'Serum A' })).not.toBeInTheDocument();
      expect(currentUrl()).toContain('barcode=DEMO-0002');
    });

    it('holds a one-character search back instead of sending it to the server', async () => {
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.type(screen.getByLabelText(samplesCopy.filters.search), 'p');

      // `ListSamples` answers INVALID_ARGUMENT below two bytes; sending it would
      // turn "still typing" into an error state. The rows stay, the hint says
      // what to do, and the URL still carries what the user typed.
      expect(screen.getByText(samplesCopy.filters.searchTooShort)).toBeInTheDocument();
      expect(within(table()).getByRole('cell', { name: 'Serum A' })).toBeInTheDocument();
      expect(currentUrl()).toContain('q=p');
    });

    it('searches names and barcodes case-insensitively once there are two characters', async () => {
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.type(screen.getByLabelText(samplesCopy.filters.search), 'plasma');

      await waitFor(() => {
        expect(within(table()).getByRole('cell', { name: 'Plasma A' })).toBeInTheDocument();
      });
      expect(within(table()).queryByRole('cell', { name: 'Serum A' })).not.toBeInTheDocument();
    });

    it('clears every filter at once', async () => {
      renderScreen({ route: `${SAMPLES_PATH}?status=SAMPLE_STATUS_CHECKED_OUT&boxId=box-2` });
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.click(screen.getByRole('button', { name: samplesCopy.filters.clear }));

      await waitFor(() => {
        expect(within(table()).getByRole('cell', { name: 'Serum A' })).toBeInTheDocument();
      });
      expect(screen.getByLabelText(samplesCopy.filters.status)).toHaveValue('');
      expect(currentUrl()).toBe(SAMPLES_PATH);
    });

    it('ignores a filter value it does not understand, instead of erroring', async () => {
      renderScreen({ route: `${SAMPLES_PATH}?status=SAMPLE_STATUS_GONE&boxId=` });

      // A stale or hand-edited link degrades to "fewer filters"; the table is
      // still the lab's samples.
      await screen.findByRole('table', { name: samplesCopy.table.caption });
      expect(within(table()).getByRole('cell', { name: 'Serum A' })).toBeInTheDocument();
      expect(screen.getByLabelText(samplesCopy.filters.status)).toHaveValue('');
    });
  });

  describe('a shared link', () => {
    it('restores every filter on reload', async () => {
      renderScreen({
        route: `${SAMPLES_PATH}?status=SAMPLE_STATUS_CHECKED_OUT&itemTypeId=it-plasma&boxId=box-2&q=plasma`,
      });

      await screen.findByRole('table', { name: samplesCopy.table.caption });

      // Controls show what the link asked for…
      expect(screen.getByLabelText(samplesCopy.filters.status)).toHaveValue(
        'SAMPLE_STATUS_CHECKED_OUT',
      );
      expect(screen.getByLabelText(samplesCopy.filters.itemType)).toHaveValue('it-plasma');
      expect(screen.getByLabelText(samplesCopy.filters.box)).toHaveValue('box-2');
      expect(screen.getByLabelText(samplesCopy.filters.search)).toHaveValue('plasma');
      // …and the rows are the filtered ones.
      expect(within(table()).getByRole('cell', { name: 'Plasma A' })).toBeInTheDocument();
      expect(within(table()).queryByRole('cell', { name: 'Serum A' })).not.toBeInTheDocument();
    });

    it('is the whole URL: no filter state lives anywhere else', async () => {
      const { unmount } = renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });
      await userEvent.selectOptions(
        screen.getByLabelText(samplesCopy.filters.status),
        'SAMPLE_STATUS_ACTIVE',
      );
      const shared = currentUrl();

      unmount();
      renderScreen({ route: shared });

      await waitFor(() => {
        expect(screen.getByLabelText(samplesCopy.filters.status)).toHaveValue(
          'SAMPLE_STATUS_ACTIVE',
        );
      });
      // Wait for the restored view to finish loading, not just for the control:
      // the custom-field columns are the last request to land, and a test that
      // ends before it does leaves a request in flight past teardown.
      await waitFor(() => {
        expect(within(table()).getByRole('cell', { name: '12.5' })).toBeInTheDocument();
      });
    });
  });

  describe('paging', () => {
    it('loads one page at a time and asks for the next when the window reaches the end', async () => {
      renderScreen({ demo: seedSamples(createDemoLab(), 150) });

      await screen.findByRole('table', { name: samplesCopy.table.caption });
      // 100 rows in the cache, not 150: the first request was a page, and the
      // server said there is more.
      await waitFor(() => {
        expect(screen.getByText(samplesCopy.table.more)).toBeInTheDocument();
      });
      expect(screen.getByText(/of 100 rows/)).toBeInTheDocument();

      scrollToEnd(100);

      await waitFor(() => {
        expect(screen.getByText(/of 150 rows/)).toBeInTheDocument();
      });
      // The last page is short, so the sequence ends there.
      expect(screen.queryByText(samplesCopy.table.more)).not.toBeInTheDocument();
    });

    it('keeps 100k rows out of the DOM', async () => {
      renderScreen({ demo: seedSamples(createDemoLab(), 100_000) });

      await screen.findByRole('table', { name: samplesCopy.table.caption });
      await waitFor(() => {
        expect(screen.getByText(/of 100 rows/)).toBeInTheDocument();
      });

      // The virtualizer's window is what is in the DOM; the table is not.
      expect(dataRowCount()).toBeLessThan(100);
      expect(screen.queryByText('Serum 100000')).not.toBeInTheDocument();

      scrollToEnd(100);
      await waitFor(() => {
        expect(screen.getByText(/of 200 rows/)).toBeInTheDocument();
      });
      // Still a window, now over twice as many loaded rows.
      expect(dataRowCount()).toBeLessThan(100);
    });
  });

  describe('live updates', () => {
    it('merges a new sample, an update and a tombstone into the list', async () => {
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });
      act(() => {
        FakeEventSource.current().open();
      });

      // Three demo samples. A row *count*, not just a name: a tombstone that
      // replaced the row with a tombstoned copy would leave the grid the same
      // size and still hide the edited name — which is exactly how the weaker
      // version of this assertion passed under a planted violation.
      expect(dataRowCount()).toBe(3);

      pushFrame({ id: 'sample-9', name: 'Serum Z', status: 'SAMPLE_STATUS_ACTIVE' });
      expect(within(table()).getByRole('cell', { name: 'Serum Z' })).toBeInTheDocument();
      expect(dataRowCount()).toBe(4);

      pushFrame({ id: 'sample-1', name: 'Serum A (edited)', status: 'SAMPLE_STATUS_ACTIVE' });
      expect(within(table()).getByRole('cell', { name: 'Serum A (edited)' })).toBeInTheDocument();

      pushFrame({ id: 'sample-1', name: 'Serum A tombstoned', status: 'SAMPLE_STATUS_TOMBSTONED' });
      expect(
        within(table()).queryByRole('cell', { name: 'Serum A tombstoned' }),
      ).not.toBeInTheDocument();
      expect(
        within(table()).queryByRole('cell', { name: 'Serum A (edited)' }),
      ).not.toBeInTheDocument();
      await waitFor(() => {
        expect(dataRowCount()).toBe(3);
      });
    });

    it('scopes the feed to the box filter, which the route understands', async () => {
      renderScreen({ route: `${SAMPLES_PATH}?boxId=box-2` });
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      expect(FakeEventSource.current().url).toBe(
        `/api/v1/sample/watch?lab_id=${LAB_ID}&box_id=box-2`,
      );
    });

    it('says so when the live connection is down, and recovers after a reconnect', async () => {
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });
      act(() => {
        FakeEventSource.current().open();
      });
      expect(screen.getByRole('status')).toHaveTextContent(samplesCopy.live.live);

      vi.useFakeTimers();
      act(() => {
        FakeEventSource.current().transportError();
      });
      expect(screen.getByRole('status')).toHaveTextContent(samplesCopy.live.error);

      // The SSE wrapper owns the backoff; the screen's job is to keep merging
      // after it, which is what makes the feed survive a flaky network.
      await act(async () => {
        await vi.advanceTimersByTimeAsync(1_100);
      });
      expect(FakeEventSource.all()).toHaveLength(2);

      act(() => {
        FakeEventSource.current().open();
        FakeEventSource.current().message(
          JSON.stringify({
            id: 'sample-2',
            lab_id: LAB_ID,
            name: 'Serum B (live)',
            status: 'SAMPLE_STATUS_ACTIVE',
          }),
        );
      });
      expect(screen.getByRole('status')).toHaveTextContent(samplesCopy.live.live);
      expect(within(table()).getByRole('cell', { name: 'Serum B (live)' })).toBeInTheDocument();
    });
  });

  describe('placement', () => {
    it('tells a broken location apart from a sample that is not in a box', async () => {
      const lab = createDemoLab();
      // One sample names a box the layout does not contain, one has no box at
      // all. G3.1's resolver answers `{placed: false, partial: true}` for the
      // first — "unknown", never "never placed".
      lab.samples.push(
        create(SampleSchema, {
          id: 'sample-ghost',
          labId: LAB_ID,
          itemTypeId: 'it-serum',
          name: 'Ghost box',
          boxId: 'box-ghost',
          status: SampleStatus.ACTIVE,
        }),
        create(SampleSchema, {
          id: 'sample-no-box',
          labId: LAB_ID,
          itemTypeId: 'it-serum',
          name: 'Nobody',
          status: SampleStatus.ACTIVE,
        }),
      );
      renderScreen({ demo: lab });

      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await waitFor(() => {
        expect(
          within(table()).getByRole('cell', { name: samplesCopy.placement.unknown }),
        ).toBeInTheDocument();
      });
      expect(
        within(table()).getByRole('cell', { name: samplesCopy.placement.unplaced }),
      ).toBeInTheDocument();
      expect(samplesCopy.placement.unknown).not.toBe(samplesCopy.placement.unplaced);
    });

    it('shows the full path for a placed sample', async () => {
      renderScreen();

      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await waitFor(() => {
        expect(
          within(table()).getByRole('cell', {
            name: 'Freezer A › Rack 1 › Top drawer › Box A › A1',
          }),
        ).toBeInTheDocument();
      });
    });
  });

  describe('custom-field columns', () => {
    it('offers a column per field definition', async () => {
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.click(screen.getByText('Columns'));

      expect(
        screen.getByRole('checkbox', { name: 'Show or hide the Concentration column' }),
      ).toBeInTheDocument();
      expect(
        screen.getByRole('checkbox', { name: 'Show or hide the Freeze/thaw count column' }),
      ).toBeInTheDocument();
    });

    it('shows each row its own values', async () => {
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      // The definitions are a second request, so the columns appear with them.
      await waitFor(() => {
        expect(within(table()).getByRole('cell', { name: '12.5' })).toBeInTheDocument();
      });
      // Scoped to the row on purpose: the demo lab holds a `3` in two different
      // samples' custom fields (Plasma A's freeze/thaw count and Serum A's
      // aliquot count), and an unscoped query would match both.
      const serumRow = within(table()).getByRole('row', { name: /Serum A/ });
      expect(within(serumRow).getByRole('cell', { name: '3' })).toBeInTheDocument();
    });

    it('degrades to the base columns for a member without custom_field.define', async () => {
      renderScreen({ user: READ_ONLY });
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.click(screen.getByText('Columns'));

      expect(screen.queryByRole('checkbox', { name: /Concentration/ })).not.toBeInTheDocument();
      // The screen itself is intact: the list is what the member came for.
      expect(within(table()).getByRole('cell', { name: 'Serum A' })).toBeInTheDocument();
    });
  });

  describe('export', () => {
    const createObjectURL = vi.fn((_blob: Blob) => 'blob:csv');
    const revokeObjectURL = vi.fn((_url: string) => undefined);

    beforeEach(() => {
      // jsdom implements neither, so this fills a hole rather than replacing a
      // working default.
      vi.stubGlobal(
        'URL',
        Object.assign(class extends URL {}, { createObjectURL, revokeObjectURL }),
      );
    });

    it('downloads samples-<lab>-<date>.csv from sample/export', async () => {
      const clicked: HTMLAnchorElement[] = [];
      vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function (
        this: HTMLAnchorElement,
      ) {
        clicked.push(this);
      });
      renderScreen();
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.click(screen.getByRole('button', { name: samplesCopy.export.action }));

      await waitFor(() => {
        expect(clicked).toHaveLength(1);
      });
      expect(clicked[0]?.download).toMatch(/^samples-lab-demo-\d{4}-\d{2}-\d{2}\.csv$/);
      // The body is the server's CSV, not a client-side rendering of the
      // loaded rows — which is also why the export ignores the filters.
      const blob = createObjectURL.mock.calls[0]?.[0];
      await expect(blob.text()).resolves.toContain('id,lab_id,item_type_id,name');
    });

    it('reports an export the server refused, without disturbing the list', async () => {
      renderScreen({ fail: { 'sample/export': 'PERMISSION_DENIED' } });
      await screen.findByRole('table', { name: samplesCopy.table.caption });

      await userEvent.click(screen.getByRole('button', { name: samplesCopy.export.action }));

      expect(await screen.findByRole('alert')).toHaveTextContent(samplesCopy.export.errorTitle);
      expect(within(table()).getByRole('cell', { name: 'Serum A' })).toBeInTheDocument();
    });
  });

  describe('failures', () => {
    it('shows the refusal when the list is not permitted', async () => {
      renderScreen({ fail: { 'sample/list': 'PERMISSION_DENIED' } });

      expect(await screen.findByText(samplesCopy.errorTitle)).toBeInTheDocument();
      expect(screen.getByText(/do not have permission/)).toBeInTheDocument();
      expect(screen.queryByRole('table')).not.toBeInTheDocument();
    });

    it('offers a retry when the connection failed', async () => {
      renderScreen({ fail: { 'sample/list': 'UNAVAILABLE' } });

      expect(await screen.findByText(samplesCopy.errorTitle)).toBeInTheDocument();
      expect(screen.getByText(/could not be reached/)).toBeInTheDocument();
      expect(screen.getByRole('button', { name: 'Try again' })).toBeInTheDocument();
    });

    it('tells an empty lab apart from an empty filter result', async () => {
      const empty = createDemoLab();
      empty.samples = empty.samples.filter((sample) => sample.labId !== LAB_ID);
      const { unmount } = renderScreen({ demo: empty });

      expect(await screen.findByText(samplesCopy.table.emptyTitle)).toBeInTheDocument();
      unmount();

      renderScreen({ route: `${SAMPLES_PATH}?barcode=DEMO-NOPE` });

      expect(await screen.findByText(samplesCopy.table.emptyFilteredTitle)).toBeInTheDocument();
      expect(screen.getByRole('button', { name: samplesCopy.filters.clear })).toBeInTheDocument();
    });

    it('keeps working when the item-type list fails', async () => {
      renderScreen({ fail: { 'item-type/list': 'PERMISSION_DENIED' } });

      // The rows still render; the item-type cell falls back to the id rather
      // than blanking, because a blank cell reads as "no item type".
      await screen.findByRole('table', { name: samplesCopy.table.caption });
      // Two of the three demo samples are serum, so this is a `getAllBy`.
      expect(within(table()).getAllByRole('cell', { name: 'it-serum' }).length).toBeGreaterThan(0);
    });
  });
});
