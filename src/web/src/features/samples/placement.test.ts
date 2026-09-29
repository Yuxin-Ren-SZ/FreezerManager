// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import type { LocationPath } from '../layout/layoutModel';
import { placementKind, placementPath } from './placement';

/**
 * How a sample's placement reads in the table (TODO.md G3.2).
 *
 * The distinction this file exists for comes from the #52 review: G3.1's
 * `resolveLocationPath` answers `{placed: false, partial: true}` for a box id
 * it cannot resolve, which is *not* the same thing as a sample that has never
 * been placed. Qt returns a hard error for that case; the web table has to say
 * something, and "never placed" would be a lie about a deleted box.
 */

const path = (overrides: Partial<LocationPath>): LocationPath => ({
  placed: false,
  partial: false,
  segments: [],
  ...overrides,
});

describe('placementKind', () => {
  it('is placed when the walk reached a box', () => {
    expect(
      placementKind(
        path({ placed: true, segments: [{ kind: 'box', label: 'Box A', containerKind: null }] }),
      ),
    ).toBe('placed');
  });

  it('is unplaced when no box was named at all', () => {
    // An empty `box_id` is a normal state: the sample exists, it is just not in
    // a box yet.
    expect(placementKind(path({ placed: false, partial: false }))).toBe('unplaced');
  });

  it('is unknown — not unplaced — when a box id could not be resolved', () => {
    // `{placed: false, partial: true}`: a id was named, the chain broke (the box
    // is deleted, archived differently, or outside the loaded layout).
    expect(placementKind(path({ placed: false, partial: true }))).toBe('unknown');
  });

  it('is placed even when the path is partial, because the box is on screen', () => {
    // A cycle above the box still names a box the user can act on; only the
    // upper part of the path is best effort.
    expect(
      placementKind(
        path({
          placed: true,
          partial: true,
          segments: [{ kind: 'box', label: 'Box A', containerKind: null }],
        }),
      ),
    ).toBe('placed');
  });
});

describe('placementPath', () => {
  it('joins the segments outermost first', () => {
    const located = path({
      placed: true,
      segments: [
        { kind: 'freezer', label: 'Freezer A', containerKind: null },
        { kind: 'container', label: 'Top drawer', containerKind: null },
        { kind: 'box', label: 'Box A', containerKind: null },
        { kind: 'position', label: 'A1', containerKind: null },
      ],
    });

    expect(placementPath(located)).toBe('Freezer A › Top drawer › Box A › A1');
  });

  it('is empty when there is nothing to show', () => {
    expect(placementPath(path({}))).toBe('');
  });
});
