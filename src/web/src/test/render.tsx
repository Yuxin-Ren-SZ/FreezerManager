// SPDX-License-Identifier: AGPL-3.0-or-later
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { render, type RenderOptions, type RenderResult } from '@testing-library/react';
import type { ReactElement, ReactNode } from 'react';
import { MemoryRouter } from 'react-router-dom';

/**
 * `renderWithProviders()` (TODO.md G1.2): the one render helper every screen
 * test uses, so no test has to remember which providers the app needs.
 *
 * The query client is built for tests: `retry: false` (a failing call must fail
 * the assertion, not be retried three times first) and `gcTime: 0` so nothing
 * leaks between tests. i18next is already initialised globally by
 * `src/test/setup.ts`.
 */

export interface ProvidersOptions {
  readonly queryClient?: QueryClient;
  /** Initial URL for the `MemoryRouter` wrapper. */
  readonly route?: string;
}

export function createTestQueryClient(): QueryClient {
  return new QueryClient({
    defaultOptions: {
      queries: { retry: false, gcTime: 0, staleTime: 0 },
      mutations: { retry: false },
    },
  });
}

/** The provider stack on its own, for `renderHook(fn, { wrapper })`. */
export function createWrapper(options: ProvidersOptions = {}) {
  const queryClient = options.queryClient ?? createTestQueryClient();

  return function Wrapper({ children }: { children: ReactNode }) {
    return (
      <QueryClientProvider client={queryClient}>
        <MemoryRouter initialEntries={[options.route ?? '/']}>{children}</MemoryRouter>
      </QueryClientProvider>
    );
  };
}

export interface RenderWithProvidersResult extends RenderResult {
  readonly queryClient: QueryClient;
}

export function renderWithProviders(
  ui: ReactElement,
  options: ProvidersOptions & Omit<RenderOptions, 'wrapper'> = {},
): RenderWithProvidersResult {
  const queryClient = options.queryClient ?? createTestQueryClient();
  const { route, queryClient: _ignored, ...renderOptions } = options;

  return {
    ...render(ui, { wrapper: createWrapper({ queryClient, route }), ...renderOptions }),
    queryClient,
  };
}
