// SPDX-License-Identifier: AGPL-3.0-or-later
import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { Button } from './Button';
import { Checkbox } from './Checkbox';
import { IconButton } from './IconButton';
import { Select } from './Select';
import { TextField } from './TextField';
import { expectNoA11yViolations } from './a11y';

describe('Button', () => {
  it('defaults to type="button" so it never submits a form by accident', () => {
    render(<Button>Save</Button>);

    expect(screen.getByRole('button', { name: 'Save' })).toHaveAttribute('type', 'button');
  });

  it('keeps the caller-supplied type when one is given', () => {
    render(<Button type="submit">Save</Button>);

    expect(screen.getByRole('button', { name: 'Save' })).toHaveAttribute('type', 'submit');
  });

  it('runs its handler on click', async () => {
    const onClick = vi.fn();
    render(<Button onClick={onClick}>Save</Button>);

    await userEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(onClick).toHaveBeenCalledTimes(1);
  });

  it('is reachable and activatable from the keyboard alone', async () => {
    const onClick = vi.fn();
    render(<Button onClick={onClick}>Save</Button>);

    await userEvent.tab();
    expect(screen.getByRole('button', { name: 'Save' })).toHaveFocus();
    await userEvent.keyboard('{Enter}');
    await userEvent.keyboard(' ');

    expect(onClick).toHaveBeenCalledTimes(2);
  });

  it('is not reachable by tab while disabled', async () => {
    render(<Button disabled>Save</Button>);

    await userEvent.tab();

    expect(screen.getByRole('button', { name: 'Save' })).not.toHaveFocus();
  });

  it('reports itself busy and swallows clicks while loading', async () => {
    const onClick = vi.fn();
    render(
      <Button loading onClick={onClick}>
        Save
      </Button>,
    );
    const button = screen.getByRole('button', { name: /save/i });

    expect(button).toHaveAttribute('aria-busy', 'true');
    expect(button).toBeDisabled();

    await userEvent.click(button);
    expect(onClick).not.toHaveBeenCalled();
  });

  it('has no accessibility violations in every variant', async () => {
    const { container } = render(
      <>
        <Button variant="primary">Primary</Button>
        <Button variant="secondary">Secondary</Button>
        <Button variant="danger">Danger</Button>
        <Button variant="ghost">Ghost</Button>
        <Button loading>Busy</Button>
        <Button disabled>Disabled</Button>
      </>,
    );

    await expectNoA11yViolations(container);
  });
});

describe('IconButton', () => {
  it('takes its accessible name from the required label', () => {
    render(<IconButton label="Close the dialog">×</IconButton>);

    expect(screen.getByRole('button', { name: 'Close the dialog' })).toBeInTheDocument();
  });

  it('is activatable with the keyboard', async () => {
    const onClick = vi.fn();
    render(
      <IconButton label="Close" onClick={onClick}>
        ×
      </IconButton>,
    );

    await userEvent.tab();
    await userEvent.keyboard('{Enter}');

    expect(onClick).toHaveBeenCalledTimes(1);
  });

  it('has no accessibility violations', async () => {
    const { container } = render(
      <IconButton label="Dismiss notification" variant="ghost">
        ×
      </IconButton>,
    );

    await expectNoA11yViolations(container);
  });
});

describe('TextField', () => {
  it('associates its label with the input', () => {
    render(<TextField label="Sample name" />);

    expect(screen.getByLabelText('Sample name')).toBeInTheDocument();
  });

  it('accepts typing', async () => {
    render(<TextField label="Sample name" />);

    await userEvent.type(screen.getByLabelText('Sample name'), 'Liver');

    expect(screen.getByLabelText('Sample name')).toHaveValue('Liver');
  });

  it('describes the input with the hint text', () => {
    render(<TextField label="Sample name" hint="At least two characters" />);

    expect(screen.getByLabelText('Sample name')).toHaveAccessibleDescription(
      'At least two characters',
    );
  });

  it('marks the input invalid and describes it with the error', () => {
    render(<TextField label="Sample name" error="A name is required" />);

    const input = screen.getByLabelText('Sample name');
    expect(input).toHaveAttribute('aria-invalid', 'true');
    expect(input).toHaveAccessibleDescription('A name is required');
  });

  it('announces the error politely so it is read when it appears', () => {
    render(<TextField label="Sample name" error="A name is required" />);

    expect(screen.getByRole('alert')).toHaveTextContent('A name is required');
  });

  it('keeps the hint in the description and appends the error to it', () => {
    render(<TextField label="Sample name" hint="Hint" error="Error" />);

    expect(screen.getByLabelText('Sample name')).toHaveAccessibleDescription('Hint Error');
  });

  it('is reachable by tab and cannot be reached when disabled', async () => {
    render(
      <>
        <TextField label="Enabled" />
        <TextField label="Disabled" disabled />
      </>,
    );

    await userEvent.tab();
    expect(screen.getByLabelText('Enabled')).toHaveFocus();
    await userEvent.tab();
    expect(screen.getByLabelText('Disabled')).not.toHaveFocus();
  });

  it('has no accessibility violations', async () => {
    const { container } = render(
      <>
        <TextField label="Sample name" hint="At least two characters" />
        <TextField label="Barcode" error="Already in use" required />
        <TextField label="Notes" disabled />
      </>,
    );

    await expectNoA11yViolations(container);
  });
});

describe('Select', () => {
  const options = [
    { value: 'f1', label: 'Freezer 1' },
    { value: 'f2', label: 'Freezer 2' },
  ];

  it('associates its label and renders every option', () => {
    render(<Select label="Freezer" options={options} />);

    const select = screen.getByLabelText('Freezer');
    expect(select).toBeInTheDocument();
    expect(screen.getAllByRole('option')).toHaveLength(2);
  });

  it('can be changed from the keyboard', async () => {
    render(<Select label="Freezer" options={options} />);
    const select = screen.getByLabelText('Freezer');

    await userEvent.tab();
    expect(select).toHaveFocus();
    await userEvent.selectOptions(select, 'f2');

    expect(select).toHaveValue('f2');
  });

  it('renders a placeholder option when asked to', () => {
    render(<Select label="Freezer" options={options} placeholder="Pick a freezer" />);

    expect(screen.getByRole('option', { name: 'Pick a freezer' })).toBeInTheDocument();
  });

  it('has no accessibility violations', async () => {
    const { container } = render(
      <Select label="Freezer" options={options} placeholder="Pick a freezer" />,
    );

    await expectNoA11yViolations(container);
  });
});

describe('Checkbox', () => {
  it('toggles with the keyboard', async () => {
    render(<Checkbox label="Only available" />);

    await userEvent.tab();
    expect(screen.getByLabelText('Only available')).toHaveFocus();
    await userEvent.keyboard(' ');

    expect(screen.getByLabelText('Only available')).toBeChecked();
  });

  it('respects the controlled checked prop', async () => {
    const onChange = vi.fn();
    render(<Checkbox label="Only available" checked={false} onChange={onChange} />);

    await userEvent.click(screen.getByLabelText('Only available'));

    expect(onChange).toHaveBeenCalledTimes(1);
    expect(screen.getByLabelText('Only available')).not.toBeChecked();
  });

  it('describes itself with its hint', () => {
    render(<Checkbox label="Only available" hint="Hide checked-out samples" />);

    expect(screen.getByLabelText('Only available')).toHaveAccessibleDescription(
      'Hide checked-out samples',
    );
  });

  it('has no accessibility violations', async () => {
    const { container } = render(
      <>
        <Checkbox label="Only available" />
        <Checkbox label="Include archived" hint="Tombstoned samples are excluded by default" />
        <Checkbox label="Disabled" disabled />
      </>,
    );

    await expectNoA11yViolations(container);
  });
});
