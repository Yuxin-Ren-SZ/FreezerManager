// SPDX-License-Identifier: AGPL-3.0-or-later
import { screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { Route, Routes } from 'react-router-dom';
import { describe, expect, it } from 'vitest';
import layoutCopy from '../../../locales/en/layout.json';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { renderWithProviders } from '../../test/render';
import { server } from '../../test/server';
import { axe } from '../../test/setup';
import { LayoutTreeScreen } from './LayoutTreeScreen';

/**
 * The layout screen (TODO.md G3.1): a collapsible tree of the lab's storage,
 * with a count per node, archived nodes hidden, and a box that opens the box
 * view.
 *
 * Two assertions here are about *not* lying to the user: an archived node is
 * absent rather than greyed out, and a failed list is an error state rather
 * than a lab that looks empty.
 */

const LAB_ID = 'lab-demo';

/** The screen, inside the routes it links to, so a click can be observed. */
function renderScreen(options: { labId?: string; demo?: DemoLab; latencyMs?: number } = {}) {
  server.use(...fakeApi({ lab: options.demo ?? createDemoLab(), latencyMs: options.latencyMs }));

  return renderWithProviders(
    <Routes>
      <Route path="/labs/:labId/layout" element={<LayoutTreeScreen />} />
      <Route path="/labs/:labId/boxes/:boxId" element={<p>box view</p>} />
    </Routes>,
    { route: `/labs/${options.labId ?? LAB_ID}/layout` },
  );
}

/**
 * A node's disclosure button, matched on the label it shows.
 *
 * Anchored (`/^Rack 1/`), because an accessible name is the row's whole text:
 * "Rack 2 Rack 1 box" contains "Rack 1" and would otherwise match too.
 */
const toggle = (label: RegExp) => screen.getByRole('button', { name: label });

/** A box row. `hidden: true` keeps it findable while its branch is collapsed. */
const boxRow = (label: RegExp) => screen.getByRole('link', { name: label, hidden: true });

describe('LayoutTreeScreen', () => {
  it('renders the title and the lab layout, with a count on each node', async () => {
    renderScreen();

    expect(
      await screen.findByRole('heading', { level: 1, name: layoutCopy.title }),
    ).toBeInTheDocument();
    // The h1 is there from the first paint; the tree is not, so this is what
    // waits for the four queries.
    await screen.findByRole('button', { name: /^Freezer A/ });

    // Freezer → rack → drawer → boxes, from the seeded demo layout.
    expect(toggle(/^Freezer A/)).toBeInTheDocument();
    expect(toggle(/^Rack 1/)).toBeInTheDocument();
    expect(toggle(/^Top drawer/)).toBeInTheDocument();
    expect(boxRow(/^Box A/)).toBeInTheDocument();
    expect(boxRow(/^Box B/)).toBeInTheDocument();
    expect(toggle(/^Freezer B/)).toBeInTheDocument();
    expect(boxRow(/^Box C/)).toBeInTheDocument();

    // Counts are per subtree, and a box counts its positions.
    expect(within(toggle(/^Freezer A/)).getByText('2 boxes')).toBeInTheDocument();
    expect(within(toggle(/^Top drawer/)).getByText('2 boxes')).toBeInTheDocument();
    expect(within(toggle(/^Freezer B/)).getByText('1 box')).toBeInTheDocument();
    expect(within(boxRow(/^Box A/)).getByText('96 positions')).toBeInTheDocument();
    expect(within(boxRow(/^Box C/)).getByText('9 positions')).toBeInTheDocument();
  });

  it('names the container kind, matching the Qt client', async () => {
    renderScreen();

    const rack = await screen.findByRole('button', { name: /^Rack 1/ });
    expect(within(rack).getByText('Rack')).toBeInTheDocument();
  });

  it('hides archived nodes instead of greying them out', async () => {
    renderScreen();

    await screen.findByRole('button', { name: /^Freezer A/ });

    expect(screen.queryByText('Old freezer')).not.toBeInTheDocument();
    expect(screen.queryByText('Old tower')).not.toBeInTheDocument();
    expect(screen.queryByText('Old box')).not.toBeInTheDocument();
  });

  it('collapses and expands a branch, and says so with aria-expanded', async () => {
    renderScreen();
    const drawer = await screen.findByRole('button', { name: /Top drawer/ });

    expect(drawer).toHaveAttribute('aria-expanded', 'true');
    expect(boxRow(/Box A/)).toBeVisible();

    await userEvent.click(drawer);

    expect(drawer).toHaveAttribute('aria-expanded', 'false');
    expect(boxRow(/Box A/)).not.toBeVisible();
    expect(boxRow(/Box B/)).not.toBeVisible();
    // A sibling branch is untouched.
    expect(boxRow(/Box C/)).toBeVisible();

    await userEvent.click(drawer);

    expect(drawer).toHaveAttribute('aria-expanded', 'true');
    expect(boxRow(/Box A/)).toBeVisible();
  });

  it('opens the box view when a box is selected', async () => {
    renderScreen();

    await userEvent.click(await screen.findByRole('link', { name: /Box A/ }));

    expect(screen.getByText('box view')).toBeInTheDocument();
  });

  it('shows a loading state before the four lists answer', () => {
    renderScreen({ latencyMs: 50 });

    expect(screen.getByRole('status')).toBeInTheDocument();
  });

  it('shows an error state, not a half-built tree, when a list fails partway', async () => {
    // The freezer list answers; the container list does not. Rendering the
    // freezers with no contents would look like an empty lab.
    server.use(...fakeApi({ fail: { 'storage-container/list': 'INTERNAL' } }));
    renderWithProviders(
      <Routes>
        <Route path="/labs/:labId/layout" element={<LayoutTreeScreen />} />
      </Routes>,
      { route: `/labs/${LAB_ID}/layout` },
    );

    expect(await screen.findByRole('alert')).toHaveTextContent(layoutCopy.tree.errorTitle);
    expect(screen.queryByRole('button', { name: /^Freezer A/ })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Try again' })).toBeInTheDocument();
  });

  it('recovers when the retry succeeds', async () => {
    server.use(...fakeApi({ fail: { 'freezer/list': 'UNAVAILABLE' } }));
    renderWithProviders(
      <Routes>
        <Route path="/labs/:labId/layout" element={<LayoutTreeScreen />} />
      </Routes>,
      { route: `/labs/${LAB_ID}/layout` },
    );

    await screen.findByRole('alert');

    server.use(...fakeApi());
    await userEvent.click(screen.getByRole('button', { name: 'Try again' }));

    expect(await screen.findByRole('button', { name: /^Freezer A/ })).toBeInTheDocument();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('tells a lab with no layout that there is nothing yet', async () => {
    const demo = createDemoLab();
    renderScreen({
      demo: { ...demo, freezers: [], storageContainers: [], boxTypes: [], boxes: [] },
    });

    expect(
      await screen.findByRole('heading', { level: 2, name: layoutCopy.tree.emptyTitle }),
    ).toBeInTheDocument();
  });

  it('has no accessibility violations', async () => {
    const { container } = renderScreen();

    await screen.findByRole('button', { name: /^Freezer A/ });

    expect(await axe(container)).toHaveNoViolations();
  });
});
