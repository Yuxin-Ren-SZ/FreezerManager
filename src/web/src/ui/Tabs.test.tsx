// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { axe } from '../test/setup';
import { Tabs } from './Tabs';

const items = [
  { id: 'details', label: 'Details', content: <p>Details panel</p> },
  { id: 'history', label: 'History', content: <p>History panel</p> },
  { id: 'shares', label: 'Shares', content: <p>Shares panel</p>, disabled: true },
];

describe('Tabs', () => {
  it('renders one tab per item and shows only the selected panel', () => {
    render(<Tabs label="Sample sections" items={items} />);

    expect(screen.getAllByRole('tab')).toHaveLength(3);
    expect(screen.getByRole('tabpanel')).toHaveTextContent('Details panel');
    expect(screen.queryByText('History panel')).not.toBeInTheDocument();
  });

  it('selects the tab named by defaultValue', () => {
    render(<Tabs label="Sample sections" items={items} defaultValue="history" />);

    expect(screen.getByRole('tabpanel')).toHaveTextContent('History panel');
  });

  it('marks the selected tab for assistive technology and keeps the rest reachable by arrow keys', async () => {
    render(<Tabs label="Sample sections" items={items} />);

    const [details, history] = screen.getAllByRole('tab');
    expect(details).toHaveAttribute('aria-selected', 'true');
    // Roving tabindex: only the selected tab is in the page tab order.
    expect(details).toHaveAttribute('tabindex', '0');
    expect(history).toHaveAttribute('tabindex', '-1');

    await userEvent.tab();
    expect(details).toHaveFocus();
    await userEvent.keyboard('{ArrowRight}');

    expect(screen.getByRole('tab', { name: 'History' })).toHaveFocus();
    expect(screen.getByRole('tab', { name: 'History' })).toHaveAttribute('aria-selected', 'true');
    expect(screen.getByRole('tabpanel')).toHaveTextContent('History panel');
  });

  it('wraps around with ArrowLeft and ArrowRight', async () => {
    render(<Tabs label="Sample sections" items={items} />);

    await userEvent.tab();
    await userEvent.keyboard('{ArrowLeft}');

    // "Shares" is disabled, so the wrap lands on the last enabled tab.
    expect(screen.getByRole('tab', { name: 'History' })).toHaveFocus();
  });

  it('jumps to the first and last enabled tab with Home and End', async () => {
    render(<Tabs label="Sample sections" items={items} />);

    await userEvent.tab();
    await userEvent.keyboard('{End}');
    expect(screen.getByRole('tab', { name: 'History' })).toHaveFocus();

    await userEvent.keyboard('{Home}');
    expect(screen.getByRole('tab', { name: 'Details' })).toHaveFocus();
  });

  it('skips disabled tabs when arrowing', async () => {
    render(<Tabs label="Sample sections" items={items} />);

    await userEvent.tab();
    await userEvent.keyboard('{ArrowRight}{ArrowRight}');

    // Details -> History -> (Shares is skipped) -> Details
    expect(screen.getByRole('tab', { name: 'Details' })).toHaveFocus();
  });

  it('reports selection changes to its caller', async () => {
    const onValueChange = vi.fn();
    render(<Tabs label="Sample sections" items={items} onValueChange={onValueChange} />);

    await userEvent.click(screen.getByRole('tab', { name: 'History' }));

    expect(onValueChange).toHaveBeenCalledWith('history');
  });

  it('does not select a disabled tab when it is clicked', async () => {
    render(<Tabs label="Sample sections" items={items} />);

    await userEvent.click(screen.getByRole('tab', { name: 'Shares' }));

    expect(screen.getByRole('tab', { name: 'Shares' })).toHaveAttribute('aria-selected', 'false');
    expect(screen.getByRole('tabpanel')).toHaveTextContent('Details panel');
  });

  it('has no accessibility violations', async () => {
    const { container } = render(
      <main>
        <Tabs label="Sample sections" items={items} />
      </main>,
    );

    expect(await axe(container)).toHaveNoViolations();
  });
});
