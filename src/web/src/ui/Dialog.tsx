// SPDX-License-Identifier: AGPL-3.0-or-later
import * as RadixDialog from '@radix-ui/react-dialog';
import type { ReactNode } from 'react';
import { useTranslation } from 'react-i18next';
import { IconButton } from './IconButton';
import styles from './Dialog.module.css';
import { classNames } from './classNames';

/** Not a translatable string: it is a glyph, and the name comes from `label`. */
const CLOSE_GLYPH = '\u00d7';

export type DialogSize = 'sm' | 'md' | 'lg';

export interface DialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  title: string;
  description?: string;
  children?: ReactNode;
  /** Rendered in the footer bar, usually the action buttons. */
  footer?: ReactNode;
  size?: DialogSize;
}

/**
 * A modal dialog on the Radix primitive.
 *
 * G-arch 1 allows Radix "only where accessibility is hard", and a modal is
 * exactly that: focus trapping, focus restore, `aria-modal`, scroll locking and
 * inert background content are all things a hand-rolled dialog gets subtly
 * wrong. `title` is required because Radix uses it as the dialog's accessible
 * name.
 */
export function Dialog({
  open,
  onOpenChange,
  title,
  description,
  children,
  footer,
  size = 'md',
}: DialogProps) {
  const { t } = useTranslation('ui');

  return (
    <RadixDialog.Root open={open} onOpenChange={onOpenChange}>
      <RadixDialog.Portal>
        <RadixDialog.Overlay className={styles.overlay} />
        <RadixDialog.Content
          className={classNames(styles.content, styles[size])}
          // Radix warns when a dialog has neither a description nor an explicit
          // `undefined`, so say so rather than let it guess.
          aria-describedby={description ? undefined : undefined}
        >
          <header className={styles.header}>
            <RadixDialog.Title className={styles.title}>{title}</RadixDialog.Title>
            <RadixDialog.Close asChild>
              <IconButton label={t('dialog.close')} size="sm">
                {CLOSE_GLYPH}
              </IconButton>
            </RadixDialog.Close>
          </header>
          {description ? (
            <RadixDialog.Description className={styles.description}>
              {description}
            </RadixDialog.Description>
          ) : null}
          {children ? <div className={styles.body}>{children}</div> : null}
          {footer ? <footer className={styles.footer}>{footer}</footer> : null}
        </RadixDialog.Content>
      </RadixDialog.Portal>
    </RadixDialog.Root>
  );
}
