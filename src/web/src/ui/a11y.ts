// SPDX-License-Identifier: AGPL-3.0-or-later
import { expect } from 'vitest';
import { axe } from 'vitest-axe';

/**
 * The one place G1.3's components run axe-core.
 *
 * `vitest-axe` is used through its exported `axe` runner rather than its
 * `toHaveNoViolations` matcher on purpose: the matcher has to be registered
 * with `expect.extend` from the shared setup file, and when this was written
 * `src/test/` belonged to another agent's task. That constraint is gone now
 * (#42 merged), so this is a tidy-up rather than a fix — the runner is the same
 * axe-core and only the failure formatting differs.
 *
 * TODO(#46): register `toHaveNoViolations` in `src/test/setup.ts` and delete
 * this file. Tracked as its own issue because it touches the shared test setup,
 * not because anything here is wrong.
 */
export async function expectNoA11yViolations(container: Element): Promise<void> {
  const results = await axe(container, {
    rules: {
      // jsdom has no layout engine and no canvas, so axe's colour-contrast
      // check can only ever report "incomplete" here (and logs a
      // `getContext()` warning per node). Contrast is not unchecked: it is
      // computed from the real token values in `tokens.contrast.test.ts`.
      'color-contrast': { enabled: false },
    },
  });

  const summary = results.violations.map(
    (violation) =>
      `${violation.id} [${violation.impact ?? 'unknown'}] ${violation.help} — ` +
      `${String(violation.nodes.length)} node(s), e.g. ${violation.nodes[0]?.target.join(' ') ?? '?'}`,
  );

  expect(summary, 'axe-core reported accessibility violations').toEqual([]);
}
