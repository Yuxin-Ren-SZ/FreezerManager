// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback, useMemo } from 'react';
import { useBoxes, useBoxTypes, useFreezers, useStorageContainers } from '../../api/hooks';
import type { Box, BoxType, Freezer, StorageContainer } from '../../gen/fmgr/v1/box_pb';
import {
  buildLayoutTree,
  resolveLocationPath,
  type LabLayoutData,
  type LayoutNode,
  type LocationPath,
} from './layoutModel';

/**
 * The lab layout, loaded once per lab and shared by every screen that needs it
 * (TODO.md G3.1; G3.2–G3.5 and G3.8 reuse it).
 *
 * The four `BoxService` lists are separate queries rather than one, because
 * that is what the RPCs are and because the box view (G3.4) wants one of them
 * without the others; TanStack Query deduplicates them by key, so N screens in
 * one lab still cost one request per resource.
 *
 * The derived tree and the path helper come from `layoutModel`, which is pure:
 * this hook only decides *when* they are computed. Both are safe to call while
 * the data is still loading — they see empty lists and return an empty tree /
 * an empty path, so a screen never has to guard its own render.
 *
 * `isError` is deliberately the OR of the four: a screen must not show a
 * silently partial layout, because "this drawer is empty" and "the container
 * list failed" would look identical to a user (see `doc/dev/web.md` on
 * `fakeApi`'s default answers for the same failure mode).
 */

export interface UseLabLayoutResult {
  /** One node per visible freezer, boxes counted per subtree. */
  readonly tree: readonly LayoutNode[];
  /** The raw lists, for the screens that need more than the tree (G3.4, G3.8). */
  readonly freezers: readonly Freezer[];
  readonly storageContainers: readonly StorageContainer[];
  readonly boxTypes: readonly BoxType[];
  readonly boxes: readonly Box[];
  /** Freezer → … → box → position, with the Qt resolver's cycle/orphan guards. */
  readonly locationPath: (boxId: string, position?: string) => LocationPath;
  readonly isPending: boolean;
  readonly isError: boolean;
  /** The first failure of the four, for `apiErrorMessage()`. */
  readonly error: unknown;
  /** Re-runs all four queries; what the error state's retry button calls. */
  readonly refetch: () => Promise<void>;
}

export function useLabLayout(labId: string): UseLabLayoutResult {
  const freezersQuery = useFreezers(labId);
  const containersQuery = useStorageContainers(labId);
  const boxTypesQuery = useBoxTypes(labId);
  const boxesQuery = useBoxes(labId);

  const freezers = freezersQuery.data?.freezers;
  const storageContainers = containersQuery.data?.containers;
  const boxTypes = boxTypesQuery.data?.boxTypes;
  const boxes = boxesQuery.data?.boxes;

  const data = useMemo<LabLayoutData>(
    () => ({
      freezers: freezers ?? [],
      storageContainers: storageContainers ?? [],
      boxTypes: boxTypes ?? [],
      boxes: boxes ?? [],
    }),
    [freezers, storageContainers, boxTypes, boxes],
  );

  const tree = useMemo(() => buildLayoutTree(data), [data]);

  const locationPath = useCallback(
    (boxId: string, position = '') => resolveLocationPath(data, boxId, position),
    [data],
  );

  const error =
    [freezersQuery.error, containersQuery.error, boxTypesQuery.error, boxesQuery.error].find(
      (candidate) => candidate !== null,
    ) ?? null;

  const { refetch: refetchFreezers } = freezersQuery;
  const { refetch: refetchContainers } = containersQuery;
  const { refetch: refetchBoxTypes } = boxTypesQuery;
  const { refetch: refetchBoxes } = boxesQuery;

  const refetch = useCallback(async () => {
    await Promise.all([
      refetchFreezers(),
      refetchContainers(),
      refetchBoxTypes(),
      refetchBoxes(),
    ]);
  }, [refetchFreezers, refetchContainers, refetchBoxTypes, refetchBoxes]);

  return {
    tree,
    freezers: data.freezers,
    storageContainers: data.storageContainers,
    boxTypes: data.boxTypes,
    boxes: data.boxes,
    locationPath,
    isPending:
      freezersQuery.isPending ||
      containersQuery.isPending ||
      boxTypesQuery.isPending ||
      boxesQuery.isPending,
    isError:
      freezersQuery.isError ||
      containersQuery.isError ||
      boxTypesQuery.isError ||
      boxesQuery.isError,
    error,
    refetch,
  };
}
