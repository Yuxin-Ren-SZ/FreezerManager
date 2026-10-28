// SPDX-License-Identifier: AGPL-3.0-or-later
import { useQuery } from '@tanstack/react-query';
import { call } from '../client';

/**
 * TanStack Query hooks for the storage layout (`BoxService`, TODO.md G3.1).
 *
 * Same rules as `samples.ts` and `labs.ts`: every key is lab-scoped, so
 * switching labs is a cache miss rather than a stale render, and `useLabLayout`
 * (in `src/features/layout/`) composes these four into the tree the screens
 * render.
 *
 * **No `page` field is sent, on purpose.** The gateway's `ListFreezers`,
 * `ListStorageContainers`, `ListBoxTypes` and `ListBoxes` ignore `page`
 * entirely today (`src/server/BoxServiceImpl.cc` — the paged stubs are F2
 * work), and the Qt client calls them exactly the same way. The whole lab comes
 * back in one response, which is what a tree needs: a page boundary in the
 * middle of a parent chain would produce orphans. When those RPCs start paging,
 * all four hooks have to change together.
 */

export const layoutKeys = {
  all: (labId: string) => ['lab', labId, 'layout'] as const,
  freezers: (labId: string) => [...layoutKeys.all(labId), 'freezers'] as const,
  storageContainers: (labId: string) => [...layoutKeys.all(labId), 'storage-containers'] as const,
  boxTypes: (labId: string) => [...layoutKeys.all(labId), 'box-types'] as const,
  boxes: (labId: string) => [...layoutKeys.all(labId), 'boxes'] as const,
  /** Every freezer scene of one lab: what a sample-watch frame invalidates. */
  scenes: (labId: string) => [...layoutKeys.all(labId), 'scene'] as const,
  scene: (labId: string, freezerId: string) => [...layoutKeys.scenes(labId), freezerId] as const,
  models: (labId: string) => [...layoutKeys.all(labId), 'freezer-models'] as const,
};

/** Every freezer of one lab, archived ones included. */
export function useFreezers(labId: string) {
  return useQuery({
    queryKey: layoutKeys.freezers(labId),
    queryFn: () => call('freezer/list', { labId }),
    enabled: labId !== '',
  });
}

/** Every storage container of one lab, at every depth. */
export function useStorageContainers(labId: string) {
  return useQuery({
    queryKey: layoutKeys.storageContainers(labId),
    queryFn: () => call('storage-container/list', { labId }),
    enabled: labId !== '',
  });
}

/** Every box type of one lab — the position lists behind each box. */
export function useBoxTypes(labId: string) {
  return useQuery({
    queryKey: layoutKeys.boxTypes(labId),
    queryFn: () => call('box-type/list', { labId }),
    enabled: labId !== '',
  });
}

/** Every box of one lab, everywhere in the layout. */
export function useBoxes(labId: string) {
  return useQuery({
    queryKey: layoutKeys.boxes(labId),
    queryFn: () => call('box/list', { labId }),
    enabled: labId !== '',
  });
}

/**
 * One freezer drawn in 3D (ADR 0003 §3): the freezer, its model and the live
 * layout under it, parents first, with box counts but no sample data.
 *
 * Keyed under the lab, so `sample/watch` frames can invalidate every scene of
 * the lab at once (a move changes a box's count) and a lab switch is a miss.
 */
export function useFreezerScene(labId: string, freezerId: string) {
  return useQuery({
    queryKey: layoutKeys.scene(labId, freezerId),
    queryFn: () => call('freezer/scene', { freezerId }),
    enabled: labId !== '' && freezerId !== '',
  });
}

/** The built-in freezer models; `labId` is the lab the caller reads for. */
export function useFreezerModels(labId: string) {
  return useQuery({
    queryKey: layoutKeys.models(labId),
    queryFn: () => call('freezer-model/list', { labId }),
    enabled: labId !== '',
    // The catalogue ships with the server; it does not change under a session.
    staleTime: Infinity,
  });
}
