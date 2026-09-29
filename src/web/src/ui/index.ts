// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * The shared UI kit (TODO.md G1.3).
 *
 * A feature screen should import from `../../ui` and never reach into a
 * component file directly, so a rename inside the kit is one edit here.
 *
 * `tokens.css` is not re-exported: it is a stylesheet, pulled in once by
 * `main.tsx` so that custom properties exist before any module renders.
 *
 * **`Table` is deliberately not here** (issue #64). This barrel is imported by
 * the shell, so it lives in the entry chunk, and a re-export from it is *not*
 * tree-shakeable: `Table.tsx` imports `Table.module.css`, which makes the
 * module side-effectful, so the bundler keeps it and everything it imports —
 * TanStack Table and TanStack Virtual — in the entry chunk whether or not an
 * entry-chunk module uses it. Measured on the G3.2 tree: leaving the line below
 * in place costs **33.5 KiB gzipped in the initial bundle** (189.2 vs 155.7
 * KiB). Import it where it is used, from `../../ui/Table`.
 */
export { Badge, type BadgeProps, type BadgeTone } from './Badge';
export { Button, type ButtonProps, type ButtonSize, type ButtonVariant } from './Button';
export { Checkbox, type CheckboxProps } from './Checkbox';
export { ConfirmDialog, type ConfirmDialogProps } from './ConfirmDialog';
export { Dialog, type DialogProps, type DialogSize } from './Dialog';
export { EmptyState, type EmptyStateProps } from './EmptyState';
export { ErrorState, type ErrorStateProps } from './ErrorState';
export {
  IconButton,
  type IconButtonProps,
  type IconButtonSize,
  type IconButtonVariant,
} from './IconButton';
export { Kbd, type KbdProps } from './Kbd';
export { Select, type SelectOption, type SelectProps } from './Select';
export { Skeleton, type SkeletonProps } from './Skeleton';
export { Spinner, type SpinnerProps, type SpinnerSize } from './Spinner';
export { Tabs, type TabItem, type TabsProps } from './Tabs';
export { TextField, type TextFieldProps } from './TextField';
export { ToastProvider, useToast, type ToastApi, type ToastOptions, type ToastTone } from './Toast';
export { VisuallyHidden } from './VisuallyHidden';
export { classNames } from './classNames';
