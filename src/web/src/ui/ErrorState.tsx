// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { Button } from './Button';
import styles from './State.module.css';
import { classNames } from './classNames';

export interface ErrorStateProps {
  title: string;
  description?: string;
  /** Omit when there is nothing sensible to retry. */
  onRetry?: () => void;
  /** Defaults to the translated "Try again". */
  retryLabel?: string;
  /**
   * The `X-Request-Id` of the failed call, shown so a user can quote one
   * request in a bug report. Never contains PHI (G-arch 7).
   */
  requestId?: string;
  className?: string;
}

/**
 * "This did not work", with a way forward.
 *
 * `role="alert"`: unlike `EmptyState`, this replaces content the user was
 * already looking at, so it has to be announced without waiting for a pause.
 */
export function ErrorState({
  title,
  description,
  onRetry,
  retryLabel,
  requestId,
  className,
}: ErrorStateProps) {
  const { t } = useTranslation('ui');

  return (
    <div className={classNames(styles.root, styles.error, className)} role="alert">
      <h2 className={styles.title}>{title}</h2>
      {description ? <p className={styles.description}>{description}</p> : null}
      {requestId ? (
        <p className={styles.requestId}>{t('errorState.requestId', { requestId })}</p>
      ) : null}
      {onRetry ? (
        <div className={styles.action}>
          <Button onClick={onRetry}>{retryLabel ?? t('errorState.retry')}</Button>
        </div>
      ) : null}
    </div>
  );
}
