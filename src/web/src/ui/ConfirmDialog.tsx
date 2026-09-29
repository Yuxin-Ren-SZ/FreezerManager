// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import { Button } from './Button';
import { Dialog } from './Dialog';

export interface ConfirmDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: string;
  description?: string;
  confirmLabel: string;
  cancelLabel: string;
  /** `danger` for anything that destroys or moves data. */
  tone?: 'primary' | 'danger';
  /** Disables both actions and shows a spinner on the confirm button. */
  pending?: boolean;
  onConfirm: () => void;
  children?: ReactNode;
}

/**
 * The "are you sure?" dialog, expressed in terms of `Dialog`.
 *
 * `cancelLabel` has no default on purpose: "Cancel" is the wrong word for
 * "Keep", "Don't delete" or "Leave without saving", and the button that
 * protects the user's data should say what it does.
 */
export function ConfirmDialog({
  open,
  onOpenChange,
  title,
  description,
  confirmLabel,
  cancelLabel,
  tone = 'primary',
  pending = false,
  onConfirm,
  children,
}: ConfirmDialogProps) {
  return (
    <Dialog
      open={open}
      onOpenChange={onOpenChange}
      title={title}
      description={description}
      size="sm"
      footer={
        <>
          <Button
            disabled={pending}
            onClick={() => {
              onOpenChange(false);
            }}
          >
            {cancelLabel}
          </Button>
          <Button variant={tone} loading={pending} onClick={onConfirm}>
            {confirmLabel}
          </Button>
        </>
      }
    >
      {children}
    </Dialog>
  );
}
