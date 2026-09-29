// SPDX-License-Identifier: AGPL-3.0-or-later
import { QueryClientProvider } from '@tanstack/react-query';
import { useState } from 'react';
import type { ReactNode } from 'react';
import { ConnectionProvider, type ConnectionState } from './connection';
import { LabProvider } from './labs';
import { createQueryClient } from './queryClient';
import { SessionProvider, type SessionLoader } from './session';
import { ToastProvider } from '../ui';

export interface AppProvidersProps {
  children: ReactNode;
  /** How the shell learns who is signed in; the stub until G1.2 lands. */
  loadSession: SessionLoader;
  /** Live-connection state; G1.2's SSE wrapper will drive this. */
  connectionStatus?: ConnectionState;
}

/**
 * Everything the shell and every feature screen can assume is above them:
 * TanStack Query, the session, the selected lab, the live-connection state and
 * the toast host.
 *
 * i18next is deliberately *not* here. It is initialised as a module singleton
 * by `src/app/i18n.ts`, before the first render, so there is nothing to
 * provide and no window in which a component could render a raw key.
 *
 * Order matters: the lab list comes from the session, and a toast raised while
 * a screen is unmounting still has to be announced, so the toast host is
 * innermost.
 */
export function AppProviders({
  children,
  loadSession,
  connectionStatus = 'connecting',
}: AppProvidersProps) {
  const [queryClient] = useState(createQueryClient);

  return (
    <QueryClientProvider client={queryClient}>
      <SessionProvider loadSession={loadSession}>
        <LabProvider>
          <ConnectionProvider status={connectionStatus}>
            <ToastProvider>{children}</ToastProvider>
          </ConnectionProvider>
        </LabProvider>
      </SessionProvider>
    </QueryClientProvider>
  );
}
