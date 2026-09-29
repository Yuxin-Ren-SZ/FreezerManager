// SPDX-License-Identifier: AGPL-3.0-or-later
import '@testing-library/jest-dom/vitest';
import { cleanup } from '@testing-library/react';
import { configureAxe } from 'vitest-axe';
import * as matchers from 'vitest-axe/matchers';
import { afterAll, afterEach, beforeAll, expect } from 'vitest';
// Initialises i18next for every test file, the same way src/main.tsx does for
// the app: a component under test always renders translated text, never keys.
import '../app/i18n';
import { server } from './server';

// Vitest runs with `globals: false` (see vite.config.ts), so nothing is
// registered implicitly: the matchers, the MSW lifecycle and RTL cleanup are
// wired up here, once, for every test file.
expect.extend(matchers);

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

/**
 * The one place the UI tests run axe-core.
 *
 * `toHaveNoViolations` asserts on axe's *results*, not on a container, so a
 * test reads `expect(await axe(container)).toHaveNoViolations()`. Both halves
 * live here because both have to be global: the matcher is registered once via
 * `expect.extend`, and every run has to share one rule configuration. Tests
 * import `axe` from this file for that reason — it is the only module that has
 * the configured runner.
 *
 * `color-contrast` is off because jsdom has no layout engine and no canvas, so
 * axe can only ever report it as "incomplete" there (and logs a `getContext()`
 * warning per node). Contrast is not unchecked: it is computed from the real
 * token values in `tokens.contrast.test.ts`, which reads `tokens.css` back
 * through the bundler — see `css: true` in vite.config.ts for why that import
 * needs the setting to return anything at all.
 */
export const axe = configureAxe({
  rules: {
    'color-contrast': { enabled: false },
  },
});

declare module 'vitest' {
  // `Matchers` is the interface every `Assertion` extends, and the only one of
  // the two that Vitest 5 declares exactly once — augmenting `Assertion`
  // directly fails with TS2428 ("All declarations of 'Assertion' must have
  // identical type parameters") because Vitest ships two incompatible
  // declarations of it. `T` is unused here but cannot be renamed or dropped:
  // the interface only merges while the parameter list matches Vitest's.
  // eslint-disable-next-line @typescript-eslint/no-unused-vars -- TS2428 otherwise
  interface Matchers<R extends void | Promise<void> = void | Promise<void>, T = unknown> {
    /**
     * Asserts that the axe results passed in report no violations. It takes the
     * *results* of `axe()`, not a container: `expect(await axe(x))`, never
     * `expect(x)`. Anything else throws "No violations found in aXe results
     * object".
     */
    toHaveNoViolations(): R;
  }
}

// The augmentation is written out here instead of imported from
// `vitest-axe/extend-expect` because vitest-axe 0.1.0 ships only the legacy
// `declare global { namespace Vi { interface Assertion } }` form, which Vitest
// 5 does not read: with that import in place `tsc` still reports
// `Property 'toHaveNoViolations' does not exist on type 'Assertion<...>'` at
// every call site. The runtime matcher comes from `vitest-axe/matchers` either
// way; only the types had to be restated. Drop this block when a vitest-axe
// release augments `module 'vitest'`.
