// SPDX-License-Identifier: AGPL-3.0-or-later
import '@testing-library/jest-dom/vitest';
import { cleanup } from '@testing-library/react';
import { afterAll, afterEach, beforeAll } from 'vitest';
// Initialises i18next for every test file, the same way src/main.tsx does for
// the app: a component under test always renders translated text, never keys.
import '../app/i18n';
import { server } from './server';

// Vitest runs with `globals: false` (see vite.config.ts), so nothing is
// registered implicitly: the matchers, the MSW lifecycle and RTL cleanup are
// wired up here, once, for every test file.

beforeAll(() => {
  server.listen({ onUnhandledRequest: 'error' });
});

afterEach(() => {
  server.resetHandlers();
  cleanup();
});

afterAll(() => {
  server.close();
});
