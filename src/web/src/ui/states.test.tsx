// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { Button } from './Button';
import { EmptyState } from './EmptyState';
import { ErrorState } from './ErrorState';
import { expectNoA11yViolations } from './a11y';

describe('EmptyState', () => {
  it('renders its title and description as a level-2 heading', () => {
    render(<EmptyState title="No samples yet" description="Store one to get started." />);

    expect(screen.getByRole('heading', { level: 2, name: 'No samples yet' })).toBeInTheDocument();
    expect(screen.getByText('Store one to get started.')).toBeInTheDocument();
  });

  it('renders its action so the user can leave the dead end', () => {
    render(<EmptyState title="No samples yet" action={<Button>New sample</Button>} />);

    expect(screen.getByRole('button', { name: 'New sample' })).toBeInTheDocument();
  });

  it('has no accessibility violations', async () => {
    const { container } = render(
      <main>
        <EmptyState
          title="No samples yet"
          description="Store one to get started."
          action={<Button>New sample</Button>}
        />
      </main>,
    );

    await expectNoA11yViolations(container);
  });
});

describe('ErrorState', () => {
  it('renders the failure as a level-2 heading with its explanation', () => {
    render(<ErrorState title="Could not load samples" description="The server is unreachable." />);

    expect(
      screen.getByRole('heading', { level: 2, name: 'Could not load samples' }),
    ).toBeInTheDocument();
    expect(screen.getByText('The server is unreachable.')).toBeInTheDocument();
  });

  it('offers a retry action that calls back', async () => {
    const onRetry = vi.fn();
    render(<ErrorState title="Could not load samples" onRetry={onRetry} />);

    await userEvent.click(screen.getByRole('button', { name: 'Try again' }));

    expect(onRetry).toHaveBeenCalledTimes(1);
  });

  it('uses the caller-supplied retry label', () => {
    render(<ErrorState title="Could not save" onRetry={vi.fn()} retryLabel="Retry the save" />);

    expect(screen.getByRole('button', { name: 'Retry the save' })).toBeInTheDocument();
  });

  it('omits the retry button when there is nothing to retry', () => {
    render(<ErrorState title="Not found" />);

    expect(screen.queryByRole('button')).not.toBeInTheDocument();
  });

  it('surfaces the request id so a report can point at one request', () => {
    render(<ErrorState title="Could not load samples" requestId="0f4d2c" />);

    expect(screen.getByText(/0f4d2c/)).toBeInTheDocument();
  });

  it('announces itself immediately, because it replaces content that was there', () => {
    render(<ErrorState title="Could not load samples" />);

    expect(screen.getByRole('alert')).toBeInTheDocument();
  });

  it('has no accessibility violations', async () => {
    const { container } = render(
      <main>
        <ErrorState
          title="Could not load samples"
          description="The server is unreachable."
          requestId="0f4d2c"
          onRetry={vi.fn()}
        />
      </main>,
    );

    await expectNoA11yViolations(container);
  });
});
