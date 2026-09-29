// SPDX-License-Identifier: AGPL-3.0-or-later
import { create, fromJson, toJson, type JsonValue } from '@bufbuild/protobuf';
import { fireEvent, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { HttpResponse, http } from 'msw';
import { Route, Routes } from 'react-router-dom';
import { describe, expect, it } from 'vitest';
import sampleDetailCopy from '../../../locales/en/sample-detail.json';
import { apiRoutes } from '../../api/routes';
import { SessionProvider } from '../../app/session';
import {
  CustomFieldDefinitionSchema,
  FieldDataType,
  ScopeKind,
} from '../../gen/fmgr/v1/item_type_pb';
import { SampleStatus } from '../../gen/fmgr/v1/sample_pb';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { renderWithProviders } from '../../test/render';
import { server } from '../../test/server';
import { allPermissionsUser, currentUserWith } from '../../test/session';
import { axe } from '../../test/setup';
import { SampleCreateScreen } from './SampleCreateScreen';
import { SampleDetailScreen } from './SampleDetailScreen';

/**
 * The generated create form (TODO.md G3.3).
 *
 * The form is not a fixed list of inputs: it is produced from the item type's
 * **inherited** field definitions, so what it renders depends on the item type
 * selected. Every test here picks one first and then asserts on what appeared —
 * including the two mistakes that look correct against an item type with no
 * parent: reading only the leaf, and reading a sibling's fields.
 *
 * Date and datetime inputs are driven with `fireEvent.change` rather than
 * `userEvent.type`: jsdom's date inputs accept a whole value or nothing, which
 * is a property of the test environment and not of the component.
 */

const LAB_ID = 'lab-demo';

interface RenderOptions {
  readonly demo?: DemoLab;
  readonly user?: ReturnType<typeof currentUserWith>;
}

function renderCreate(options: RenderOptions = {}) {
  const demo = options.demo ?? createDemoLab();
  server.use(...fakeApi({ lab: demo }));

  const user = options.user ?? allPermissionsUser();

  return {
    ...renderWithProviders(
      <SessionProvider loadSession={() => Promise.resolve(user)}>
        <Routes>
          <Route path="/labs/:labId/samples/new" element={<SampleCreateScreen />} />
          <Route path="/labs/:labId/samples/:sampleId" element={<p>detail screen</p>} />
        </Routes>
      </SessionProvider>,
      { route: `/labs/${LAB_ID}/samples/new` },
    ),
    demo,
  };
}

const escapeRegExp = (value: string) => value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

/**
 * `getByLabelText`, tolerant of the required marker the kit appends inside the
 * label. The marker is `aria-hidden`, so a screen reader reads "Name" — but the
 * label's text content is "Name*", which an exact query would miss.
 */
const byLabel = (label: string): HTMLElement =>
  screen.getByLabelText(new RegExp(`^${escapeRegExp(label)}\\*?$`));

const findByLabel = (label: string): Promise<HTMLElement> =>
  screen.findByLabelText(new RegExp(`^${escapeRegExp(label)}\\*?$`));

/** Pick the item type the form should generate itself from. */
async function chooseItemType(id: string) {
  await userEvent.selectOptions(await findByLabel(sampleDetailCopy.form.itemType), id);
}

const setDate = (label: string, value: string) => {
  fireEvent.change(byLabel(label), { target: { value } });
};

const submit = async () => {
  await userEvent.click(screen.getByRole('button', { name: sampleDetailCopy.form.submitCreate }));
};

describe('SampleCreateScreen', () => {
  it('shows the core fields of a sample', async () => {
    renderCreate();

    expect(
      await screen.findByRole('heading', { level: 1, name: sampleDetailCopy.form.createTitle }),
    ).toBeInTheDocument();
    for (const label of [
      sampleDetailCopy.form.name,
      sampleDetailCopy.form.barcode,
      sampleDetailCopy.form.itemType,
      sampleDetailCopy.form.containerType,
      sampleDetailCopy.form.box,
      sampleDetailCopy.form.position,
      sampleDetailCopy.form.volumeValue,
      sampleDetailCopy.form.volumeUnit,
      sampleDetailCopy.form.massValue,
      sampleDetailCopy.form.massUnit,
      sampleDetailCopy.form.parentSample,
    ]) {
      expect(byLabel(label), label).toBeInTheDocument();
    }
  });

  it('lists the lab item types and nothing from another lab', async () => {
    renderCreate();

    const select = await findByLabel(sampleDetailCopy.form.itemType);
    const values = within(select)
      .getAllByRole('option')
      .map((option) => option.getAttribute('value'));

    expect(values).toContain('it-serum');
    expect(values).not.toContain('it-dna');
  });

  it('generates a control per custom-field data type', async () => {
    // Serum inherits from Blood, so one selection covers every seeded type:
    // text, int, float, bool, date, datetime, enum and reference.
    renderCreate();
    await chooseItemType('it-serum');

    expect(byLabel('Serum notes')).toHaveAttribute('type', 'text');

    const count = byLabel('Aliquot count');
    expect(count).toHaveAttribute('type', 'number');
    expect(count).toHaveAttribute('step', '1');

    expect(byLabel('Concentration')).toHaveAttribute('type', 'number');
    expect(byLabel('Hemolyzed')).toHaveAttribute('type', 'checkbox');
    expect(byLabel('Collection date')).toHaveAttribute('type', 'date');
    expect(byLabel('Received at')).toHaveAttribute('type', 'datetime-local');

    const tube = byLabel('Tube type');
    expect(tube.tagName).toBe('SELECT');
    // The enum's options are the definition's `validation_json.values`, so a
    // value outside the set cannot be typed at all.
    expect(
      within(tube)
        .getAllByRole('option')
        .map((option) => option.textContent),
    ).toEqual([sampleDetailCopy.form.noChoice, 'EDTA', 'heparin', 'plain']);

    expect(byLabel('Parent aliquot')).toHaveAttribute('type', 'text');
  });

  it('generates the fields inherited from the parent item type, not just the leaf', async () => {
    renderCreate();
    await chooseItemType('it-serum');

    // `aliquot_count`, `collection_date`, `received_at`, `tube_type` and
    // `notes` are defined on Blood; `hemolyzed` and `parent_aliquot` on Serum.
    // An item type with no parent would hide this bug completely.
    expect(byLabel('Aliquot count')).toBeInTheDocument();
    expect(byLabel('Collection date')).toBeInTheDocument();
    expect(byLabel('Received at')).toBeInTheDocument();
    expect(byLabel('Tube type')).toBeInTheDocument();
    expect(byLabel('Hemolyzed')).toBeInTheDocument();
  });

  it('lets the leaf item type override a key its parent also defines', async () => {
    renderCreate();
    await chooseItemType('it-serum');

    // Both Blood and Serum define `notes`; the label names the definition that
    // won, and the max length came with it (asserted below).
    expect(byLabel('Serum notes')).toBeInTheDocument();
    expect(screen.queryByLabelText('Notes')).not.toBeInTheDocument();
  });

  it('does not show a sibling item type\u2019s fields', async () => {
    renderCreate();
    await chooseItemType('it-serum');

    expect(screen.queryByLabelText('Tissue grade')).not.toBeInTheDocument();
    // Nor the other lab's, which a key-only resolver would leak.
    expect(screen.queryByLabelText('Ploidy')).not.toBeInTheDocument();
  });

  it('marks a PHI field as PHI and still shows the control', async () => {
    renderCreate();
    await chooseItemType('it-serum');

    // The kit's `TextField` takes a string label, so the marker is part of the
    // accessible name rather than a separate badge (the detail *view*, which
    // owns its own markup, renders the badge).
    expect(
      screen.getByLabelText(new RegExp(`Donor name.*${sampleDetailCopy.phi.badge}`)),
    ).toBeInTheDocument();
  });

  it('marks a required custom field as required', async () => {
    renderCreate();
    await chooseItemType('it-tissue');

    expect(screen.getByLabelText(/Tissue grade/)).toBeRequired();
  });

  it('sends the custom fields as a JSON blob of wire-typed values', async () => {
    const { demo } = renderCreate();
    await chooseItemType('it-serum');

    await userEvent.type(byLabel('Serum notes'), 'ok');
    await userEvent.type(byLabel('Aliquot count'), '3');
    await userEvent.click(byLabel('Hemolyzed'));
    setDate('Collection date', '2026-01-05');
    setDate('Received at', '2026-01-05T10:30');
    await userEvent.selectOptions(byLabel('Tube type'), 'EDTA');
    await userEvent.type(byLabel(sampleDetailCopy.form.name), 'Serum C');
    await submit();

    await screen.findByText('detail screen');

    const created = demo.samples.at(-1);
    expect(created?.name).toBe('Serum C');
    expect(JSON.parse(created?.customFieldsJson ?? '{}')).toEqual({
      notes: 'ok',
      aliquot_count: 3,
      is_hemolyzed: true,
      collection_date: '2026-01-05',
      // `datetime-local` gives minute precision and the validator wants
      // seconds, so the form widens it rather than sending a rejected value.
      received_at: '2026-01-05T10:30:00',
      tube_type: 'EDTA',
    });
  });

  it('leaves a blank optional field out of the blob instead of sending an empty string', async () => {
    const { demo } = renderCreate();
    await chooseItemType('it-serum');

    await userEvent.type(byLabel(sampleDetailCopy.form.name), 'Serum C');
    await submit();

    await screen.findByText('detail screen');

    // The text, number, date, datetime, enum and reference controls are all
    // blank and none of them is submitted. The checkbox is the exception and
    // deliberately so: an unchecked box is `false`, which is a value, not an
    // absent one.
    expect(JSON.parse(demo.samples.at(-1)?.customFieldsJson ?? '{}')).toEqual({
      is_hemolyzed: false,
    });
  });

  it('writes the custom fields into the item type\u2019s own constraints, not the parent\u2019s', async () => {
    // `notes` is max_length 5 on Serum and 20 on Blood: 6 characters must be
    // rejected, which only happens when the leaf definition won.
    renderCreate();
    await chooseItemType('it-serum');

    await userEvent.type(byLabel('Serum notes'), 'abcdef');
    await userEvent.type(byLabel(sampleDetailCopy.form.name), 'Serum C');
    await submit();

    expect(await screen.findByText(sampleDetailCopy.validation.text.tooLong)).toBeInTheDocument();
    expect(screen.queryByText('detail screen')).not.toBeInTheDocument();
  });

  it('blocks the submit client-side when the mirror finds a violation', async () => {
    renderCreate();
    await chooseItemType('it-serum');

    await userEvent.type(byLabel('Aliquot count'), '99');
    await userEvent.type(byLabel(sampleDetailCopy.form.name), 'Serum C');
    await submit();

    expect(await screen.findByText(sampleDetailCopy.validation.aboveMax)).toBeInTheDocument();
    expect(screen.queryByText('detail screen')).not.toBeInTheDocument();
  });

  it('shows a server rejection on the field it names, even one the form could not render', async () => {
    // `custom-field-def/list` is gated on `sample.read` since #69, so a caller
    // holding `sample.write` without it cannot read the definitions: the form
    // renders no custom-field inputs at all — and `tissue_grade` is required on
    // the server. The rejection has to name the field rather than become a
    // banner that leaves the user hunting.
    const demo = createDemoLab();
    renderCreate({ demo, user: currentUserWith(['sample.write']) });

    expect(
      await screen.findByText(sampleDetailCopy.form.customFieldsUnavailable),
    ).toBeInTheDocument();

    await chooseItemType('it-tissue');
    await userEvent.type(byLabel(sampleDetailCopy.form.name), 'Tissue A');
    await submit();

    const reported = await screen.findByTestId('server-field-errors');
    expect(within(reported).getByText(/tissue_grade/)).toBeInTheDocument();
    expect(within(reported).getByText(sampleDetailCopy.validation.required)).toBeInTheDocument();
    expect(screen.queryByText('detail screen')).not.toBeInTheDocument();
  });

  it('shows an ALREADY_EXISTS position conflict on the position field', async () => {
    // The picker only offers free positions, so the conflict has to be the one
    // it cannot prevent: the slot is taken between the picker being drawn and
    // the save. Draw it with A1 free...
    const demo = createDemoLab();
    const index = demo.samples.findIndex((sample) => sample.id === 'sample-1');
    const [heldElsewhere] = demo.samples.splice(index, 1);
    renderCreate({ demo });
    await chooseItemType('it-plasma');

    await userEvent.selectOptions(await findByLabel(sampleDetailCopy.form.box), 'box-1');
    await userEvent.selectOptions(byLabel(sampleDetailCopy.form.position), 'A1');
    await userEvent.type(byLabel(sampleDetailCopy.form.name), 'Plasma B');

    // ...and put the holder back before submitting.
    server.use(...fakeApi({ lab: createDemoLab() }));

    await submit();

    expect(await screen.findByText(sampleDetailCopy.server.positionTaken)).toBeInTheDocument();
    expect(byLabel(sampleDetailCopy.form.position)).toHaveAttribute('aria-invalid', 'true');
    expect(screen.queryByText('detail screen')).not.toBeInTheDocument();
    expect(heldElsewhere.id).toBe('sample-1');
  });

  it('shows a size-class mismatch on the container type field', async () => {
    renderCreate();
    await chooseItemType('it-plasma');

    // bt-9 (box-3) accepts only size class tube-15; ct-50ml is tube-50.
    await userEvent.selectOptions(
      await findByLabel(sampleDetailCopy.form.containerType),
      'ct-50ml',
    );
    await userEvent.selectOptions(byLabel(sampleDetailCopy.form.box), 'box-3');
    await userEvent.selectOptions(byLabel(sampleDetailCopy.form.position), 'A1');
    await userEvent.type(byLabel(sampleDetailCopy.form.name), 'Plasma B');
    await submit();

    expect(
      await screen.findByText(sampleDetailCopy.server.sizeClassNotAccepted),
    ).toBeInTheDocument();
    expect(byLabel(sampleDetailCopy.form.containerType)).toHaveAttribute('aria-invalid', 'true');
  });

  it('offers only the free positions of the chosen box', async () => {
    renderCreate();

    await userEvent.selectOptions(await findByLabel(sampleDetailCopy.form.box), 'box-1');

    // A1 and A2 are taken by sample-1 and sample-2; A3 is free.
    const labels = within(byLabel(sampleDetailCopy.form.position))
      .getAllByRole('option')
      .map((option) => option.textContent);
    expect(labels).not.toContain('A1');
    expect(labels).not.toContain('A2');
    expect(labels).toContain('A3');
  });

  it('creates an active sample, which is the server\u2019s decision and not the form\u2019s', async () => {
    const { demo } = renderCreate();
    await chooseItemType('it-plasma');

    await userEvent.type(byLabel(sampleDetailCopy.form.name), 'Plasma B');
    await submit();

    await screen.findByText('detail screen');

    expect(demo.samples.at(-1)?.status).toBe(SampleStatus.ACTIVE);
  });

  it('has no accessibility violations', async () => {
    const { container } = renderCreate();
    await chooseItemType('it-serum');

    expect(await axe(container)).toHaveNoViolations();
  });
});

/**
 * Editing a sample whose item type carries PHI fields the caller cannot read.
 *
 * The server withholds **every** PHI value from a caller without `phi.read`
 * (`reveal_phi()` in `SampleServiceImpl.cc`), so the response carries no PHI key
 * at all — while the definitions come from `custom-field-def/list`, which is
 * gated on `sample.read` (#69, not on `phi.read`), so the form still renders a
 * control for each PHI definition. The form is therefore holding fields whose
 * values it has never seen.
 *
 * What it does with them is a question about the **payload**, not about the
 * screen: one PHI key makes the request authoritative for the whole stored
 * envelope server-side (#83), so an invented `false` is not a cosmetic default —
 * it deletes every other PHI key on the sample. A test that only asserted the
 * save succeeded passes against that, which is why this suite missed it.
 */

const PHI_BOOL_KEY = 'donor_screening_flag';
const PHI_BOOL_LABEL = 'Donor screening flag';

/**
 * A caller that may open and edit the sample — which is also what lets the form
 * read the item type's definitions, since `custom-field-def/list` is gated on
 * `sample.read` (#69) and not on `custom_field.define` — but holds no `phi.read`.
 */
const phiBlindEditor = () => currentUserWith(['sample.read', 'sample.write']);

/**
 * The fake seeds Text PHI (`donor_name`) but no Bool PHI, and a Bool is the one
 * field type whose "empty" control is a *value* rather than an absence. Added to
 * Blood so the sample's own type (Serum) inherits it, like `donor_name`.
 */
function withPhiBool(demo: DemoLab): DemoLab {
  demo.customFieldDefs.push(
    create(CustomFieldDefinitionSchema, {
      id: `cfd-${PHI_BOOL_KEY}`,
      labId: LAB_ID,
      scopeKind: ScopeKind.SAMPLE,
      itemTypeId: 'it-blood',
      key: PHI_BOOL_KEY,
      label: PHI_BOOL_LABEL,
      dataType: FieldDataType.BOOL,
      isPhi: true,
    }),
  );
  return demo;
}

function renderEdit(options: RenderOptions = {}) {
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
      { route: `/labs/${LAB_ID}/samples/sample-1` },
    ),
    demo,
  };
}

/**
 * The `sample/update` bodies the form actually put on the wire.
 *
 * Registered *after* the screen has rendered, because MSW matches the
 * most-recently-added handler first — so this one sees the request instead of
 * the fake's resolver, and echoing the request back keeps the save a success
 * (`UpdateSampleRequest` and `UpdateSampleResponse` are both `{ sample }`, so
 * the request body *is* a valid response body). Asserting against a failed save
 * would prove nothing about what a successful one sends.
 */
function captureUpdateBodies(): JsonValue[] {
  const bodies: JsonValue[] = [];
  const route = apiRoutes['sample/update'];

  server.use(
    http.post(route.path, async ({ request }) => {
      const body = (await request.json()) as JsonValue;
      bodies.push(body);
      const echoed = fromJson(route.output, body, { ignoreUnknownFields: false });
      return HttpResponse.json(toJson(route.output, echoed, { useProtoFieldName: true }));
    }),
  );

  return bodies;
}

/** The one custom-field blob of a captured request, parsed. */
function sentCustomFields(bodies: JsonValue[]): Record<string, unknown> {
  const [sent] = bodies;
  expect(sent, 'the form sent no sample/update request').toBeDefined();
  const { sample } = sent as { sample?: { custom_fields_json?: string } };
  return JSON.parse(sample?.custom_fields_json ?? '{}') as Record<string, unknown>;
}

const openEditor = async () => {
  await screen.findByRole('heading', { level: 1, name: 'Serum A' });
  await userEvent.click(screen.getByRole('button', { name: sampleDetailCopy.actions.edit }));
  return findByLabel(sampleDetailCopy.form.name);
};

const save = async () => {
  await userEvent.click(screen.getByRole('button', { name: sampleDetailCopy.form.submitUpdate }));
  await screen.findByText(sampleDetailCopy.form.saved);
};

describe('SampleForm: PHI the caller cannot read', () => {
  it('leaves a withheld Bool PHI field out of the payload rather than sending false', async () => {
    // `sample-1` comes back without a `donor_screening_flag` key, which is what
    // `reveal_phi()` produces for this caller: the stored envelope is real, it
    // is simply not disclosed. A blank control is not a value the user chose.
    renderEdit({ demo: withPhiBool(createDemoLab()), user: phiBlindEditor() });
    const name = await openEditor();
    // The control is **rendered** — that is what makes the payload assertion
    // below meaningful. This caller holds `sample.read`, so the definitions are
    // readable and the form has a PHI control to withhold; a caller without it
    // would render no custom fields at all (`definitionsReadable`), and a
    // "the key is absent" assertion would then hold for the wrong reason.
    expect(screen.getByLabelText(new RegExp(`^${PHI_BOOL_LABEL}`))).toBeInTheDocument();
    const bodies = captureUpdateBodies();

    await userEvent.clear(name);
    await userEvent.type(name, 'Serum A2');
    await save();

    // The whole blob: the fields the caller *could* see are still carried, so
    // the absence is specific to PHI rather than a form that sent nothing.
    expect(sentCustomFields(bodies)).toEqual({
      concentration: 12.5,
      notes: 'ok',
      aliquot_count: 3,
      is_hemolyzed: true,
      collection_date: '2026-01-05',
    });
  });

  it('offers no PHI control it could not fill with the stored value', async () => {
    renderEdit({ demo: withPhiBool(createDemoLab()), user: phiBlindEditor() });
    await openEditor();

    // The value is hidden, not missing, so the control must not accept an edit
    // that `wireValues` would then have to drop on submit.
    expect(screen.getByLabelText(new RegExp(`^${PHI_BOOL_LABEL}`))).toBeDisabled();
    expect(screen.getByText(sampleDetailCopy.form.phiWriteWarning)).toBeInTheDocument();
    // And the warning has to match that behaviour: the old copy claimed a save
    // "clears them", which is not what happens for a field the form never
    // submits. A warning that overstates is one people learn to ignore.
    expect(sampleDetailCopy.form.phiWriteWarning).not.toMatch(/clear/i);
  });

  it('sends an explicit false when a caller that can read PHI unchecks the box', async () => {
    const demo = withPhiBool(createDemoLab());
    // A `phi.read` holder's response has the envelope merged into
    // `custom_fields_json`, so the box renders the stored value.
    const sample = demo.samples.find((candidate) => candidate.id === 'sample-1');
    if (sample === undefined) throw new Error('fixture has no sample-1');
    sample.customFieldsJson = JSON.stringify({
      ...(JSON.parse(sample.customFieldsJson) as Record<string, unknown>),
      [PHI_BOOL_KEY]: true,
    });

    renderEdit({ demo, user: allPermissionsUser() });
    await openEditor();
    const box = await screen.findByLabelText(new RegExp(`^${PHI_BOOL_LABEL}`));
    expect(box).toBeChecked();
    const bodies = captureUpdateBodies();

    await userEvent.click(box);
    await save();

    // `false` is a deliberate unset and is distinguishable in the payload from
    // the withheld field above, which is *absent*.
    expect(sentCustomFields(bodies)).toMatchObject({ [PHI_BOOL_KEY]: false });
  });
});
