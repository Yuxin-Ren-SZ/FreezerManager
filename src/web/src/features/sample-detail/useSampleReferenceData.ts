// SPDX-License-Identifier: AGPL-3.0-or-later
import { useMemo } from 'react';
import {
  useBoxes,
  useBoxTypes,
  useContainerTypes,
  useCustomFieldDefinitions,
  useFreezers,
  useItemTypes,
  useStorageContainers,
} from '../../api/hooks';
import { useCan } from '../../app/session';
import type { Box, BoxType, ContainerType } from '../../gen/fmgr/v1/box_pb';
import type { CustomFieldDefinition, ItemType } from '../../gen/fmgr/v1/item_type_pb';
import { resolveLocationPath, type LabLayoutData, type LocationPath } from '../layout/layoutModel';

/**
 * Everything a sample screen needs that is not the sample itself (TODO.md G3.3):
 * item types, custom-field definitions, container types and the storage layout.
 *
 * It composes the service hooks rather than replacing them, so TanStack Query
 * still deduplicates: the detail view and the form in the same page share one
 * request per resource, and the two screens share with G3.1's layout tree.
 *
 * `definitionsReadable` is a permission, not a failure: the server's
 * `ListCustomFieldDefinitions` authorizes on **`sample.read`** since #69, the
 * same permission that opens this screen — a Member holds it, and
 * `custom_field.define` they do not. The enforcement point is
 * `ItemTypeServiceImpl::ListCustomFieldDefinitions` in
 * `src/server/ItemTypeServiceImpl.cc` (the `middleware_.authorize(...,
 * core::Permission::SampleRead, lab_id)` call), and the `AuthMiddleware`
 * registry entry for that RPC names the same permission. Only the *read* moved:
 * Create/Update/Archive of a definition still requires `custom_field.define`.
 *
 * This matters because the gate has to follow the enforcement point, not the
 * other way round. This comment previously stated as a fact about the server
 * that the RPC authorized on `custom_field.define`, and this hook gated on it;
 * that was true before #69 and is false now, and a reader who trusts it would
 * "fix" the gate back to matching. Do not restore `custom_field.define` here.
 *
 * G3.2's `useCustomFieldDefinitions` in `src/api/hooks/labs.ts` takes the gate
 * as an option, so the SPA never fires a request it knows will 403 — and the
 * form can say *why* it is not showing custom fields instead of rendering a
 * form that silently omits them. The lab-wide call is deliberate: an
 * `item_type_id` would get that node's definitions alone, with no ancestor
 * merge.
 */

export interface SampleReferenceData {
  readonly itemTypes: readonly ItemType[];
  readonly cfds: readonly CustomFieldDefinition[];
  readonly containerTypes: readonly ContainerType[];
  readonly boxes: readonly Box[];
  readonly boxTypes: readonly BoxType[];
  /** Freezer → … → box → position for this lab. */
  readonly locationPath: (boxId: string, position?: string) => LocationPath;
  /** Whether the caller may read custom-field definitions at all. */
  readonly definitionsReadable: boolean;
  readonly isPending: boolean;
  readonly isError: boolean;
  readonly error: unknown;
  readonly refetch: () => Promise<void>;
}

export function useSampleReferenceData(labId: string): SampleReferenceData {
  // `sample.read`, not `custom_field.define`: it is what the server enforces on
  // `ListCustomFieldDefinitions` since #69. See the module comment.
  const definitionsReadable = useCan('sample.read', labId);

  const itemTypesQuery = useItemTypes(labId);
  const cfdsQuery = useCustomFieldDefinitions(labId, { enabled: definitionsReadable });
  const containerTypesQuery = useContainerTypes(labId);

  const freezersQuery = useFreezers(labId);
  const containersQuery = useStorageContainers(labId);
  const boxTypesQuery = useBoxTypes(labId);
  const boxesQuery = useBoxes(labId);

  const itemTypes = itemTypesQuery.data?.itemTypes ?? [];
  const cfds = cfdsQuery.data?.cfds ?? [];
  const containerTypes = containerTypesQuery.data?.containerTypes ?? [];

  // Built from the query results inside one `useMemo`: `?? []` allocates a new
  // array on every render, which would make the memo below recompute (and its
  // dependency list change) for no reason.
  const layout = useMemo<LabLayoutData>(
    () => ({
      freezers: freezersQuery.data?.freezers ?? [],
      storageContainers: containersQuery.data?.containers ?? [],
      boxTypes: boxTypesQuery.data?.boxTypes ?? [],
      boxes: boxesQuery.data?.boxes ?? [],
    }),
    [freezersQuery.data, containersQuery.data, boxTypesQuery.data, boxesQuery.data],
  );

  const queries = [
    itemTypesQuery,
    containerTypesQuery,
    freezersQuery,
    containersQuery,
    boxTypesQuery,
    boxesQuery,
    // Only a query that actually ran can be pending or failed; a disabled
    // definitions query is `pending` forever and must not hold the screen.
    ...(definitionsReadable ? [cfdsQuery] : []),
  ];

  return {
    itemTypes,
    cfds,
    containerTypes,
    boxes: layout.boxes,
    boxTypes: layout.boxTypes,
    locationPath: (boxId, position = '') => resolveLocationPath(layout, boxId, position),
    definitionsReadable,
    isPending: queries.some((query) => query.isPending),
    isError: queries.some((query) => query.isError),
    error: queries.map((query) => query.error).find((candidate) => candidate !== null) ?? null,
    refetch: async () => {
      await Promise.all(queries.map((query) => query.refetch()));
    },
  };
}
