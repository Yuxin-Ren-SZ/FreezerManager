// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { expectNoA11yViolations } from './a11y';
import { Badge } from './Badge';
import { Kbd } from './Kbd';
import { Skeleton } from './Skeleton';
import { Spinner } from './Spinner';
import { VisuallyHidden } from './VisuallyHidden';

describe('VisuallyHidden', () => {
  it('keeps its text in the accessibility tree', () => {
    render(<VisuallyHidden>Hidden label</VisuallyHidden>);

    expect(screen.getByText('Hidden label')).toBeInTheDocument();
  });

  it('has no accessibility violations', async () => {
    const { container } = render(<VisuallyHidden>Hidden label</VisuallyHidden>);

    await expectNoA11yViolations(container);
  });
});

describe('Spinner', () => {
  it('announces itself as a polite status region', () => {
    render(<Spinner />);

    expect(screen.getByRole('status')).toBeInTheDocument();
  });

  it('uses the caller-supplied label instead of the translated default', () => {
    render(<Spinner label="Saving sample" />);

    expect(screen.getByRole('status')).toHaveTextContent('Saving sample');
  });

  it('takes no focus: a spinner is not an interactive control', () => {
    render(<Spinner />);

    expect(screen.getByRole('status')).not.toHaveAttribute('tabindex');
  });

  it('has no accessibility violations', async () => {
    const { container } = render(<Spinner label="Loading samples" />);

    await expectNoA11yViolations(container);
  });
});

describe('Skeleton', () => {
  it('is hidden from assistive technology', () => {
    const { container } = render(<Skeleton width="12rem" />);

    expect(container.firstElementChild).toHaveAttribute('aria-hidden', 'true');
  });

  it('is not focusable', () => {
    const { container } = render(<Skeleton />);

    expect(container.firstElementChild).not.toHaveAttribute('tabindex');
  });

  it('has no accessibility violations while the busy region names itself', async () => {
    const { container } = render(
      <div aria-busy="true" aria-live="polite">
        <VisuallyHidden>Loading samples</VisuallyHidden>
        <Skeleton />
      </div>,
    );

    await expectNoA11yViolations(container);
  });
});

describe('Badge', () => {
  it('renders its label', () => {
    render(<Badge tone="success">Available</Badge>);

    expect(screen.getByText('Available')).toBeInTheDocument();
  });

  it('does not claim a widget role', () => {
    render(<Badge tone="danger">Quarantined</Badge>);

    expect(screen.queryByRole('button')).not.toBeInTheDocument();
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
  });

  it('has no accessibility violations in any tone', async () => {
    const { container } = render(
      <>
        <Badge tone="neutral">Neutral</Badge>
        <Badge tone="info">Info</Badge>
        <Badge tone="success">Success</Badge>
        <Badge tone="warning">Warning</Badge>
        <Badge tone="danger">Danger</Badge>
      </>,
    );

    await expectNoA11yViolations(container);
  });
});

describe('Kbd', () => {
  it('renders a <kbd> element so the shortcut is machine-readable', () => {
    render(<Kbd>/</Kbd>);

    expect(screen.getByText('/').tagName).toBe('KBD');
  });

  it('has no accessibility violations', async () => {
    const { container } = render(
      <p>
        Press <Kbd>/</Kbd> to search
      </p>,
    );

    await expectNoA11yViolations(container);
  });
});
