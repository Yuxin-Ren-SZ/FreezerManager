// SPDX-License-Identifier: AGPL-3.0-or-later
import { useInfiniteQuery, useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { call } from '../client';
import type { RequestInitOf } from '../routes';

/**
 * TanStack Query hooks for `SampleService` (TODO.md G1.2, G-arch 1/5).
 *
 * Two rules from the issue are visible in every key below:
 *
 *   - **every key includes `lab_id`.** Two labs must never share a cache entry,
 *     and switching labs must be a cache miss rather than a stale render;
 *   - **mutations invalidate the matching keys** rather than hand-patching the
 *     cache. A write invalidates `sampleKeys.all(labId)`, which covers the
 *     lists and every detail of that lab and nothing else.
 */

/** Filters that are part of the query key, so a change refetches. */
export interface SampleListFilters {
  readonly boxId?: string;
  readonly itemTypeId?: string;
  readonly barcode?: string;
  readonly includeArchived?: boolean;
}

export const sampleKeys = {
  all: (labId: string) => ['lab', labId, 'samples'] as const,
  lists: (labId: string) => [...sampleKeys.all(labId), 'list'] as const,
  /** Filters are in the key: a different filter is a different page set. */
  list: (labId: string, filters: SampleListFilters) =>
    [...sampleKeys.lists(labId), filters] as const,
  details: (labId: string) => [...sampleKeys.all(labId), 'detail'] as const,
  detail: (labId: string, sampleId: string) => [...sampleKeys.details(labId), sampleId] as const,
};

export interface UseSamplesOptions extends SampleListFilters {
  readonly labId: string;
  /** Server default is 100 when omitted. */
  readonly pageSize?: number;
  readonly enabled?: boolean;
}

/**
 * One lab's samples, paged through the server's opaque `page_token`.
 *
 * `useInfiniteQuery` (not a hand-rolled page counter) is what makes the table
 * and the virtualised grid in G3.2 work without loading the whole lab.
 */
export function useSamples({ labId, pageSize, enabled = true, ...filters }: UseSamplesOptions) {
  return useInfiniteQuery({
    queryKey: sampleKeys.list(labId, filters),
    queryFn: ({ pageParam }) =>
      call('sample/list', {
        labId,
        page: { pageSize: pageSize ?? 0, pageToken: pageParam },
        includeArchived: filters.includeArchived ?? false,
        boxId: filters.boxId,
        itemTypeId: filters.itemTypeId,
        barcode: filters.barcode,
      }),
    initialPageParam: '',
    // `undefined` (not `''`) ends the sequence: TanStack treats any non-null
    // value as "there is another page", so an empty token would keep
    // `hasNextPage` true and the grid would fetch an empty page forever.
    getNextPageParam: (lastPage) => {
      const token = lastPage.page?.nextPageToken ?? '';
      return token === '' ? undefined : token;
    },
    enabled: enabled && labId !== '',
  });
}

/** One sample. Disabled without an id, so a "new sample" route can reuse it. */
export function useSample(labId: string, sampleId: string) {
  return useQuery({
    queryKey: sampleKeys.detail(labId, sampleId),
    queryFn: () => call('sample/get', { sampleId }),
    enabled: labId !== '' && sampleId !== '',
  });
}

/** Invalidate everything about one lab's samples after a write. */
function useInvalidateSamples(labId: string) {
  const queryClient = useQueryClient();
  return () => queryClient.invalidateQueries({ queryKey: sampleKeys.all(labId) });
}

export function useCreateSample(labId: string) {
  const invalidate = useInvalidateSamples(labId);
  return useMutation({
    mutationFn: (request: RequestInitOf<'sample/create'>) => call('sample/create', request),
    onSuccess: () => invalidate(),
  });
}

export function useUpdateSample(labId: string) {
  const invalidate = useInvalidateSamples(labId);
  return useMutation({
    mutationFn: (request: RequestInitOf<'sample/update'>) => call('sample/update', request),
    onSuccess: () => invalidate(),
  });
}

export function useMoveSample(labId: string) {
  const invalidate = useInvalidateSamples(labId);
  return useMutation({
    mutationFn: (request: RequestInitOf<'sample/move'>) => call('sample/move', request),
    onSuccess: () => invalidate(),
  });
}

export function useSoftDeleteSample(labId: string) {
  const invalidate = useInvalidateSamples(labId);
  return useMutation({
    mutationFn: (request: RequestInitOf<'sample/delete'>) => call('sample/delete', request),
    onSuccess: () => invalidate(),
  });
}

export function useCheckoutSample(labId: string) {
  const invalidate = useInvalidateSamples(labId);
  return useMutation({
    mutationFn: (request: RequestInitOf<'sample/checkout'>) => call('sample/checkout', request),
    onSuccess: () => invalidate(),
  });
}
