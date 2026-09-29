// SPDX-License-Identifier: AGPL-3.0-or-later
import { useQuery } from '@tanstack/react-query';
import { call } from '../client';

/**
 * TanStack Query hook for `AuditService.ListAuditEvents` (TODO.md G3.3).
 *
 * The filter is the contract: `entity_kind` + `entity_id` is what makes this
 * "this sample's history" rather than "the lab's audit log". The caller passes
 * both, and the query stays disabled without them so a detail screen that has
 * not resolved its sample id yet cannot fetch the whole log by accident.
 *
 * History is only fetched when the caller holds `audit.read` (the acceptance
 * criterion is "no section at all", not an empty one), so `enabled` is part of
 * the hook's public shape rather than something the screen bolts on afterwards.
 */

export const auditKeys = {
  all: (labId: string) => ['lab', labId, 'audit'] as const,
  entity: (labId: string, entityKind: string, entityId: string) =>
    [...auditKeys.all(labId), entityKind, entityId] as const,
};

export interface UseAuditEventsOptions {
  readonly labId: string;
  readonly entityKind: string;
  readonly entityId: string;
  readonly enabled?: boolean;
}

/** One entity's audit events, newest page first as the server orders them. */
export function useAuditEvents({
  labId,
  entityKind,
  entityId,
  enabled = true,
}: UseAuditEventsOptions) {
  return useQuery({
    queryKey: auditKeys.entity(labId, entityKind, entityId),
    queryFn: () => call('audit/list', { labId, entityKind, entityId }),
    enabled: enabled && labId !== '' && entityId !== '',
  });
}
