// SPDX-License-Identifier: AGPL-3.0-or-later
import { renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it } from 'vitest';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { createWrapper } from '../../test/render';
import { server } from '../../test/server';
import { useAuditEvents } from './audit';
import { useContainerTypes } from './containers';

/**
 * The two reference-data hooks G3.3 added: container types, and one entity's
 * audit events.
 *
 * Item types and custom-field definitions are G3.2's hooks in `labs.ts`, tested
 * there, so this file does not restate them. The properties worth pinning here
 * are that each key is lab-scoped (two labs must never share a cache entry) and
 * that `audit/list` really carries the `entity_kind`/`entity_id` filter — a
 * history section that quietly showed the whole lab's audit log would look
 * identical in a screenshot.
 */

let lab: DemoLab;

beforeEach(() => {
  lab = createDemoLab();
  server.use(...fakeApi({ lab }));
});

describe('useContainerTypes', () => {
  it('loads the lab container types with their size class', async () => {
    const { result } = renderHook(() => useContainerTypes('lab-demo'), {
      wrapper: createWrapper(),
    });

    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });

    const tube = result.current.data?.containerTypes.find((item) => item.id === 'ct-15ml');
    expect(tube?.sizeClass).toBe('tube-15');
  });
});

describe('useAuditEvents', () => {
  it('asks only for this entity, in this lab', async () => {
    const { result } = renderHook(
      () => useAuditEvents({ labId: 'lab-demo', entityKind: 'sample', entityId: 'sample-1' }),
      { wrapper: createWrapper() },
    );

    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });

    const ids = result.current.data?.events.map((event) => event.id);
    // sample-2's row and lab-second's row are both in the fake and neither may
    // appear here.
    expect(ids).toEqual(['audit-1', 'audit-2', 'audit-3']);
  });

  it('stays disabled when it is not given an entity', () => {
    const { result } = renderHook(
      () => useAuditEvents({ labId: 'lab-demo', entityKind: 'sample', entityId: '' }),
      { wrapper: createWrapper() },
    );

    expect(result.current.fetchStatus).toBe('idle');
  });
});
