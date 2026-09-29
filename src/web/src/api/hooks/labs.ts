// SPDX-License-Identifier: AGPL-3.0-or-later
import { useQuery } from '@tanstack/react-query';
import { call } from '../client';

/**
 * TanStack Query hooks for `LabService` and `ItemTypeService` (TODO.md G1.2).
 * Same rules as `samples.ts`: lab-scoped keys, one key per distinct query.
 */

export const labKeys = {
  all: ['labs'] as const,
  list: () => [...labKeys.all, 'list'] as const,
  detail: (labId: string) => [...labKeys.all, 'detail', labId] as const,
  itemTypes: (labId: string) => ['lab', labId, 'item-types'] as const,
};

/** Every lab the caller can see; the lab picker in G1.3 reads this. */
export function useLabs() {
  return useQuery({
    queryKey: labKeys.list(),
    queryFn: () => call('lab/list', {}),
  });
}

export function useLab(labId: string) {
  return useQuery({
    queryKey: labKeys.detail(labId),
    queryFn: () => call('lab/get', { labId }),
    enabled: labId !== '',
  });
}

export function useItemTypes(labId: string, options: { includeArchived?: boolean } = {}) {
  const includeArchived = options.includeArchived ?? false;
  return useQuery({
    queryKey: [...labKeys.itemTypes(labId), { includeArchived }] as const,
    queryFn: () => call('item-type/list', { labId, includeArchived }),
    enabled: labId !== '',
  });
}
