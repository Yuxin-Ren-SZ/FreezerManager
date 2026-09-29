// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { Button } from './Button';
import { ConfirmDialog } from './ConfirmDialog';
import { Dialog } from './Dialog';
import { ToastProvider, useToast } from './Toast';
import { expectNoA11yViolations } from './a11y';

describe('Dialog', () => {
  it('renders its title and body when open', () => {
    render(
      <Dialog open onOpenChange={vi.fn()} title="New sample">
        <p>Body content</p>
      </Dialog>,
    );

    expect(screen.getByRole('dialog', { name: 'New sample' })).toBeInTheDocument();
    expect(screen.getByText('Body content')).toBeInTheDocument();
  });

  it('renders nothing while closed', () => {
    render(
      <Dialog open={false} onOpenChange={vi.fn()} title="New sample">
        <p>Body content</p>
      </Dialog>,
    );

    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('asks to close when Escape is pressed', async () => {
    const onOpenChange = vi.fn();
    render(
      <Dialog open onOpenChange={onOpenChange} title="New sample">
        <p>Body content</p>
      </Dialog>,
    );

    await userEvent.keyboard('{Escape}');

    expect(onOpenChange).toHaveBeenCalledWith(false);
  });

  it('offers a named close control reachable from the keyboard', async () => {
    const onOpenChange = vi.fn();
    render(
      <Dialog open onOpenChange={onOpenChange} title="New sample">
        <p>Body content</p>
      </Dialog>,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Close' }));

    expect(onOpenChange).toHaveBeenCalledWith(false);
  });

  it('moves focus into the dialog when it opens', async () => {
    render(
      <Dialog open onOpenChange={vi.fn()} title="New sample">
        <Button>Inner action</Button>
      </Dialog>,
    );

    await waitFor(() => {
      expect(screen.getByRole('dialog')).toContainElement(
        document.activeElement as HTMLElement | null,
      );
    });
  });

  it('has no accessibility violations', async () => {
    render(
      <Dialog open onOpenChange={vi.fn()} title="New sample" description="Stored in Freezer 1">
        <Button>Inner action</Button>
      </Dialog>,
    );

    await screen.findByRole('dialog');
    await expectNoA11yViolations(document.body);
  });
});

describe('ConfirmDialog', () => {
  const baseProps = {
    open: true,
    onOpenChange: vi.fn(),
    title: 'Delete this sample?',
    description: 'The sample is tombstoned, not erased.',
    confirmLabel: 'Delete',
    cancelLabel: 'Keep',
  };

  it('renders both actions and the description', () => {
    render(<ConfirmDialog {...baseProps} onConfirm={vi.fn()} />);

    expect(screen.getByRole('button', { name: 'Delete' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Keep' })).toBeInTheDocument();
    expect(screen.getByText('The sample is tombstoned, not erased.')).toBeInTheDocument();
  });

  it('calls onConfirm when the confirm action is used', async () => {
    const onConfirm = vi.fn();
    render(<ConfirmDialog {...baseProps} onConfirm={onConfirm} />);

    await userEvent.click(screen.getByRole('button', { name: 'Delete' }));

    expect(onConfirm).toHaveBeenCalledTimes(1);
  });

  it('closes without confirming when the cancel action is used', async () => {
    const onConfirm = vi.fn();
    const onOpenChange = vi.fn();
    render(<ConfirmDialog {...baseProps} onOpenChange={onOpenChange} onConfirm={onConfirm} />);

    await userEvent.click(screen.getByRole('button', { name: 'Keep' }));

    expect(onConfirm).not.toHaveBeenCalled();
    expect(onOpenChange).toHaveBeenCalledWith(false);
  });

  it('blocks both actions while the confirmation is in flight', () => {
    render(<ConfirmDialog {...baseProps} pending onConfirm={vi.fn()} />);

    expect(screen.getByRole('button', { name: 'Keep' })).toBeDisabled();
    expect(screen.getByRole('button', { name: /delete/i })).toBeDisabled();
  });

  it('has no accessibility violations', async () => {
    render(<ConfirmDialog {...baseProps} tone="danger" onConfirm={vi.fn()} />);

    await screen.findByRole('dialog');
    await expectNoA11yViolations(document.body);
  });
});

function ToastTrigger({ duration }: { duration?: number }) {
  const { show } = useToast();
  return (
    <Button
      onClick={() => {
        show({ title: 'Sample saved', tone: 'success', duration });
      }}
    >
      Save
    </Button>
  );
}

describe('Toast', () => {
  it('announces a shown toast in a labelled notifications region', async () => {
    render(
      <ToastProvider>
        <ToastTrigger duration={0} />
      </ToastProvider>,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(screen.getByRole('region', { name: 'Notifications' })).toBeInTheDocument();
    expect(screen.getByText('Sample saved')).toBeInTheDocument();
  });

  it('uses an assertive alert for failures', async () => {
    function FailureTrigger() {
      const { show } = useToast();
      return (
        <Button
          onClick={() => {
            show({ title: 'Could not save', tone: 'danger', duration: 0 });
          }}
        >
          Fail
        </Button>
      );
    }
    render(
      <ToastProvider>
        <FailureTrigger />
      </ToastProvider>,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Fail' }));

    expect(screen.getByRole('alert')).toHaveTextContent('Could not save');
  });

  it('dismisses on request, from the keyboard', async () => {
    render(
      <ToastProvider>
        <ToastTrigger duration={0} />
      </ToastProvider>,
    );
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    await userEvent.tab();
    expect(screen.getByRole('button', { name: 'Dismiss notification' })).toHaveFocus();
    await userEvent.keyboard('{Enter}');

    await waitFor(() => {
      expect(screen.queryByText('Sample saved')).not.toBeInTheDocument();
    });
  });

  it('removes itself after the provider duration', async () => {
    render(
      <ToastProvider defaultDuration={25}>
        <ToastTrigger />
      </ToastProvider>,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    await waitFor(() => {
      expect(screen.queryByText('Sample saved')).not.toBeInTheDocument();
    });
  });

  it('keeps a toast with duration 0 until it is dismissed, while a timed one goes', async () => {
    function TwoToasts() {
      const { show } = useToast();
      return (
        <Button
          onClick={() => {
            show({ title: 'Sticky notice', duration: 0 });
            show({ title: 'Fleeting notice' });
          }}
        >
          Save
        </Button>
      );
    }
    render(
      <ToastProvider defaultDuration={25}>
        <TwoToasts />
      </ToastProvider>,
    );

    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    await waitFor(() => {
      expect(screen.queryByText('Fleeting notice')).not.toBeInTheDocument();
    });
    expect(screen.getByText('Sticky notice')).toBeInTheDocument();
  });

  it('refuses to be used outside its provider', () => {
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);

    expect(() => render(<ToastTrigger />)).toThrow(/ToastProvider/);

    consoleError.mockRestore();
  });

  it('has no accessibility violations', async () => {
    render(
      <ToastProvider>
        <main>
          <ToastTrigger duration={0} />
        </main>
      </ToastProvider>,
    );
    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    await expectNoA11yViolations(document.body);
  });
});
