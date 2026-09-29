// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * The shared UI kit (TODO.md G1.3).
 *
 * A feature screen should import from `../../ui` and never reach into a
 * component file directly, so a rename inside the kit is one edit here.
 *
 * `tokens.css` is not re-exported: it is a stylesheet, pulled in once by
 * `main.tsx` so that custom properties exist before any module renders.
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
export { Table, type TableColumn, type TableFeatures, type TableProps } from './Table';
export { Tabs, type TabItem, type TabsProps } from './Tabs';
export { TextField, type TextFieldProps } from './TextField';
export { ToastProvider, useToast, type ToastApi, type ToastOptions, type ToastTone } from './Toast';
export { VisuallyHidden } from './VisuallyHidden';
export { classNames } from './classNames';
