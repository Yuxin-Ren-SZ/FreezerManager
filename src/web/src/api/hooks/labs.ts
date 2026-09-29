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
  customFieldDefs: (labId: string) => ['lab', labId, 'custom-field-defs'] as const,
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

export interface UseCustomFieldDefinitionsOptions {
  /** Ask for one item type's definitions (lab-scoped ones are not included). */
  readonly itemTypeId?: string;
  /**
   * `custom-field-def/list` is gated on `sample.read` since #69 — the same
   * permission that opens the sample screens — so the caller passes its
   * permission check here (G-arch 8) and the request is never made on behalf of
   * a caller the server would refuse. The failure branch still exists for a
   * server that refuses anyway.
   */
  readonly enabled?: boolean;
}

/**
 * The lab's custom-field definitions (TODO.md G3.2 uses them as columns).
 *
 * Definitions are *metadata*, and the route that serves them is gated on
 * `sample.read` since #69 — the permission that already opens the sample
 * browser and the sample detail view that render them. The caller can still
 * switch this off: without the definitions the screen shows its base columns,
 * which is a complete screen, not a broken one.
 */
export function useCustomFieldDefinitions(
  labId: string,
  options: UseCustomFieldDefinitionsOptions = {},
) {
  const { itemTypeId, enabled = true } = options;
  return useQuery({
    queryKey: [...labKeys.customFieldDefs(labId), { itemTypeId: itemTypeId ?? '' }] as const,
    queryFn: () => call('custom-field-def/list', { labId, itemTypeId }),
    enabled: enabled && labId !== '',
  });
}
