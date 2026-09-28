// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { App } from './App';

describe('App', () => {
  it('renders the placeholder page title from the bundled en locale', () => {
    render(<App />);

    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Web UI scaffold');
  });

  it('renders the placeholder body text through i18next', () => {
    render(<App />);

    expect(screen.getByText(/toolchain is wired up/i)).toBeInTheDocument();
  });
});
