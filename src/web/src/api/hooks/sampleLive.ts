// SPDX-License-Identifier: AGPL-3.0-or-later
import { useQueryClient, type InfiniteData, type QueryClient } from '@tanstack/react-query';
import { useEffect, useState } from 'react';
import { SampleStatus, SampleSchema, type Sample } from '../../gen/fmgr/v1/sample_pb';
import type { ApiError } from '../errors';
import type { ResponseOf } from '../routes';
import {
  subscribeSse,
  type EventSourceLike,
  type SseParamsOf,
  type SseRetryPolicy,
} from '../sse';
import { sampleKeys, type SampleListFilters } from './samples';

/**
 * Live sample-list updates from `sample/watch` (TODO.md G3.2, F7).
 *
 * Three rules shape this hook, and the first is the one that matters most:
 *
 * 1. **A watch frame is not a sample.** The stream never carries PHI
 *    (`SampleServiceImpl::fill_sample` copies only the non-PHI custom-field
 *    blob, and `WatchSampleList` documents "PHI is never disclosed on this
 *    stream"), so a frame merges into *list* caches and the matching
 *    `sample/get` entry is **invalidated, never overwritten**: a detail view
 *    must keep asking the server, which is the only thing that can answer with
 *    PHI for a caller allowed to read it.
 * 2. **The cache key is the filter.** Each cached list carries its own filters
 *    in the key, so the merge reads them back from the key instead of being
 *    told them: a list whose filters the frame does not satisfy is left alone,
 *    and a row that stops satisfying them (moved box, new status, edited name
 *    against a search) leaves that list.
 * 3. **The stream is a hint, not the sequence.** Rows already in a page are
 *    replaced in place; a row the client has never loaded is appended only when
 *    the loaded window reaches the end of the sequence. While a
 *    `next_page_token` is outstanding, appending would put a row inside an
 *    offset window it does not belong to, and it would arrive a second time
 *    when that page is fetched.
 *
 * The feed is subscribed with the two filters it understands (`box_id`,
 * `item_type_id`); status, barcode and free text are not feed parameters, so
 * frames outside those filters arrive and are ignored by rule 2.
 */

export type SampleLiveStatus = 'connecting' | 'live' | 'error';

export interface UseSampleLiveOptions {
  readonly labId: string;
  /** Feed filter, passed to `sample/watch`. */
  readonly boxId?: string;
  /** Feed filter, passed to `sample/watch`. */
  readonly itemTypeId?: string;
  /** Off for a screen that does not want live updates (or a test). */
  readonly enabled?: boolean;
  /** Test seam, like `subscribeSse`'s. */
  readonly eventSourceFactory?: (url: string) => EventSourceLike;
  readonly reconnect?: Partial<SseRetryPolicy>;
}

export interface UseSampleLiveResult {
  readonly status: SampleLiveStatus;
  /** The last failure, for the indicator's message. `null` until one happens. */
  readonly error: ApiError | null;
}

/** A cached list of samples, as `useSamples` stores it. */
type SampleListData = InfiniteData<ResponseOf<'sample/list'>, string>;

/** `sampleKeys.list(labId, filters)` is `[lab, labId, 'samples', 'list', filters]`. */
const FILTERS_INDEX = 4;

/**
 * Mirrors the server-side filters the feed cannot apply: `contains_ci_any` for
 * the free text (`LIKE '%…%'` folded to lower case) and exact equality for the
 * rest. `barcode` is exact on the server too, not a substring.
 */
function matchesFilters(sample: Sample, filters: SampleListFilters): boolean {
  if (filters.boxId !== undefined && sample.boxId !== filters.boxId) return false;
  if (filters.itemTypeId !== undefined && sample.itemTypeId !== filters.itemTypeId) return false;
  if (filters.barcode !== undefined && sample.barcode !== filters.barcode) return false;
  if (filters.status !== undefined && sample.status !== filters.status) return false;
  if (filters.query !== undefined) {
    const needle = filters.query.toLowerCase();
    const inName = sample.name.toLowerCase().includes(needle);
    const inBarcode = (sample.barcode ?? '').toLowerCase().includes(needle);
    if (!inName && !inBarcode) return false;
  }
  return true;
}

/** Whether the frame belongs in this list at all. */
function belongsInList(sample: Sample, filters: SampleListFilters): boolean {
  if (sample.status === SampleStatus.TOMBSTONED && filters.includeArchived !== true) {
    return false;
  }
  return matchesFilters(sample, filters);
}

/**
 * One cached page set after one frame. Pure, so the rules above are testable
 * without a stream, a React tree or a query client.
 */
export function mergeSampleFrame(
  data: SampleListData,
  sample: Sample,
  filters: SampleListFilters,
): SampleListData {
  const { pages } = data;
  const lastIndex = pages.length - 1;
  const belongs = belongsInList(sample, filters);
  let found = false;

  const nextPages = pages.map((response) => {
    const position = response.samples.findIndex((candidate) => candidate.id === sample.id);
    if (position < 0) {
      return response;
    }
    found = true;

    const samples = [...response.samples];
    if (belongs) {
      samples[position] = sample;
    } else {
      // Moved out of the filter, or tombstoned in a list that hides deleted
      // rows: the row drops out rather than lying about the current filters.
      samples.splice(position, 1);
    }
    return { ...response, samples };
  });

  // See rule 3: only a row the server has already told us is the last one can be
  // appended, and only when the loaded window actually reaches the end.
  const lastPage = pages[lastIndex];
  if (
    !found &&
    belongs &&
    lastPage !== undefined &&
    (lastPage.page?.nextPageToken ?? '') === ''
  ) {
    nextPages[lastIndex] = { ...lastPage, samples: [...lastPage.samples, sample] };
  }

  return { ...data, pages: nextPages };
}

export function applySampleFrame(queryClient: QueryClient, labId: string, sample: Sample): void {
  for (const query of queryClient.getQueryCache().findAll({ queryKey: sampleKeys.lists(labId) })) {
    const filters = query.queryKey[FILTERS_INDEX] as SampleListFilters | undefined;
    const data = query.state.data as SampleListData | undefined;
    if (filters === undefined || data?.pages === undefined) continue;

    queryClient.setQueryData<SampleListData>(
      query.queryKey,
      mergeSampleFrame(data, sample, filters),
    );
  }

  // Never `setQueryData` here: the detail view's source of truth is
  // `sample/get`, which is also the only call that can disclose PHI.
  void queryClient.invalidateQueries({
    queryKey: sampleKeys.detail(labId, sample.id),
    exact: true,
  });
}

/** The feed's query parameters: only the ones the route understands. */
function feedParams(labId: string, boxId?: string, itemTypeId?: string): SseParamsOf<'sample/watch'> {
  return {
    lab_id: labId,
    ...(boxId === undefined || boxId === '' ? {} : { box_id: boxId }),
    ...(itemTypeId === undefined || itemTypeId === '' ? {} : { item_type_id: itemTypeId }),
  };
}

export function useSampleLive({
  labId,
  boxId,
  itemTypeId,
  enabled = true,
  eventSourceFactory,
  reconnect,
}: UseSampleLiveOptions): UseSampleLiveResult {
  const queryClient = useQueryClient();
  const [status, setStatus] = useState<SampleLiveStatus>('connecting');
  const [error, setError] = useState<ApiError | null>(null);

  const active = enabled && labId !== '';

  useEffect(() => {
    if (!active) {
      return;
    }

    setStatus('connecting');
    setError(null);

    return subscribeSse('sample/watch', {
      schema: SampleSchema,
      params: feedParams(labId, boxId, itemTypeId),
      onFrame: (frame) => {
        applySampleFrame(queryClient, labId, frame.data);
        // A frame arriving proves the feed works, including after a reconnect.
        setStatus('live');
      },
      onOpen: () => {
        setStatus('live');
        setError(null);
      },
      onError: (caught) => {
        setError(caught);
        setStatus('error');
      },
      ...(eventSourceFactory === undefined ? {} : { eventSourceFactory }),
      ...(reconnect === undefined ? {} : { reconnect }),
    });
  }, [active, labId, boxId, itemTypeId, queryClient, eventSourceFactory, reconnect]);

  return { status, error };
}
