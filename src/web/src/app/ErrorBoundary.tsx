// SPDX-License-Identifier: AGPL-3.0-or-later
import { Component } from 'react';
import type { ErrorInfo, ReactNode } from 'react';
import { isRouteErrorResponse, useRouteError } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { ErrorState } from '../ui';
import styles from './ErrorBoundary.module.css';

export interface ErrorBoundaryProps {
  children: ReactNode;
  /** Rendered instead of the crashed subtree, with a way to try again. */
  fallback: (error: Error, reset: () => void) => ReactNode;
}

interface ErrorBoundaryState {
  error: Error | null;
}

/**
 * The last line of defence: without it, one bad render blanks the whole SPA and
 * the user has no way back except a manual reload.
 *
 * A class component because `componentDidCatch` has no hook equivalent, and a
 * `fallback` render prop because the fallback needs `t()` and a class cannot
 * call hooks. `AppErrorBoundary` below is the translated one.
 */
export class ErrorBoundary extends Component<ErrorBoundaryProps, ErrorBoundaryState> {
  override state: ErrorBoundaryState = { error: null };

  static getDerivedStateFromError(error: Error): ErrorBoundaryState {
    return { error };
  }

  override componentDidCatch(error: Error, info: ErrorInfo): void {
    // G-arch 7: API payloads are never logged to the console, so this stays on
    // the message and the component stack. When a real error reporter exists it
    // hangs off this method.
    console.error('Unhandled render error:', error.message, info.componentStack);
  }

  override render(): ReactNode {
    const { error } = this.state;
    if (error === null) {
      return this.props.children;
    }
    return this.props.fallback(error, () => {
      this.setState({ error: null });
    });
  }
}

/** The app-wide boundary, with translated copy. */
export function AppErrorBoundary({ children }: { children: ReactNode }) {
  const { t } = useTranslation('shell');

  return (
    <ErrorBoundary
      fallback={(_error, reset) => (
        <main className={styles.page}>
          <ErrorState
            title={t('error.title')}
            description={t('error.body')}
            onRetry={reset}
            retryLabel={t('error.reset')}
          />
        </main>
      )}
    >
      {children}
    </ErrorBoundary>
  );
}

/**
 * `errorElement` for the router: catches loader and render errors per route
 * instead of blanking the shell, and knows the difference between "the route
 * threw" and "the route 404'd".
 */
export function RouteErrorBoundary() {
  const { t } = useTranslation('shell');
  const error: unknown = useRouteError();

  const description = isRouteErrorResponse(error)
    ? `${String(error.status)} ${error.statusText}`
    : error instanceof Error
      ? error.message
      : undefined;

  return (
    <main className={styles.page}>
      <ErrorState title={t('error.title')} description={description} />
    </main>
  );
}
