// SPDX-License-Identifier: AGPL-3.0-or-later
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { call } from '../../api/client';
import { labKeys } from '../../api/hooks';
import type { RequestInitOf } from '../../api/routes';

/**
 * The four writes the item-type admin makes (TODO.md G3.9).
 *
 * A feature hook rather than an entry in `src/api/hooks/labs.ts`, for the same
 * two reasons G3.1's `useLabLayout` is one: the read hooks it needs already
 * exist there, and a screen's mutations are only ever used by that screen — so
 * keeping them in the feature directory is what lets a task own its own files.
 *
 * **Invalidation, not cache patching.** Both writes invalidate the item types
 * *and* the definitions of this lab: a re-parent moves which definitions a
 * whole subtree inherits, so a cache update that only touched the moved row
 * would leave the field lists of every descendant wrong.
 */

export function useItemTypeAdmin(labId: string) {
  const client = useQueryClient();

  const invalidate = () => {
    void client.invalidateQueries({ queryKey: labKeys.itemTypes(labId) });
    void client.invalidateQueries({ queryKey: labKeys.customFieldDefs(labId) });
  };

  const createItemType = useMutation({
    mutationFn: (request: RequestInitOf<'item-type/create'>) => call('item-type/create', request),
    onSuccess: invalidate,
  });

  const updateItemType = useMutation({
    mutationFn: (request: RequestInitOf<'item-type/update'>) => call('item-type/update', request),
    onSuccess: invalidate,
  });

  const createDefinition = useMutation({
    mutationFn: (request: RequestInitOf<'custom-field-def/create'>) =>
      call('custom-field-def/create', request),
    onSuccess: invalidate,
  });

  const updateDefinition = useMutation({
    mutationFn: (request: RequestInitOf<'custom-field-def/update'>) =>
      call('custom-field-def/update', request),
    onSuccess: invalidate,
  });

  return { createItemType, updateItemType, createDefinition, updateDefinition, invalidate };
}
