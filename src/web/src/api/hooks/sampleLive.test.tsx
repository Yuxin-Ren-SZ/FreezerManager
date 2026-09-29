// SPDX-License-Identifier: AGPL-3.0-or-later
import { create, toJson } from '@bufbuild/protobuf';
import { QueryClient } from '@tanstack/react-query';
import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ListSamplesResponseSchema, SampleSchema, SampleStatus } from '../../gen/fmgr/v1/sample_pb';
import type { Sample } from '../../gen/fmgr/v1/sample_pb';
import { FakeEventSource, fakeEventSource } from '../../test/fakeEventSource';
import { createWrapper } from '../../test/render';
import { sampleKeys } from './samples';
import { useSampleLive } from './sampleLive';

/**
 * Live updates from `sample/watch` (TODO.md G3.2, F7).
 *
 * The PHI rule is the reason this file is thorough: a watch frame is **not** a
 * sample detail. It never carries PHI (the server says so, and
 * `SampleServiceImpl::fill_sample` only copies the non-PHI custom-field blob),
 * so a frame merges into *list* caches and the detail cache is **invalidated,
 * never overwritten** — otherwise a stream frame would silently become the
 * source of truth for a detail view and a PHI field the detail had disclosed
 * would vanish, or a non-PHI frame would be presented as the whole record.
 */

const LAB_ID = 'lab-demo';

/** One `Sample` frame, as the gateway serialises it (proto field names). */
function frame(seed: Partial<Sample> & { id: string }): string {
  return JSON.stringify(
    toJson(
      SampleSchema,
      create(SampleSchema, { labId: LAB_ID, status: SampleStatus.ACTIVE, ...seed }),
      { useProtoFieldName: true },
    ),
  );
}

/** A cached `sample/list` page. `nextPageToken` non-empty means "more to come". */
function page(ids: string[], nextPageToken = '') {
  return create(ListSamplesResponseSchema, {
    samples: ids.map((id) =>
      create(SampleSchema, { id, labId: LAB_ID, name: `Sample ${id}`, status: SampleStatus.ACTIVE }),
    ),
    page: { nextPageToken },
  });
}

function cachedIds(client: QueryClient, filters = {}): string[] {
  const data = client.getQueryData<{ pages: { samples: Sample[] }[] }>(
    sampleKeys.list(LAB_ID, filters),
  );
  return (data?.pages ?? []).flatMap((p) => p.samples.map((sample) => sample.id));
}

function cachedSample(
  client: QueryClient,
  id: string,
  filters = {},
): Sample | undefined {
  const data = client.getQueryData<{ pages: { samples: Sample[] }[] }>(
    sampleKeys.list(LAB_ID, filters),
  );
  return (data?.pages ?? []).flatMap((p) => p.samples).find((sample) => sample.id === id);
}

/**
 * `createTestQueryClient()` uses `gcTime: 0` (G1.2: nothing leaks between
 * tests), which drops a query the moment its last observer goes away. These
 * tests seed caches by hand and then assert on them, so they need the data to
 * stay put; the hook itself is always observed by the render below.
 */
function createSeededQueryClient(): QueryClient {
  return new QueryClient({ defaultOptions: { queries: { retry: false, gcTime: Infinity } } });
}

let client: QueryClient;

beforeEach(() => {
  FakeEventSource.reset();
  client = createSeededQueryClient();
});

afterEach(() => {
  vi.useRealTimers();
});

function renderLive(
  options: Partial<Parameters<typeof useSampleLive>[0]> = {},
  queryClient = client,
) {
  return renderHook(
    () =>
      useSampleLive({
        labId: LAB_ID,
        eventSourceFactory: fakeEventSource,
        ...options,
      }),
    { wrapper: createWrapper({ queryClient }) },
  );
}

/** Push one frame through the open stream and let React commit the update. */
async function push(payload: string, lastEventId = ''): Promise<void> {
  await act(async () => {
    FakeEventSource.current().message(payload, lastEventId);
  });
}

describe('useSampleLive', () => {
  it('subscribes to the lab feed, scoped by the filters the feed understands', () => {
    renderLive({ boxId: 'box-1', itemTypeId: 'it-serum' });

    expect(FakeEventSource.current().url).toBe(
      '/api/v1/sample/watch?lab_id=lab-demo&box_id=box-1&item_type_id=it-serum',
    );
  });

  it('does not open a stream without a lab', () => {
    renderLive({ labId: '', enabled: false });

    expect(FakeEventSource.all()).toHaveLength(0);
  });

  it('replaces a changed row in the list cache', async () => {
    client.setQueryData(sampleKeys.list(LAB_ID, {}), { pages: [page(['sample-1', 'sample-2'])], pageParams: [''] });
    renderLive();

    await push(frame({ id: 'sample-1', name: 'Serum A (edited)' }));

    expect(cachedSample(client, 'sample-1')?.name).toBe('Serum A (edited)');
    // The other row and the page's token are untouched: a frame updates one row.
    expect(cachedIds(client)).toEqual(['sample-1', 'sample-2']);
  });

  it('drops a tombstoned row out of the list cache', async () => {
    client.setQueryData(sampleKeys.list(LAB_ID, {}), { pages: [page(['sample-1', 'sample-2'])], pageParams: [''] });
    renderLive();

    await push(frame({ id: 'sample-1', status: SampleStatus.TOMBSTONED }));

    expect(cachedIds(client)).toEqual(['sample-2']);
  });

  it('keeps a tombstoned row in a list that asked for archived rows', async () => {
    const filters = { includeArchived: true };
    client.setQueryData(sampleKeys.list(LAB_ID, filters), {
      pages: [page(['sample-1', 'sample-2'])],
      pageParams: [''],
    });
    renderLive();

    await push(frame({ id: 'sample-1', status: SampleStatus.TOMBSTONED }));

    // `include_archived` lists *show* deleted rows, so removing one would be a
    // lie in the other direction.
    expect(cachedIds(client, filters)).toEqual(['sample-1', 'sample-2']);
    expect(cachedSample(client, 'sample-1', filters)?.status).toBe(SampleStatus.TOMBSTONED);
  });

  it('appends a new row when the loaded window reaches the end of the list', async () => {
    client.setQueryData(sampleKeys.list(LAB_ID, {}), { pages: [page(['sample-1'])], pageParams: [''] });
    renderLive();

    await push(frame({ id: 'sample-9', name: 'Serum Z' }));

    expect(cachedIds(client)).toEqual(['sample-1', 'sample-9']);
  });

  it('does not append a new row while more pages remain, so offsets stay aligned', async () => {
    // A non-empty token means the server has rows this client has not loaded.
    // Inserting one at the end of the loaded window would duplicate it when the
    // next page arrives (the cursor is an offset), and the row would be shown
    // out of order. It appears when the user pages to where it lives.
    client.setQueryData(sampleKeys.list(LAB_ID, {}), {
      pages: [page(['sample-1'], '1')],
      pageParams: [''],
    });
    renderLive();

    await push(frame({ id: 'sample-9', name: 'Serum Z' }));

    expect(cachedIds(client)).toEqual(['sample-1']);
  });

  it('leaves a list whose filters the frame does not satisfy alone', async () => {
    const filters = { boxId: 'box-2', status: SampleStatus.CHECKED_OUT };
    client.setQueryData(sampleKeys.list(LAB_ID, filters), {
      pages: [page(['sample-1'])],
      pageParams: [''],
    });
    renderLive();

    // The feed cannot filter by status, so a frame for an active sample in
    // another box reaches the hook — and must not enter this cache.
    await push(frame({ id: 'sample-1', boxId: 'box-1', status: SampleStatus.ACTIVE }));

    expect(cachedSample(client, 'sample-1', filters)?.boxId).toBeUndefined();
  });

  it('removes a row that a frame moved out of the list it was in', async () => {
    const filters = { boxId: 'box-1' };
    client.setQueryData(sampleKeys.list(LAB_ID, filters), {
      pages: [page(['sample-1', 'sample-2'])],
      pageParams: [''],
    });
    renderLive();

    await push(frame({ id: 'sample-1', boxId: 'box-2' }));

    expect(cachedIds(client, filters)).toEqual(['sample-2']);
  });

  it('matches a free-text filter case-insensitively, as the server search does', async () => {
    const filters = { query: 'serum' };
    client.setQueryData(sampleKeys.list(LAB_ID, filters), {
      pages: [page(['sample-1'])],
      pageParams: [''],
    });
    renderLive();

    await push(frame({ id: 'sample-1', name: 'SERUM A' }));
    expect(cachedSample(client, 'sample-1', filters)?.name).toBe('SERUM A');

    await push(frame({ id: 'sample-1', name: 'Plasma A' }));
    expect(cachedIds(client, filters)).toEqual([]);
  });

  it('invalidates the detail entry instead of overwriting it', async () => {
    const detailKey = sampleKeys.detail(LAB_ID, 'sample-1');
    const detail = create(SampleSchema, {
      id: 'sample-1',
      labId: LAB_ID,
      name: 'Serum A',
      status: SampleStatus.ACTIVE,
      customFieldsJson: JSON.stringify({ phi_note: 'disclosed by sample/get only' }),
    });
    client.setQueryData(detailKey, { sample: detail });
    renderLive();

    await push(frame({ id: 'sample-1', name: 'Serum A (edited)' }));

    // Not overwritten: the detail cache still holds exactly what `sample/get`
    // returned. A watch frame that carried fewer fields — no PHI, ever — would
    // otherwise replace a complete record with a partial one.
    expect(client.getQueryData(detailKey)).toEqual({ sample: detail });
    // And invalidated, so the next detail render asks the server (G-arch 7:
    // the detail view's source of truth is `sample/get`).
    expect(client.getQueryState(detailKey)?.isInvalidated).toBe(true);
  });

  it('reports live, then error, and reconnects carrying the last event id', async () => {
    vi.useFakeTimers();
    const { result } = renderLive();

    expect(result.current.status).toBe('connecting');
    act(() => {
      FakeEventSource.current().open();
    });
    expect(result.current.status).toBe('live');

    await act(async () => {
      FakeEventSource.current().message(frame({ id: 'sample-1' }), '1758931200000000');
      FakeEventSource.current().transportError();
    });
    expect(result.current.status).toBe('error');

    // G1.2's SSE wrapper owns the backoff; the hook's job is to keep merging
    // after it. The cursor travels as `since`, so the gap is not lost.
    await act(async () => {
      await vi.advanceTimersByTimeAsync(1_000);
    });
    expect(FakeEventSource.all()).toHaveLength(2);
    expect(FakeEventSource.current().url).toContain('since=1758931200000000');

    client.setQueryData(sampleKeys.list(LAB_ID, {}), { pages: [page(['sample-1'])], pageParams: [''] });
    act(() => {
      FakeEventSource.current().open();
    });
    await push(frame({ id: 'sample-1', name: 'After reconnect' }));

    expect(cachedSample(client, 'sample-1')?.name).toBe('After reconnect');
    expect(result.current.status).toBe('live');
  });

  it('stops on PERMISSION_DENIED instead of reconnecting into a refusal', async () => {
    vi.useFakeTimers();
    const { result } = renderLive();
    act(() => {
      FakeEventSource.current().open();
    });

    await act(async () => {
      FakeEventSource.current().serverError({ code: 'PERMISSION_DENIED', message: 'nope' });
      await vi.advanceTimersByTimeAsync(60_000);
    });

    expect(result.current.status).toBe('error');
    expect(result.current.error?.code).toBe('PERMISSION_DENIED');
    expect(FakeEventSource.all()).toHaveLength(1);
  });

  it('survives a malformed frame without dropping the subscription', async () => {
    const { result } = renderLive();
    act(() => {
      FakeEventSource.current().open();
    });

    await push('{not json');

    expect(result.current.status).toBe('error');
    expect(FakeEventSource.current().closed).toBe(false);
  });

  it('closes the stream and stops merging on unmount', async () => {
    client.setQueryData(sampleKeys.list(LAB_ID, {}), { pages: [page(['sample-1'])], pageParams: [''] });
    const { unmount } = renderLive();
    act(() => {
      FakeEventSource.current().open();
    });

    unmount();

    expect(FakeEventSource.current().closed).toBe(true);
    await push(frame({ id: 'sample-1', name: 'Too late' }));
    expect(cachedSample(client, 'sample-1')?.name).toBe('Sample sample-1');
  });
});
