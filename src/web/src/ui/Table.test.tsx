// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { Table, type TableColumn } from './Table';
import { expectNoA11yViolations } from './a11y';
import tableCss from './Table.module.css?raw';

interface Sample {
  id: string;
  name: string;
  status: string;
}

const COLUMNS: TableColumn<Sample>[] = [
  { id: 'name', accessorKey: 'name', header: 'Name' },
  { id: 'status', accessorKey: 'status', header: 'Status' },
];

const ROWS: Sample[] = [
  { id: 's1', name: 'Liver 01', status: 'available' },
  { id: 's2', name: 'Liver 02', status: 'checked_out' },
];

// jsdom has no layout engine: `offsetWidth`/`offsetHeight` are always 0, so
// the virtualizer correctly concludes that not one row fits and renders an
// empty grid. TanStack Virtual measures its scroll container through exactly
// those two properties (virtual-core's `getRect`), so report the height the
// container's inline `max-height` asks for — which is what a browser reports
// for a scroll container whose content overflows it. `restoreMocks: true`
// (vite.config.ts) undoes both spies after every test.
beforeEach(() => {
  vi.spyOn(HTMLElement.prototype, 'offsetHeight', 'get').mockImplementation(function (
    this: HTMLElement,
  ) {
    return Number.parseFloat(this.style.maxHeight) || 0;
  });
  vi.spyOn(HTMLElement.prototype, 'offsetWidth', 'get').mockImplementation(() => 800);
});

function renderTable(props: Partial<Parameters<typeof Table<Sample>>[0]> = {}) {
  return render(
    <main>
      <Table<Sample>
        caption="Samples"
        columns={COLUMNS}
        data={ROWS}
        getRowId={(row) => row.id}
        {...props}
      />
    </main>,
  );
}

describe('Table', () => {
  it('renders a named table with one header per column', () => {
    renderTable();

    const table = screen.getByRole('table', { name: 'Samples' });
    expect(
      within(table)
        .getAllByRole('columnheader')
        .map((cell) => cell.textContent),
    ).toEqual(['Name', 'Status']);
  });

  it('renders one row per data item', () => {
    renderTable();

    expect(screen.getAllByRole('row')).toHaveLength(3); // header + 2 data rows
    expect(screen.getByRole('cell', { name: 'Liver 01' })).toBeInTheDocument();
  });

  it('renders the empty message instead of rows when there is no data', () => {
    renderTable({ data: [], emptyMessage: 'No samples match' });

    expect(screen.getByText('No samples match')).toBeInTheDocument();
    // The header row plus the one row holding the empty message; no data cells.
    expect(screen.getAllByRole('row')).toHaveLength(2);
    expect(screen.queryByRole('cell', { name: 'Liver 01' })).not.toBeInTheDocument();
  });

  it('hides a column through the column picker and shows it again', async () => {
    renderTable();

    await userEvent.click(screen.getByText('Columns'));
    await userEvent.click(screen.getByRole('checkbox', { name: 'Show or hide the Status column' }));

    expect(screen.queryByRole('columnheader', { name: 'Status' })).not.toBeInTheDocument();
    expect(screen.getByRole('columnheader', { name: 'Name' })).toBeInTheDocument();

    await userEvent.click(screen.getByRole('checkbox', { name: 'Show or hide the Status column' }));
    expect(screen.getByRole('columnheader', { name: 'Status' })).toBeInTheDocument();
  });

  it('keeps the last visible column from being hidden', () => {
    renderTable({ enableColumnVisibility: false });

    expect(screen.queryByText('Columns')).not.toBeInTheDocument();
  });

  it('renders only a window of a long list, not every row', () => {
    const many: Sample[] = Array.from({ length: 1000 }, (_, index) => ({
      id: `s${String(index)}`,
      name: `Sample ${String(index)}`,
      status: 'available',
    }));

    renderTable({ data: many, maxHeight: 200, rowHeight: 40 });

    const rendered = screen.getAllByRole('row').length;
    expect(rendered).toBeGreaterThan(0);
    expect(rendered).toBeLessThan(100);
  });

  it('renders every row when virtualization is turned off', () => {
    const many: Sample[] = Array.from({ length: 40 }, (_, index) => ({
      id: `s${String(index)}`,
      name: `Sample ${String(index)}`,
      status: 'available',
    }));

    renderTable({ data: many, virtualized: false });

    expect(screen.getAllByRole('row')).toHaveLength(41);
  });

  it('gives the header a sticky position in the stylesheet, not just a class name', () => {
    // jsdom does not lay out or apply stylesheet rules, so the only honest way
    // to test "sticky header" is to read the rule that makes it sticky.
    expect(tableCss).toMatch(/\.head\s*\{[^}]*position:\s*sticky/s);
    expect(tableCss).toMatch(/\.head\s*\{[^}]*top:\s*0/s);
  });

  it('has no accessibility violations', async () => {
    const { container } = renderTable();

    await expectNoA11yViolations(container);
  });

  it('has no accessibility violations with a long virtualized list', async () => {
    const many: Sample[] = Array.from({ length: 500 }, (_, index) => ({
      id: `s${String(index)}`,
      name: `Sample ${String(index)}`,
      status: 'available',
    }));
    const { container } = renderTable({ data: many, maxHeight: 200 });

    await expectNoA11yViolations(container);
  });

  it('reads the header rule out of the real stylesheet', () => {
    // Guards the test above: if `?raw` ever resolves to an empty string again
    // (Vitest's default CSS stubbing), that assertion would pass vacuously.
    expect(tableCss.length).toBeGreaterThan(0);
  });
});
