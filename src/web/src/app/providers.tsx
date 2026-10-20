// SPDX-License-Identifier: AGPL-3.0-or-later
import { QueryClientProvider } from '@tanstack/react-query';
import { useState } from 'react';
import type { ReactNode } from 'react';
import { LabProvider } from './labs';
import { createQueryClient } from './queryClient';
import { SessionProvider, type SessionLoader } from './session';
import { SessionExpiryWatcher } from './sessionExpiry';
import { ToastProvider } from '../ui';

export interface AppProvidersProps {
  children: ReactNode;
  /** How the shell learns who is signed in; the `auth/whoami` call (G0.2). */
  loadSession: SessionLoader;
}

/**
 * Everything the shell and every feature screen can assume is above them:
 * TanStack Query, the session, the selected lab and the toast host.
 *
 * The live-connection state is deliberately *not* provided here: it is
 * transport state, owned by `src/api/sse.ts` and read through
 * `app/connection.tsx`, so it follows the real streams instead of a prop the
 * root would have to guess (that guess was the #141 bug).
 *
 * i18next is deliberately *not* here. It is initialised as a module singleton
 * by `src/app/i18n.ts`, before the first render, so there is nothing to
 * provide and no window in which a component could render a raw key.
 *
 * Order matters: the lab list comes from the session, and a toast raised while
 * a screen is unmounting still has to be announced, so the toast host is
 * innermost.
 */
export function AppProviders({ children, loadSession }: AppProvidersProps) {
  const [queryClient] = useState(createQueryClient);

  return (
    <QueryClientProvider client={queryClient}>
      <SessionProvider loadSession={loadSession}>
        {/* Inside the session, because "the session ended" is what it writes. */}
        <SessionExpiryWatcher />
        <LabProvider>
          <ToastProvider>{children}</ToastProvider>
        </LabProvider>
      </SessionProvider>
    </QueryClientProvider>
  );
}
