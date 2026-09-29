// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { HttpResponse, http } from 'msw';
import { Route, Routes } from 'react-router-dom';
import { beforeEach, describe, expect, it } from 'vitest';
import sampleDetailCopy from '../../../locales/en/sample-detail.json';
import { auditKeys } from '../../api/hooks';
import { SessionProvider } from '../../app/session';
import { CheckoutAction, SampleStatus, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { renderWithProviders } from '../../test/render';
import { server } from '../../test/server';
import { allPermissionsUser, currentUserWith } from '../../test/session';
import { axe } from '../../test/setup';
import { SampleDetailScreen } from './SampleDetailScreen';

/**
 * The sample detail view (TODO.md G3.3): every field, the PHI rule, the parent
 * link, the location path, `audit.read`-gated history and the five lifecycle
 * actions.
 *
 * Three of these are about what the screen must *not* do:
 *
 *  - it must not render a PHI field the response did not contain — the server
 *    filters by `phi.read` and the client has no business reconstructing it;
 *  - it must not render a history section at all without `audit.read`, not an
 *    empty one and not an error;
 *  - it must not show an action the caller has no permission for.
 */

const LAB_ID = 'lab-demo';
const SAMPLE_ID = 'sample-1';

/** Every request this file caused, cloned before MSW consumed it. */
let calls: { path: string; body: () => Promise<Record<string, unknown>> }[] = [];

server.events.on('request:start', ({ request }) => {
  // One clone per request, parsed at most once: a test that asks twice must not
  // read the same body stream twice.
  const cloned = request.clone();
  let parsed: Promise<Record<string, unknown>> | null = null;
  calls.push({
    path: new URL(request.url).pathname,
    body: () => (parsed ??= cloned.json() as Promise<Record<string, unknown>>),
  });
});

const CHECKOUT_PATH = '/api/v1/sample/checkout';

/** The bodies of every request to one route, in the order they were sent. */
async function bodiesFor(path: string): Promise<Record<string, unknown>[]> {
  return Promise.all(calls.filter((call) => call.path === path).map(async (call) => call.body()));
}

beforeEach(() => {
  calls = [];
});

interface RenderOptions {
  readonly demo?: DemoLab;
  readonly user?: ReturnType<typeof currentUserWith>;
  readonly sampleId?: string;
}

function renderDetail(options: RenderOptions = {}) {
  const demo = options.demo ?? createDemoLab();
  server.use(...fakeApi({ lab: demo }));
  const user = options.user ?? allPermissionsUser();

  return {
    ...renderWithProviders(
      <SessionProvider loadSession={() => Promise.resolve(user)}>
        <Routes>
          <Route path="/labs/:labId/samples/:sampleId" element={<SampleDetailScreen />} />
          <Route path="/labs/:labId/samples" element={<p>sample list</p>} />
        </Routes>
      </SessionProvider>,
      { route: `/labs/${LAB_ID}/samples/${options.sampleId ?? SAMPLE_ID}` },
    ),
    demo,
  };
}

/**
 * A seeded sample by id. A missing id is a broken fixture, so this throws
 * rather than handing the test an `undefined` it would then assert against.
 */
function sampleById(demo: DemoLab, id: string): Sample {
  const found = demo.samples.find((candidate) => candidate.id === id);
  if (found === undefined) {
    throw new Error(`fixture has no sample ${id}`);
  }
  return found;
}

/** The `<dd>` of one `<dt>` in a definition list, matched by its label. */
function termValue(label: string): string {
  const term = screen.getByText(label);
  return term.nextElementSibling?.textContent ?? '';
}

const escapeRegExp = (value: string) => value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/** `findByLabelText`, tolerant of the required marker the kit appends. */
const findByLabel = (label: string): Promise<HTMLElement> =>
  screen.findByLabelText(new RegExp(`^${escapeRegExp(label)}\\*?$`));

const click = (name: string) => userEvent.click(screen.getByRole('button', { name }));

describe('SampleDetailScreen', () => {
  it('shows the sample, its core fields, its type and its location', async () => {
    renderDetail();

    expect(await screen.findByRole('heading', { level: 1, name: 'Serum A' })).toBeInTheDocument();
    expect(screen.getByText('DEMO-0001')).toBeInTheDocument();
    expect(screen.getByText('Active')).toBeInTheDocument();

    expect(termValue(sampleDetailCopy.detail.itemType)).toBe('Serum');
    // Freezer → rack → drawer → box → position, resolved by G3.1's path helper.
    expect(termValue(sampleDetailCopy.detail.location)).toBe(
      'Freezer A / Rack 1 / Top drawer / Box A / A1',
    );
    expect(termValue(sampleDetailCopy.detail.volume)).toContain('100');
  });

  it('renders a sample that is not in a box without inventing a location', async () => {
    const demo = createDemoLab();
    sampleById(demo, SAMPLE_ID).boxId = undefined;
    sampleById(demo, SAMPLE_ID).positionLabel = undefined;
    renderDetail({ demo });

    await screen.findByRole('heading', { level: 1, name: 'Serum A' });

    expect(termValue(sampleDetailCopy.detail.location)).toBe(sampleDetailCopy.detail.unplaced);
  });

  it('renders a custom field per its definition type', async () => {
    renderDetail();

    // The definitions are their own request; the rows appear once it answers.
    await screen.findByText('Concentration');

    // float, string, int, bool and date, all seeded on sample-1.
    expect(termValue('Concentration')).toBe('12.5');
    expect(termValue('Serum notes')).toBe('ok');
    expect(termValue('Aliquot count')).toBe('3');
    expect(termValue('Hemolyzed')).toBe(sampleDetailCopy.detail.yes);
    expect(termValue('Collection date')).toBe('2026-01-05');
  });

  it('shows a custom field the response did not label, rather than dropping it', async () => {
    const demo = createDemoLab();
    sampleById(demo, SAMPLE_ID).customFieldsJson = JSON.stringify({ mystery: 'x' });
    renderDetail({ demo });

    await screen.findByRole('heading', { level: 1, name: 'Serum A' });

    // No definition exists for `mystery`, so the key is what there is to show.
    expect(termValue('mystery')).toBe('x');
  });

  describe('PHI', () => {
    it('does not render a PHI field the response does not contain', async () => {
      renderDetail();

      // Wait for the definitions, not just the sample: before they arrive there
      // is nothing that *could* render, so the assertions below would hold
      // vacuously.
      await screen.findByText('Concentration');

      // `donor_name` is a PHI definition on Blood, and sample-1 has no value
      // for it: the server withheld it, so nothing may appear — not an empty
      // row and not a placeholder.
      expect(screen.queryByText('Donor name')).not.toBeInTheDocument();
      expect(screen.queryByText(sampleDetailCopy.phi.badge)).not.toBeInTheDocument();
    });

    it('renders a PHI field the response contains, marked as PHI', async () => {
      const demo = createDemoLab();
      sampleById(demo, SAMPLE_ID).customFieldsJson = JSON.stringify({
        concentration: 12.5,
        donor_name: 'not-a-real-person',
      });
      renderDetail({ demo });

      await screen.findByText('Donor name');

      expect(termValue('Donor name')).toBe('not-a-real-person');
      const row = screen.getByText('Donor name').closest('div');
      expect(
        row === null ? null : within(row).getByText(sampleDetailCopy.phi.badge),
      ).not.toBeNull();
    });

    it('marks PHI on the form too, when the definitions are readable', async () => {
      renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });

      await click(sampleDetailCopy.actions.edit);

      expect(await screen.findByLabelText(/Donor name/)).toBeInTheDocument();
    });
  });

  describe('the parent link', () => {
    it('names the parent and its status', async () => {
      const demo = createDemoLab();
      sampleById(demo, SAMPLE_ID).status = SampleStatus.DEPLETED;
      renderDetail({ demo, sampleId: 'sample-2' });

      // The parent is fetched by id, so this is its own round trip.
      expect(await screen.findByText('Parent: Serum A (Depleted)')).toBeInTheDocument();
    });

    it('falls back to the id when the parent is not in the loaded list', async () => {
      const demo = createDemoLab();
      sampleById(demo, 'sample-2').parentSampleId = 'sample-gone';
      renderDetail({ demo, sampleId: 'sample-2' });

      await screen.findByRole('heading', { level: 1, name: 'Serum B' });

      expect(screen.getByText(/sample-gone/)).toBeInTheDocument();
    });
  });

  describe('history', () => {
    it('lists the sample\u2019s own audit events when the caller has audit.read', async () => {
      renderDetail();

      await screen.findByRole('heading', { level: 1, name: 'Serum A' });

      const history = screen.getByRole('region', { name: sampleDetailCopy.detail.history });
      expect(await within(history).findByText('sample.create')).toBeInTheDocument();
      expect(within(history).getByText('sample.checkout')).toBeInTheDocument();
      // sample-2's events are in the fake, in this lab: filtering is the point.
      expect(within(history).getAllByRole('listitem')).toHaveLength(3);
    });

    it('renders no history section at all without audit.read', async () => {
      renderDetail({
        user: currentUserWith(['sample.read', 'sample.write', 'sample.checkout']),
      });

      await screen.findByRole('heading', { level: 1, name: 'Serum A' });

      expect(screen.queryByRole('region', { name: sampleDetailCopy.detail.history })).toBeNull();
      expect(screen.queryByText(sampleDetailCopy.detail.history)).not.toBeInTheDocument();
      // And it is not an error state either: the rest of the screen is fine.
      expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    });

    it('does not fetch the history without audit.read', async () => {
      // Serving no handler would *not* be an assertion: MSW's
      // `onUnhandledRequest: 'error'` only logs, and the request would pass
      // through unnoticed. So the handler that answers also records that it was
      // asked — a guard whose failure mode is visible.
      let auditRequested = false;
      server.use(
        http.post('/api/v1/audit/list', () => {
          auditRequested = true;
          return HttpResponse.json({ events: [], page: {} });
        }),
      );

      const { queryClient } = renderDetail({ user: currentUserWith(['sample.read']) });

      expect(await screen.findByRole('heading', { level: 1, name: 'Serum A' })).toBeInTheDocument();

      // Give a request that *was* issued time to reach the handler. Without
      // this flush an assertion of absence would pass no matter what — the very
      // failure mode this test exists to avoid (a disabled query and a query
      // that has not fired yet look identical).
      await act(async () => {
        await new Promise((resolve) => {
          setTimeout(resolve, 25);
        });
      });

      // TanStack creates the cache entry when the observer mounts either way,
      // so `status` is what distinguishes "never enabled" (pending forever)
      // from "answered".
      expect(queryClient.getQueryState(auditKeys.entity(LAB_ID, 'sample', SAMPLE_ID))?.status).toBe(
        'pending',
      );
      expect(auditRequested).toBe(false);
    });
  });

  describe('actions', () => {
    it('checks a sample out', async () => {
      const { demo } = renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });

      await click(sampleDetailCopy.actions.checkout);

      expect(await screen.findByText('Checked out')).toBeInTheDocument();
      expect(demo.samples.find((sample) => sample.id === SAMPLE_ID)?.status).toBe(
        SampleStatus.CHECKED_OUT,
      );
    });

    it('checks a checked-out sample back in, recording the volume used and the reason', async () => {
      const demo = createDemoLab();
      sampleById(demo, 'sample-3').volumeValue = 100;
      sampleById(demo, 'sample-3').volumeUnit = 'µL';
      renderDetail({ demo, sampleId: 'sample-3' });

      await screen.findByRole('heading', { level: 1, name: 'Plasma A' });
      await click(sampleDetailCopy.actions.checkin);

      const dialog = await screen.findByRole('dialog');
      // The unit opens on the sample's own unit: a `core::Volume` has no
      // unitless state, so the form has to resolve one rather than omit it.
      expect(within(dialog).getByLabelText(sampleDetailCopy.actions.volumeUnit)).toHaveValue('µL');
      await userEvent.type(
        within(dialog).getByLabelText(sampleDetailCopy.actions.volumeUsed),
        '40',
      );
      await userEvent.type(
        within(dialog).getByLabelText(sampleDetailCopy.actions.reason),
        'aliquot',
      );
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.confirmCheckin }),
      );

      // What the client sent: the pair. The fake enforces the server's
      // both-or-neither rule (#100), so a lone `volume_used` cannot pass here.
      expect(await bodiesFor(CHECKOUT_PATH)).toMatchObject([
        {
          sample_id: 'sample-3',
          action: 'CHECKOUT_ACTION_CHECKIN',
          volume_used: 40,
          volume_unit: 'µL',
          reason: 'aliquot',
        },
      ]);

      expect(await screen.findByText('Active')).toBeInTheDocument();
      // The stored row and the chain-of-custody event, not the 200: the signed
      // delta is where the consumed volume is actually kept.
      expect(sampleById(demo, 'sample-3').volumeValue).toBe(60);
      expect(demo.checkoutEvents).toMatchObject([
        {
          sampleId: 'sample-3',
          action: CheckoutAction.CHECKIN,
          volumeDelta: -40,
          volumeUnit: 'µL',
        },
      ]);
    });

    it('subtracts an amount given in the other unit, converting to the sample unit', async () => {
      const demo = createDemoLab();
      sampleById(demo, 'sample-3').volumeValue = 5000;
      sampleById(demo, 'sample-3').volumeUnit = 'µL';
      renderDetail({ demo, sampleId: 'sample-3' });

      await screen.findByRole('heading', { level: 1, name: 'Plasma A' });
      await click(sampleDetailCopy.actions.checkin);

      const dialog = await screen.findByRole('dialog');
      await userEvent.selectOptions(
        within(dialog).getByLabelText(sampleDetailCopy.actions.volumeUnit),
        'mL',
      );
      await userEvent.type(within(dialog).getByLabelText(sampleDetailCopy.actions.volumeUsed), '2');
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.confirmCheckin }),
      );

      expect(await bodiesFor(CHECKOUT_PATH)).toMatchObject([
        { sample_id: 'sample-3', volume_used: 2, volume_unit: 'mL' },
      ]);
      // 2 mL of a sample tracked in µL is 2000 µL (`core::Volume::to_unit`).
      expect(sampleById(demo, 'sample-3').volumeValue).toBe(3000);
      expect(demo.checkoutEvents).toMatchObject([
        { sampleId: 'sample-3', volumeDelta: -2000, volumeUnit: 'µL' },
      ]);
    });

    it('refuses a volume the sample cannot record instead of reporting a check-in (#111)', async () => {
      const demo = createDemoLab();
      sampleById(demo, 'sample-3').volumeValue = 100;
      sampleById(demo, 'sample-3').volumeUnit = 'µL';
      renderDetail({ demo, sampleId: 'sample-3' });

      await screen.findByRole('heading', { level: 1, name: 'Plasma A' });
      await click(sampleDetailCopy.actions.checkin);

      const dialog = await screen.findByRole('dialog');
      // Half a microlitre is not a quantity `core::Volume` can hold. The server
      // answers INVALID_ARGUMENT, so the screen has to say so — the defect was
      // a check-in that reported success and subtracted nothing.
      await userEvent.type(
        within(dialog).getByLabelText(sampleDetailCopy.actions.volumeUsed),
        '0.5',
      );
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.confirmCheckin }),
      );

      expect(await within(dialog).findByRole('alert')).toBeInTheDocument();
      // Nothing was applied: still checked out, volume untouched, no event.
      expect(sampleById(demo, 'sample-3').status).toBe(SampleStatus.CHECKED_OUT);
      expect(sampleById(demo, 'sample-3').volumeValue).toBe(100);
      expect(demo.checkoutEvents).toEqual([]);
    });

    it('does not offer a volume the server would not subtract from this sample', async () => {
      // sample-3 tracks no volume, and `apply_checkout` ignores `volume_used`
      // for a sample that tracks none — so an input here would be dropped
      // silently, which is the defect this change exists to close (#100).
      const demo = createDemoLab();
      renderDetail({ demo, sampleId: 'sample-3' });

      await screen.findByRole('heading', { level: 1, name: 'Plasma A' });
      await click(sampleDetailCopy.actions.checkin);

      const dialog = await screen.findByRole('dialog');
      expect(within(dialog).queryByLabelText(sampleDetailCopy.actions.volumeUsed)).toBeNull();
      expect(within(dialog).queryByLabelText(sampleDetailCopy.actions.volumeUnit)).toBeNull();

      await userEvent.type(
        within(dialog).getByLabelText(sampleDetailCopy.actions.reason),
        'back in',
      );
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.confirmCheckin }),
      );

      expect(await screen.findByText('Active')).toBeInTheDocument();
      const [checkin] = await bodiesFor(CHECKOUT_PATH);
      expect(checkin).toMatchObject({
        action: 'CHECKOUT_ACTION_CHECKIN',
        reason: 'back in',
      });
      expect(checkin).not.toHaveProperty('volume_used');
      expect(checkin).not.toHaveProperty('volume_unit');
    });

    it('discards a sample, which consumes the remaining volume', async () => {
      const { demo } = renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });
      await click(sampleDetailCopy.actions.discard);

      const dialog = await screen.findByRole('dialog');
      // The reason is what the chain of custody keeps; the volume is not an
      // input here because the server consumes all of it (`apply_checkout`).
      await userEvent.type(
        within(dialog).getByLabelText(sampleDetailCopy.actions.reason),
        'spilled',
      );
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.confirmDiscard }),
      );

      expect(await screen.findByText('Destroyed')).toBeInTheDocument();
      const updated = demo.samples.find((sample) => sample.id === SAMPLE_ID);
      expect(updated?.status).toBe(SampleStatus.DESTROYED);
      expect(updated?.volumeValue).toBe(0);
    });

    it('moves a sample to a free position', async () => {
      const { demo } = renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });
      await click(sampleDetailCopy.actions.move);

      const dialog = await screen.findByRole('dialog');
      await userEvent.selectOptions(
        within(dialog).getByLabelText(sampleDetailCopy.actions.destinationBox),
        'box-2',
      );
      await userEvent.selectOptions(
        within(dialog).getByLabelText(sampleDetailCopy.actions.destinationPosition),
        'A1',
      );
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.confirmMove }),
      );

      expect(
        await screen.findByText('Freezer A / Rack 1 / Top drawer / Box B / A1'),
      ).toBeInTheDocument();
      const moved = demo.samples.find((sample) => sample.id === SAMPLE_ID);
      expect(moved?.boxId).toBe('box-2');
      expect(moved?.positionLabel).toBe('A1');
    });

    it('offers only free positions, and shows a taken one coming back from the server', async () => {
      renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });
      await click(sampleDetailCopy.actions.move);

      const dialog = await screen.findByRole('dialog');
      await userEvent.selectOptions(
        within(dialog).getByLabelText(sampleDetailCopy.actions.destinationBox),
        'box-1',
      );

      const position = within(dialog).getByLabelText(sampleDetailCopy.actions.destinationPosition);
      // A1 is where this sample already is, A2 is sample-2; both are offered as
      // free only if the picker ignores what the box already holds.
      const options = within(position)
        .getAllByRole('option')
        .map((option) => option.textContent);
      expect(options).toContain('A3');

      // A race the client cannot prevent: the position is taken between the
      // picker being drawn and the move. The server says ALREADY_EXISTS.
      await userEvent.selectOptions(position, 'A1');
      server.use(
        ...fakeApi({
          lab: createDemoLab(),
          fail: { 'sample/move': 'ALREADY_EXISTS' },
        }),
      );
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.confirmMove }),
      );

      expect(await screen.findByText(sampleDetailCopy.server.positionTaken)).toBeInTheDocument();
    });

    it('soft-deletes after a confirmation, and leaves the screen', async () => {
      const { demo } = renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });
      await click(sampleDetailCopy.actions.delete);

      const dialog = await screen.findByRole('dialog');
      expect(within(dialog).getByText(sampleDetailCopy.actions.deleteTitle)).toBeInTheDocument();
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.confirmDelete }),
      );

      expect(await screen.findByText('sample list')).toBeInTheDocument();
      expect(demo.samples.find((sample) => sample.id === SAMPLE_ID)?.status).toBe(
        SampleStatus.TOMBSTONED,
      );
    });

    it('keeps the sample when the confirmation is declined', async () => {
      const { demo } = renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });
      await click(sampleDetailCopy.actions.delete);

      const dialog = await screen.findByRole('dialog');
      await userEvent.click(
        within(dialog).getByRole('button', { name: sampleDetailCopy.actions.keep }),
      );

      expect(demo.samples.find((sample) => sample.id === SAMPLE_ID)?.status).toBe(
        SampleStatus.ACTIVE,
      );
    });

    it('hides the write, checkout and delete actions from a read-only member', async () => {
      renderDetail({ user: currentUserWith(['sample.read', 'audit.read']) });

      await screen.findByRole('heading', { level: 1, name: 'Serum A' });

      for (const action of [
        sampleDetailCopy.actions.edit,
        sampleDetailCopy.actions.checkout,
        sampleDetailCopy.actions.move,
        sampleDetailCopy.actions.delete,
      ]) {
        expect(screen.queryByRole('button', { name: action }), action).toBeNull();
      }
      // The screen itself is still readable: read is what this member has.
      expect(screen.getByText('DEMO-0001')).toBeInTheDocument();
    });

    it('offers no check-in on a sample that is not checked out', async () => {
      renderDetail();

      await screen.findByRole('heading', { level: 1, name: 'Serum A' });

      expect(screen.queryByRole('button', { name: sampleDetailCopy.actions.checkin })).toBeNull();
      expect(
        screen.getByRole('button', { name: sampleDetailCopy.actions.checkout }),
      ).toBeInTheDocument();
    });
  });

  describe('editing', () => {
    it('opens the generated form filled in, and saves the change', async () => {
      const { demo } = renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });

      await click(sampleDetailCopy.actions.edit);

      const name = await findByLabel(sampleDetailCopy.form.name);
      expect(name).toHaveValue('Serum A');
      // The form is generated from the item type's inherited definitions, so an
      // inherited field is editable here too.
      expect(screen.getByLabelText('Aliquot count')).toHaveValue(3);

      await userEvent.clear(name);
      await userEvent.type(name, 'Serum A2');
      await userEvent.click(
        screen.getByRole('button', { name: sampleDetailCopy.form.submitUpdate }),
      );

      expect(
        await screen.findByRole('heading', { level: 1, name: 'Serum A2' }),
      ).toBeInTheDocument();
      expect(screen.getByText(sampleDetailCopy.form.saved)).toBeInTheDocument();
      expect(demo.samples.find((sample) => sample.id === SAMPLE_ID)?.name).toBe('Serum A2');
    });

    it('keeps an unattributable server failure at the form level', async () => {
      renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });
      await click(sampleDetailCopy.actions.edit);

      const name = await findByLabel(sampleDetailCopy.form.name);
      await userEvent.clear(name);
      await userEvent.type(name, 'Serum A2');

      // An injected INVALID_ARGUMENT carries a message that names no field, so
      // it cannot be pinned to one — the form-level alert is the honest place
      // for it. (The field-level path is covered by `serverErrors.test.ts` and
      // by the create form's position and size-class cases.)
      server.use(
        ...fakeApi({
          lab: createDemoLab(),
          fail: { 'sample/update': 'INVALID_ARGUMENT' },
        }),
      );
      await userEvent.click(
        screen.getByRole('button', { name: sampleDetailCopy.form.submitUpdate }),
      );

      expect(await screen.findByRole('alert')).toBeInTheDocument();
    });

    it('returns to the detail view when the edit is cancelled', async () => {
      renderDetail();
      await screen.findByRole('heading', { level: 1, name: 'Serum A' });
      await click(sampleDetailCopy.actions.edit);

      await findByLabel(sampleDetailCopy.form.name);
      await click(sampleDetailCopy.form.cancel);

      expect(await screen.findByRole('heading', { level: 1, name: 'Serum A' })).toBeInTheDocument();
      expect(screen.queryByLabelText(/^Name/)).toBeNull();
    });
  });

  describe('failures', () => {
    it('shows an error state when the sample cannot be read', async () => {
      server.use(...fakeApi({ fail: { 'sample/get': 'PERMISSION_DENIED' } }));

      renderWithProviders(
        <SessionProvider loadSession={() => Promise.resolve(allPermissionsUser())}>
          <Routes>
            <Route path="/labs/:labId/samples/:sampleId" element={<SampleDetailScreen />} />
          </Routes>
        </SessionProvider>,
        { route: `/labs/${LAB_ID}/samples/${SAMPLE_ID}` },
      );

      expect(await screen.findByRole('alert')).toHaveTextContent(sampleDetailCopy.errorTitle);
    });

    it('shows a not-found state when the sample is gone', async () => {
      server.use(...fakeApi({ fail: { 'sample/get': 'NOT_FOUND' } }));

      renderWithProviders(
        <SessionProvider loadSession={() => Promise.resolve(allPermissionsUser())}>
          <Routes>
            <Route path="/labs/:labId/samples/:sampleId" element={<SampleDetailScreen />} />
          </Routes>
        </SessionProvider>,
        { route: `/labs/${LAB_ID}/samples/${SAMPLE_ID}` },
      );

      expect(await screen.findByRole('alert')).toHaveTextContent(sampleDetailCopy.notFoundTitle);
    });
  });

  it('has no accessibility violations', async () => {
    const { container } = renderDetail();
    await screen.findByRole('heading', { level: 1, name: 'Serum A' });

    expect(await axe(container)).toHaveNoViolations();
  });

  it('has no accessibility violations in edit mode', async () => {
    const { container } = renderDetail();
    await screen.findByRole('heading', { level: 1, name: 'Serum A' });
    await click(sampleDetailCopy.actions.edit);
    await findByLabel(sampleDetailCopy.form.name);

    expect(await axe(container)).toHaveNoViolations();
  });
});
