// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { fireEvent, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { Route, Routes } from 'react-router-dom';
import { afterEach, describe, expect, it } from 'vitest';
import type { GrpcCode } from '../../api/errors';
import type { RpcName } from '../../api/routes';
import type { CurrentUser } from '../../app/session';
import { ItemTypeSchema } from '../../gen/fmgr/v1/item_type_pb';
import { TimestampSchema } from '../../gen/fmgr/v1/common/types_pb';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { renderWithProviders } from '../../test/render';
import { server } from '../../test/server';
import { currentUserWith } from '../../test/session';
import { axe } from '../../test/setup';
import { ToastProvider } from '../../ui';
import { ItemTypesScreen } from './ItemTypesScreen';

/**
 * The item-type and custom-field admin screen (TODO.md G3.9, N5).
 *
 * The four assertions worth reading twice:
 *
 *  - **A cycle cannot be created by dragging, and the server refuses it if it
 *    is attempted anyway.** Both are here: the drop into a node's own subtree
 *    never reaches the network, and a *stale* client tree — the case the drag
 *    guard cannot see — gets the server's refusal and a reload.
 *  - **A child may tighten a parent's field but must not drop a required one.**
 *    Both directions, through the form: unchecking `Required` on an inherited
 *    required field is refused with a sentence, checking it on an optional one
 *    saves.
 *  - **`is_phi` + `indexed` is refused with the reason**, not a bare rejection.
 *  - **`is_phi` is not offered at all when the lab has PHI mode off.**
 */

const LAB_ID = 'lab-demo';

/** `item_type.define` + `custom_field.define`, in a lab with PHI mode off. */
const ADMIN: CurrentUser = currentUserWith(
  ['sample.read', 'item_type.define', 'custom_field.define'],
  { isPhiEnabled: false },
);

/** The same user in a lab whose PHI mode is on (all four tests below need it). */
const PHI_ADMIN: CurrentUser = currentUserWith(
  ['sample.read', 'item_type.define', 'custom_field.define'],
  { isPhiEnabled: true },
);

/** `item_type.define` only: the tree is editable, the field catalogue is not. */
const TREE_ONLY: CurrentUser = currentUserWith(['sample.read', 'item_type.define']);

/**
 * The demo lab plus one child of `it-tissue`, whose `tissue_grade` is required.
 * That child is the only way to exercise "a child must not drop a required
 * parent field" without moving the rows the other feature tests assert on.
 */
function itemTypesDemo(): DemoLab {
  const lab = createDemoLab();
  lab.itemTypes.push(
    create(ItemTypeSchema, {
      id: 'it-ffpe',
      labId: LAB_ID,
      parentId: 'it-tissue',
      name: 'FFPE block',
      createdAt: create(TimestampSchema, { unixMicros: 1_758_931_200_000_000n }),
    }),
  );
  return lab;
}

function renderScreen(
  options: {
    readonly user?: CurrentUser;
    readonly demo?: DemoLab;
    readonly fail?: Partial<Record<RpcName, GrpcCode>>;
  } = {},
) {
  const lab = options.demo ?? itemTypesDemo();
  server.use(...fakeApi({ lab, ...(options.fail ? { fail: options.fail } : {}) }));

  const result = renderWithProviders(
    <ToastProvider defaultDuration={0}>
      <Routes>
        <Route path="/labs/:labId/admin/item-types" element={<ItemTypesScreen />} />
      </Routes>
    </ToastProvider>,
    { route: `/labs/${LAB_ID}/admin/item-types`, user: options.user ?? ADMIN },
  );
  return { ...result, lab };
}

/** The draggable row of one node. */
function nodeRow(id: string): HTMLElement {
  const found = document.querySelector<HTMLElement>(`[data-node-id="${id}"]`);
  if (found === null) {
    throw new Error(`the tree has no row for ${id}`);
  }
  return found;
}

/** A `dataTransfer` stand-in: jsdom implements no `DataTransfer` at all. */
function dragData(): { dataTransfer: Record<string, unknown> } {
  return { dataTransfer: { setData: () => undefined, effectAllowed: '', types: [] } };
}

function drag(from: string, to: string): void {
  fireEvent.dragStart(nodeRow(from), dragData());
  fireEvent.dragOver(nodeRow(to), dragData());
  fireEvent.drop(nodeRow(to), dragData());
}

/** The request bodies MSW saw, per route. */
const bodies = new Map<string, Promise<Record<string, unknown>>[]>();

function capture(route: string): void {
  const list = bodies.get(route) ?? [];
  bodies.set(route, list);
  server.events.on('request:start', ({ request }) => {
    if (new URL(request.url).pathname === route) {
      list.push(request.clone().json() as Promise<Record<string, unknown>>);
    }
  });
}

async function seen(route: string): Promise<Record<string, unknown>[]> {
  return Promise.all(bodies.get(route) ?? []);
}

/** Selects a node in the tree and waits for its detail pane. */
async function selectNode(name: string): Promise<HTMLElement> {
  await userEvent.click(await screen.findByRole('button', { name }));
  return screen.findByRole('heading', { level: 2, name });
}

/** The section of the detail pane with this heading. */
function section(title: string): HTMLElement {
  const heading = screen.getByRole('heading', { level: 3, name: title });
  const found = heading.parentElement;
  if (found === null) {
    throw new Error(`the "${title}" section has no container`);
  }
  return found;
}

/** The row of one field, by its label, in one of the two field lists. */
function fieldRow(sectionTitle: string, label: string): HTMLElement {
  const row = within(section(sectionTitle)).getByText(label).closest('li');
  if (row === null) {
    throw new Error(`no field row for ${label}`);
  }
  return row;
}

afterEach(() => {
  bodies.clear();
  server.events.removeAllListeners();
});

describe('ItemTypesScreen — the taxonomy', () => {
  it('renders the lab tree and opens the first root', async () => {
    renderScreen();

    expect(
      await screen.findByRole('heading', { level: 1, name: 'Item types and custom fields' }),
    ).toBeInTheDocument();
    expect(await screen.findByRole('button', { name: 'Blood' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Serum' })).toBeInTheDocument();
    // The first root is open, so the pane is never blank on arrival.
    expect(await screen.findByRole('heading', { level: 2, name: 'Blood' })).toBeInTheDocument();
  });

  it('shows a node\u2019s inherited fields read-only, with where each comes from', async () => {
    renderScreen();
    await selectNode('Serum');

    const inherited = section('Inherited from ancestors');
    expect(within(inherited).getByText('Aliquot count')).toBeInTheDocument();
    expect(within(inherited).getByText('Storage note')).toBeInTheDocument();
    expect(within(inherited).getAllByText(/from Blood/).length).toBeGreaterThan(0);

    // Read-only: the row renders the definition, never an input for it.
    expect(
      within(fieldRow('Inherited from ancestors', 'Aliquot count')).queryAllByRole('textbox'),
    ).toEqual([]);
  });

  it('lists a node\u2019s own definitions separately, with what they tighten', async () => {
    renderScreen();
    await selectNode('Serum');

    const own = section('Defined here');
    expect(within(own).getByText('Serum notes')).toBeInTheDocument();
    // `cfd-notes-serum` narrows Blood's `max_length: 20` to 5 — the seeded,
    // legitimate tightening, which must not read as a violation.
    expect(within(own).getByText(/max length 5/i)).toBeInTheDocument();
    expect(within(own).getByText(/Blood/)).toBeInTheDocument();
  });
});

describe('ItemTypesScreen — cycle-safe re-parenting', () => {
  it('refuses a drop into the node\u2019s own subtree without calling the server', async () => {
    renderScreen();
    capture('/api/v1/item-type/update');
    await screen.findByRole('button', { name: 'Blood' });

    drag('it-blood', 'it-serum');

    expect(await screen.findByText(/cannot be moved into its own subtree/i)).toBeInTheDocument();
    expect(await seen('/api/v1/item-type/update')).toEqual([]);
  });

  it('re-parents by drag when the move is legal', async () => {
    const { lab } = renderScreen();
    capture('/api/v1/item-type/update');
    await screen.findByRole('button', { name: 'Plasma' });

    drag('it-plasma', 'it-serum');

    await waitFor(async () => {
      expect(await seen('/api/v1/item-type/update')).toHaveLength(1);
    });
    const body = (await seen('/api/v1/item-type/update')).at(0);
    expect(body?.item_type).toMatchObject({ id: 'it-plasma', parent_id: 'it-serum' });
    expect(lab.itemTypes.find((type) => type.id === 'it-plasma')?.parentId).toBe('it-serum');
  });

  it('surfaces the server\u2019s refusal when a concurrent change made the drop a cycle', async () => {
    // The drag guard reads the tree the screen loaded. Another admin re-parents
    // `it-tissue` under `it-serum` *after* that load, so the client's own guard
    // cannot see the cycle — this is exactly the half of the acceptance
    // criterion the client cannot implement, and the server must catch it.
    const { lab } = renderScreen();
    await screen.findByRole('button', { name: 'Serum' });
    const tissue = lab.itemTypes.find((type) => type.id === 'it-tissue');
    if (tissue === undefined) {
      throw new Error('the demo lab lost it-tissue');
    }
    tissue.parentId = 'it-serum';

    drag('it-serum', 'it-tissue');

    expect(await screen.findByText(/would create a cycle/i)).toBeInTheDocument();
    // The refused move is not silently kept: the tree is reloaded to the truth,
    // which now nests Tissue under Serum.
    expect(await screen.findByRole('button', { name: 'Collapse Serum' })).toBeInTheDocument();
  });

  it('re-parents through the move dialog as well as by dragging', async () => {
    const { lab } = renderScreen();
    capture('/api/v1/item-type/update');
    await screen.findByRole('button', { name: 'Plasma' });

    await userEvent.click(screen.getByRole('button', { name: 'Move Plasma\u2026' }));
    const dialog = await screen.findByRole('dialog', { name: 'Move Plasma' });
    await userEvent.selectOptions(within(dialog).getByLabelText('Parent item type'), 'it-serum');
    await userEvent.click(within(dialog).getByRole('button', { name: 'Move' }));

    await waitFor(async () => {
      expect(await seen('/api/v1/item-type/update')).toHaveLength(1);
    });
    expect(lab.itemTypes.find((type) => type.id === 'it-plasma')?.parentId).toBe('it-serum');
  });

  it('offers no parent that would close a cycle', async () => {
    renderScreen();
    await screen.findByRole('button', { name: 'Blood' });

    await userEvent.click(screen.getByRole('button', { name: 'Move Blood\u2026' }));
    const dialog = await screen.findByRole('dialog', { name: 'Move Blood' });

    const options = within(dialog)
      .getAllByRole('option')
      .map((option) => option.textContent);
    expect(options).toContain('No parent (root)');
    expect(options).not.toContain('Serum');
    expect(options).not.toContain('Tissue');
  });
});

describe('ItemTypesScreen — field definitions', () => {
  it('refuses to drop a required field inherited from an ancestor', async () => {
    renderScreen();
    capture('/api/v1/custom-field-def/create');
    capture('/api/v1/custom-field-def/update');
    await selectNode('FFPE block');

    await userEvent.click(
      within(fieldRow('Inherited from ancestors', 'Tissue grade')).getByRole('button', {
        name: 'Override Tissue grade',
      }),
    );
    const dialog = await screen.findByRole('dialog', { name: /Tissue grade/ });
    // The key and the data type belong to the inherited definition: a child may
    // narrow it, not redefine it.
    expect(within(dialog).getByLabelText('Key')).toBeDisabled();
    expect(within(dialog).getByLabelText('Data type')).toBeDisabled();

    await userEvent.click(within(dialog).getByLabelText('Required'));

    expect(within(dialog).getByText(/Tissue grade \(Tissue\) is required/i)).toBeInTheDocument();
    expect(within(dialog).getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(await seen('/api/v1/custom-field-def/create')).toEqual([]);
    expect(await seen('/api/v1/custom-field-def/update')).toEqual([]);
  });

  it('allows a child to make an optional inherited field required', async () => {
    // The permissive direction, and the one a "required must match" rule would
    // wrongly refuse.
    const { lab } = renderScreen();
    capture('/api/v1/custom-field-def/create');
    await selectNode('Serum');

    await userEvent.click(
      within(fieldRow('Inherited from ancestors', 'Storage note')).getByRole('button', {
        name: 'Override Storage note',
      }),
    );
    const dialog = await screen.findByRole('dialog', { name: /Storage note/ });
    await userEvent.click(within(dialog).getByLabelText('Required'));
    await userEvent.click(within(dialog).getByRole('button', { name: 'Save' }));

    await waitFor(async () => {
      expect(await seen('/api/v1/custom-field-def/create')).toHaveLength(1);
    });
    expect(lab.customFieldDefs.some((cfd) => cfd.key === 'storage_note' && cfd.required)).toBe(
      true,
    );
  });

  it('allows narrowing an inherited constraint further, and saves it', async () => {
    // The permissive direction for validation: `cfd-notes-serum` already
    // narrows Blood's `max_length: 20` to 5, and 3 is a tightening of that.
    const { lab } = renderScreen();
    capture('/api/v1/custom-field-def/update');
    await selectNode('Serum');

    await userEvent.click(
      within(fieldRow('Defined here', 'Serum notes')).getByRole('button', {
        name: 'Edit Serum notes',
      }),
    );
    const dialog = await screen.findByRole('dialog', { name: 'Edit Serum notes' });
    expect(within(dialog).queryByRole('alert')).not.toBeInTheDocument();
    await userEvent.clear(within(dialog).getByLabelText('Maximum length'));
    await userEvent.type(within(dialog).getByLabelText('Maximum length'), '3');
    await userEvent.click(within(dialog).getByRole('button', { name: 'Save' }));

    await waitFor(async () => {
      expect(await seen('/api/v1/custom-field-def/update')).toHaveLength(1);
    });
    const body = (await seen('/api/v1/custom-field-def/update')).at(0);
    expect(body?.cfd).toMatchObject({ id: 'cfd-notes-serum', validation_json: '{"max_length":3}' });
    expect(lab.customFieldDefs.find((cfd) => cfd.id === 'cfd-notes-serum')?.validationJson).toBe(
      '{"max_length":3}',
    );
  });

  it('refuses to widen an inherited constraint, and says which one', async () => {
    renderScreen();
    capture('/api/v1/custom-field-def/update');
    await selectNode('Serum');

    await userEvent.click(
      within(fieldRow('Defined here', 'Serum notes')).getByRole('button', {
        name: 'Edit Serum notes',
      }),
    );
    const dialog = await screen.findByRole('dialog', { name: 'Edit Serum notes' });
    await userEvent.clear(within(dialog).getByLabelText('Maximum length'));
    await userEvent.type(within(dialog).getByLabelText('Maximum length'), '50');

    expect(within(dialog).getByText(/limits this field to max length 20/i)).toBeInTheDocument();
    expect(within(dialog).getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(await seen('/api/v1/custom-field-def/update')).toEqual([]);
  });

  it('refuses is_phi together with indexed and says why', async () => {
    renderScreen({ user: PHI_ADMIN });
    capture('/api/v1/custom-field-def/create');
    await selectNode('Serum');

    await userEvent.click(screen.getByRole('button', { name: 'Add field' }));
    const dialog = await screen.findByRole('dialog', { name: 'Add field' });
    await userEvent.type(within(dialog).getByLabelText('Key'), 'patient_id');
    await userEvent.type(within(dialog).getByLabelText('Label'), 'Patient id');
    await userEvent.click(within(dialog).getByLabelText('PHI field'));
    await userEvent.click(within(dialog).getByLabelText('Indexed'));

    // The message has to say *why*: an index stores the value outside the
    // encryption layer (L10), which is the whole reason for the rule.
    expect(within(dialog).getByText(/outside the encryption layer/i)).toBeInTheDocument();
    expect(within(dialog).getByRole('button', { name: 'Save' })).toBeDisabled();
    expect(await seen('/api/v1/custom-field-def/create')).toEqual([]);
  });

  it('offers is_phi only when the lab has PHI mode on', async () => {
    renderScreen({ user: ADMIN });
    await selectNode('Serum');

    await userEvent.click(screen.getByRole('button', { name: 'Add field' }));
    const dialog = await screen.findByRole('dialog', { name: 'Add field' });

    expect(within(dialog).queryByLabelText('PHI field')).not.toBeInTheDocument();
    expect(within(dialog).getByLabelText('Indexed')).toBeInTheDocument();
  });

  it('creates a definition with the constraints its data type accepts', async () => {
    const { lab } = renderScreen();
    capture('/api/v1/custom-field-def/create');
    await selectNode('Serum');

    await userEvent.click(screen.getByRole('button', { name: 'Add field' }));
    const dialog = await screen.findByRole('dialog', { name: 'Add field' });
    await userEvent.type(within(dialog).getByLabelText('Key'), 'storage_temp');
    await userEvent.type(within(dialog).getByLabelText('Label'), 'Storage temperature');
    await userEvent.selectOptions(
      within(dialog).getByLabelText('Data type'),
      'FIELD_DATA_TYPE_INT',
    );
    await userEvent.type(within(dialog).getByLabelText('Minimum'), '-80');
    await userEvent.type(within(dialog).getByLabelText('Maximum'), '-20');
    await userEvent.click(within(dialog).getByRole('button', { name: 'Save' }));

    await waitFor(async () => {
      expect(await seen('/api/v1/custom-field-def/create')).toHaveLength(1);
    });
    const body = (await seen('/api/v1/custom-field-def/create')).at(0);
    // proto3 JSON omits default-valued fields, so `required`/`indexed`/`is_phi`
    // are absent here unless they are true — the fake fills the defaults in.
    expect(body?.cfd).toMatchObject({
      lab_id: LAB_ID,
      item_type_id: 'it-serum',
      key: 'storage_temp',
      label: 'Storage temperature',
      data_type: 'FIELD_DATA_TYPE_INT',
      validation_json: '{"min":-80,"max":-20}',
    });
    expect(lab.customFieldDefs.some((cfd) => cfd.key === 'storage_temp')).toBe(true);
  });

  it('explains a duplicate field key instead of showing a raw status', async () => {
    // `cfd_lab_scope_type_key_unique` refuses a second definition of one key on
    // one node — the server's answer, because the form has nothing to compare
    // against: `concentration` has no ancestor definition to tighten.
    renderScreen();
    await selectNode('Serum');

    await userEvent.click(screen.getByRole('button', { name: 'Add field' }));
    const dialog = await screen.findByRole('dialog', { name: 'Add field' });
    await userEvent.type(within(dialog).getByLabelText('Key'), 'concentration');
    await userEvent.type(within(dialog).getByLabelText('Label'), 'Concentration again');
    await userEvent.click(within(dialog).getByRole('button', { name: 'Save' }));

    expect(await screen.findByText(/already exists on this item type/i)).toBeInTheDocument();
  });

  it('does not offer field edits without custom_field.define', async () => {
    renderScreen({ user: TREE_ONLY });
    await selectNode('Serum');

    expect(
      within(section('Inherited from ancestors')).queryByRole('button', {
        name: /^Override /,
      }),
    ).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Add field' })).not.toBeInTheDocument();
  });
});

describe('ItemTypesScreen — failures', () => {
  it('shows a retryable error when the item-type list is refused', async () => {
    renderScreen({ fail: { 'item-type/list': 'PERMISSION_DENIED' } });

    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent(/permission/i);
    expect(within(alert).getByRole('button', { name: /try again/i })).toBeInTheDocument();
  });

  it('shows a retryable error when the list cannot be reached', async () => {
    renderScreen({ fail: { 'item-type/list': 'UNAVAILABLE' } });

    expect(await screen.findByRole('alert')).toHaveTextContent(/could not be reached/i);
  });

  it('still renders the tree when the field definitions cannot be read', async () => {
    // The two reads have different permissions (#69): the tree is the screen,
    // the field catalogue is a second request that can fail on its own.
    renderScreen({ fail: { 'custom-field-def/list': 'PERMISSION_DENIED' } });

    expect(await screen.findByRole('button', { name: 'Serum' })).toBeInTheDocument();
    await selectNode('Serum');
    expect(screen.getByText(/could not load the field definitions/i)).toBeInTheDocument();
  });

  it('explains a duplicate item-type name instead of showing a raw status', async () => {
    renderScreen();
    await screen.findByRole('button', { name: 'Blood' });

    await userEvent.click(screen.getByRole('button', { name: 'New item type' }));
    const dialog = await screen.findByRole('dialog', { name: 'New item type' });
    await userEvent.type(within(dialog).getByLabelText('Name'), 'Blood');
    await userEvent.click(within(dialog).getByRole('button', { name: 'Create' }));

    expect(await screen.findByText(/already exists in this lab/i)).toBeInTheDocument();
  });

  it('has no accessibility violations', async () => {
    const { container } = renderScreen();
    await selectNode('Serum');

    expect(await axe(container)).toHaveNoViolations();
  });
});
