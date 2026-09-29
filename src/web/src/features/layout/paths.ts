// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * The address of a box's view (TODO.md G3.4), which the layout tree links to.
 *
 * The string is written out here rather than read from `src/app/route-map.tsx`
 * on purpose: the route map imports this feature's screen, so reaching back
 * into it would be an import cycle. `paths.test.ts` pins the two together
 * instead — it takes the pattern from the route map and fails if they drift.
 */
export function boxPath(labId: string, boxId: string): string {
  return `/labs/${encodeURIComponent(labId)}/boxes/${encodeURIComponent(boxId)}`;
}
