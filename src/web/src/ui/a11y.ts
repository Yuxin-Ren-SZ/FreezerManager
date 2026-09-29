// SPDX-License-Identifier: AGPL-3.0-or-later
import { expect } from 'vitest';
import { axe } from 'vitest-axe';

/**
 * The one place G1.3's components run axe-core.
 *
 * `vitest-axe` is used through its exported `axe` runner rather than its
 * `toHaveNoViolations` matcher on purpose: the matcher has to be registered
 * with `expect.extend` from the shared setup file, and until G1.2 (#42) lands,
 * `src/test/` belongs to another agent's task. The runner is the same axe-core
 * either way; only the failure formatting differs.
 *
 * TODO(G1.2): when `src/test/setup.ts` is ours to edit, register the matcher
 * there and drop this file.
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
