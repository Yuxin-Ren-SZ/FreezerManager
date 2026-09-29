// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { ROUTES } from '../../app/route-map';
import { boxPath } from './paths';

/**
 * `boxPath` is written by hand instead of imported from the route map, because
 * the route map imports the screen that uses it — importing back would be a
 * cycle. This test is what keeps the two honest: it takes the pattern from the
 * route map itself, so renaming the route breaks it here rather than silently
 * linking every box row to a 404.
 */

describe('boxPath', () => {
  it('matches the box route in the route map, so the two cannot drift', () => {
    const route = ROUTES.find((candidate) => candidate.id === 'box');
    if (route === undefined) {
      throw new Error('the route map has no "box" route to compare against');
    }

    expect(boxPath('lab-1', 'box-2')).toBe(
      route.path.replace(':labId', 'lab-1').replace(':boxId', 'box-2'),
    );
  });

  it('encodes both ids, so an id can never escape its path segment', () => {
    expect(boxPath('lab/1', 'box 2')).toBe('/labs/lab%2F1/boxes/box%202');
  });
});
