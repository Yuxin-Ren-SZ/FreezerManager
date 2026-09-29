// SPDX-License-Identifier: AGPL-3.0-or-later
import { useQuery } from '@tanstack/react-query';
import { call } from '../client';

/**
 * TanStack Query hook for `BoxService.ListContainerTypes` (TODO.md G3.3).
 *
 * A sample's `container_type_id` is a core field, and its container type's
 * `size_class` is what the server checks against the destination box position
 * (`box_type_position_accepts`). The form shows the container type by name and
 * the move picker can warn before the round trip; the server still decides.
 */

export const containerTypeKeys = {
  all: (labId: string) => ['lab', labId, 'container-types'] as const,
};

export function useContainerTypes(labId: string) {
  return useQuery({
    queryKey: containerTypeKeys.all(labId),
    queryFn: () => call('container-type/list', { labId }),
    enabled: labId !== '',
  });
}
