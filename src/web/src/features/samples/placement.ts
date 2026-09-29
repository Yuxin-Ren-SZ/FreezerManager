// SPDX-License-Identifier: AGPL-3.0-or-later
import type { LocationPath } from '../layout/layoutModel';

/**
 * Where a sample is, in the three states the table can honestly show (TODO.md
 * G3.2, carrying a G3.1 review finding).
 *
 * `LocationPath.placed` alone cannot answer this. G3.1's `locationPath` returns
 * `{placed: false, partial: true}` for a box id it could not resolve — a
 * *broken chain*, not an absent one — and Qt's resolver treats that as a hard
 * error. A table cell has to say something, so:
 *
 *  - `placed`: the walk reached a box; the path is shown (and may still be
 *    partial above the box, which the box segment itself does not depend on);
 *  - `unplaced`: no box was named at all — a normal state for a fresh sample;
 *  - `unknown`: a box *was* named and could not be resolved (deleted box, a box
 *    in another lab, or a layout list that has not loaded).
 *
 * "Deleted box" and "never placed" are different things, and only the first two
 * states map to "not placed".
 */
export type PlacementKind = 'placed' | 'unplaced' | 'unknown';

export function placementKind(path: LocationPath): PlacementKind {
  if (path.placed) {
    return 'placed';
  }
  return path.partial ? 'unknown' : 'unplaced';
}

/**
 * The path as one line, outermost first: `Freezer A › Top drawer › Box A › A1`.
 *
 * A separator glyph rather than a translated word, the same way breadcrumb
 * trails are drawn everywhere; the accessible name of the cell is the path
 * itself.
 */
export function placementPath(path: LocationPath): string {
  return path.segments.map((segment) => segment.label).join(' \u203a ');
}
