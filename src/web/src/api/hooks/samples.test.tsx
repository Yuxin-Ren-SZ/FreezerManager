// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '../errors';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { createTestQueryClient, createWrapper } from '../../test/render';
import { server } from '../../test/server';
import {
  sampleKeys,
  useCheckoutSample,
  useCreateSample,
  useSample,
  useSamples,
  useSoftDeleteSample,
} from './samples';

/**
 * The query layer (TODO.md G1.2). The two contractual bits are that the list is
 * a `useInfiniteQuery` over `page_token` and that every key includes `lab_id`;
 * the rest is the error handling G-arch 8 requires of every screen.
 */

let lab: DemoLab;

beforeEach(() => {
  lab = createDemoLab();
  server.use(...fakeApi({ lab }));
});

describe('sampleKeys', () => {
  it('includes lab_id in every key, so two labs can never share a cache entry', () => {
    const keys = [
      sampleKeys.all('lab-1'),
      sampleKeys.lists('lab-1'),
      sampleKeys.list('lab-1', {}),
      sampleKeys.details('lab-1'),
      sampleKeys.detail('lab-1', 'sample-1'),
    ];

    for (const key of keys) {
      expect(key).toContain('lab-1');
    }
    expect(sampleKeys.list('lab-1', {})).not.toEqual(sampleKeys.list('lab-2', {}));
  });
});

describe('useSamples', () => {
  it('loads the first page with the requested page size', async () => {
    const { result } = renderHook(() => useSamples({ labId: 'lab-demo', pageSize: 2 }), {
      wrapper: createWrapper(),
    });

    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });

    const firstPage = result.current.data?.pages[0];
    expect(firstPage?.samples).toHaveLength(2);
    // `ListSamples` returns only `next_page_token`; `total_count` is never set
    // by the server, so a screen must not render one (G3.2, fakeApi contract).
    expect(firstPage?.page?.totalCount).toBe(0);
    expect(result.current.hasNextPage).toBe(true);
  });

  it('fetches the next page with the page_token the server returned', async () => {
    const { result } = renderHook(() => useSamples({ labId: 'lab-demo', pageSize: 2 }), {
      wrapper: createWrapper(),
    });
    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });

    // `fetchNextPage` commits its state update on a later tick than the promise
    // it returns, so the interaction is driven from inside `waitFor` and the
    // assertion retried with it.
    await waitFor(async () => {
      if ((result.current.data?.pages.length ?? 0) < 2) {
        await result.current.fetchNextPage();
      }
      expect(result.current.data?.pages).toHaveLength(2);
    });
    expect(result.current.data?.pages[1]?.samples.map((sample) => sample.id)).toEqual(['sample-3']);
    expect(result.current.hasNextPage).toBe(false);
  });

  it('exposes a permission failure as a typed ApiError for the screen to handle', async () => {
    server.use(...fakeApi({ fail: { 'sample/list': 'PERMISSION_DENIED' } }));

    const { result } = renderHook(() => useSamples({ labId: 'lab-demo' }), {
      wrapper: createWrapper(),
    });

    await waitFor(() => {
      expect(result.current.isError).toBe(true);
    });
    expect(result.current.error).toBeInstanceOf(ApiError);
    expect((result.current.error as ApiError).code).toBe('PERMISSION_DENIED');
  });

  it('does not run while disabled, which is how a screen waits for a lab id', () => {
    const { result } = renderHook(() => useSamples({ labId: 'lab-demo', enabled: false }), {
      wrapper: createWrapper(),
    });

    expect(result.current.fetchStatus).toBe('idle');
  });
});

describe('useSample', () => {
  it('loads one sample by id', async () => {
    const { result } = renderHook(() => useSample('lab-demo', 'sample-1'), {
      wrapper: createWrapper(),
    });

    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });
    expect(result.current.data?.sample?.name).toBe('Serum A');
  });

  it('reports NOT_FOUND for a sample that is gone', async () => {
    const { result } = renderHook(() => useSample('lab-demo', 'sample-gone'), {
      wrapper: createWrapper(),
    });

    await waitFor(() => {
      expect(result.current.isError).toBe(true);
    });
    expect((result.current.error as ApiError).code).toBe('NOT_FOUND');
  });

  it('does not fetch without a sample id', () => {
    const { result } = renderHook(() => useSample('lab-demo', ''), { wrapper: createWrapper() });

    expect(result.current.fetchStatus).toBe('idle');
  });
});

describe('mutations', () => {
  it('invalidates the lab-scoped sample keys after a create', async () => {
    const queryClient = createTestQueryClient();
    const wrapper = createWrapper({ queryClient });
    const invalidate = vi.spyOn(queryClient, 'invalidateQueries');

    const list = renderHook(() => useSamples({ labId: 'lab-demo' }), { wrapper });
    await waitFor(() => {
      expect(list.result.current.isSuccess).toBe(true);
    });
    invalidate.mockClear();

    const create = renderHook(() => useCreateSample('lab-demo'), { wrapper });
    await act(async () => {
      await create.result.current.mutateAsync({
        labId: 'lab-demo',
        itemTypeId: 'it-serum',
        name: 'Serum C',
      });
    });

    // The contract is *which keys* a write invalidates: everything about this
    // lab's samples (so the lists refetch and every cached detail is dropped)
    // and nothing belonging to another lab.
    expect(invalidate).toHaveBeenCalledTimes(1);
    expect(invalidate).toHaveBeenCalledWith({ queryKey: sampleKeys.all('lab-demo') });
    expect(sampleKeys.all('lab-demo')).not.toEqual(sampleKeys.all('lab-other'));
    expect(lab.samples.map((sample) => sample.name)).toContain('Serum C');
  });

  it('surfaces a conflict from a delete as ALREADY_EXISTS', async () => {
    server.use(...fakeApi({ fail: { 'sample/delete': 'ALREADY_EXISTS' } }));

    const { result } = renderHook(() => useSoftDeleteSample('lab-demo'), {
      wrapper: createWrapper(),
    });

    await act(async () => {
      await expect(result.current.mutateAsync({ sampleId: 'sample-1' })).rejects.toBeInstanceOf(
        ApiError,
      );
    });

    await waitFor(() => {
      expect((result.current.error as ApiError).code).toBe('ALREADY_EXISTS');
    });
  });

  it('turns a forbidden checkout into FAILED_PRECONDITION on the mutation', async () => {
    const { result } = renderHook(() => useCheckoutSample('lab-demo'), {
      wrapper: createWrapper(),
    });

    // sample-3 is already checked out in the demo lab.
    await act(async () => {
      await result.current.mutateAsync({ sampleId: 'sample-3' }).catch(() => undefined);
    });

    await waitFor(() => {
      expect((result.current.error as ApiError).code).toBe('FAILED_PRECONDITION');
    });
  });
});
