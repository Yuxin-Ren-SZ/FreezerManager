// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { safeNext } from './next';

/**
 * The `next` parameter is attacker-controllable: it arrives in a URL, and the
 * sign-in screen navigates to it. A wrong answer here turns `/login` into an
 * open redirect, so the tests are mostly about what is *refused*.
 */
describe('safeNext', () => {
  it('keeps a same-origin path, with its query string', () => {
    expect(safeNext('?next=%2Flabs%2Flab-1%2Fsamples')).toBe('/labs/lab-1/samples');
    expect(safeNext('?next=%2Flookup%3Fq%3Dliver')).toBe('/lookup?q=liver');
  });

  it('falls back to the dashboard when there is no next target', () => {
    expect(safeNext('')).toBe('/');
    expect(safeNext('?other=1')).toBe('/');
    expect(safeNext('?next=')).toBe('/');
  });

  it.each([
    // Absolute URLs: another origin entirely.
    'https://evil.example/steal',
    'http://evil.example',
    // Protocol-relative, and the backslash form browsers also read that way.
    '//evil.example',
    '/\\evil.example',
    // Not a path at all.
    'javascript:alert(1)',
    'evil.example',
  ])('refuses %s', (target) => {
    expect(safeNext(`?next=${encodeURIComponent(target)}`)).toBe('/');
  });
});
