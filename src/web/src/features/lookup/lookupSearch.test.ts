// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { beforeEach, describe, expect, it } from 'vitest';
import { sampleKeys } from '../../api/hooks';
import { SampleSchema, SampleStatus } from '../../gen/fmgr/v1/sample_pb';
import { createDemoLab, fakeApi, seedSamples, type DemoLab } from '../../test/fakeApi';
import { server } from '../../test/server';
import { LOOKUP_PAGE_SIZE, lookupKeys, searchSamples } from './lookupSearch';

/**
 * The lookup's search contract (TODO.md G3.5, PRD §9).
 *
 * This is the part of the screen that has to be *deliberate* rather than
 * convenient, so it is tested at the transport level instead of through the
 * rendered card: what the screen shows for a given answer is G3.5's business,
 * but *which question it asks* is the difference between a scan that resolves
 * to the right sample and one that resolves to a name that happens to contain
 * the same characters.
 *
 * Two rules are contractual:
 *
 *  1. **An exact barcode match is tried first, alone.** If it hits, there is no
 *     second request and no free-text search to dilute it.
 *  2. **The free-text fallback is only reached when the barcode search came
 *     back empty** — never when it failed, and never with a term the server
 *     would refuse (`SampleServiceImpl::k_min_query_length`).
 */

let lab: DemoLab;

/** The `sample/list` requests this file caused, cloned before MSW consumed them. */
let listRequests: Request[] = [];

server.events.on('request:start', ({ request }) => {
  if (new URL(request.url).pathname === '/api/v1/sample/list') {
    listRequests.push(request.clone());
  }
});

/** The decoded bodies, in the order the requests arrived. */
async function listBodies(): Promise<Record<string, unknown>[]> {
  return Promise.all(
    listRequests.map(async (request) => (await request.json()) as Record<string, unknown>),
  );
}

function idsOf(outcome: Awaited<ReturnType<typeof searchSamples>>): string[] {
  return outcome.kind === 'none' ? [] : outcome.samples.map((sample) => sample.id);
}

beforeEach(() => {
  lab = createDemoLab();
  listRequests = [];
  server.use(...fakeApi({ lab }));
});

describe('lookupKeys', () => {
  it("nests under the lab's sample keys, so a check-out invalidation reaches a cached lookup", () => {
    // `useCheckoutSample` invalidates `sampleKeys.all(labId)`; a lookup key
    // outside that prefix would keep showing the pre-check-out status forever.
    expect(lookupKeys.search('lab-1', 'DEMO-0001').slice(0, 3)).toEqual([
      ...sampleKeys.all('lab-1'),
    ]);
  });

  it('separates labs and terms, so two of either can never share an entry', () => {
    expect(lookupKeys.search('lab-1', 'DEMO-0001')).not.toEqual(
      lookupKeys.search('lab-2', 'DEMO-0001'),
    );
    expect(lookupKeys.search('lab-1', 'DEMO-0001')).not.toEqual(
      lookupKeys.search('lab-1', 'DEMO-0002'),
    );
  });
});

describe('searchSamples', () => {
  it('answers an exact barcode with one request and never a free-text search', async () => {
    // A sample whose *name* also contains the term: if the implementation
    // searched free text first (or as well), this test would see two hits.
    lab.samples.push(
      create(SampleSchema, {
        id: 'sample-named-like-a-barcode',
        labId: 'lab-demo',
        itemTypeId: 'it-serum',
        name: 'DEMO-0001 control',
        barcode: 'DEMO-9999',
      }),
    );

    const outcome = await searchSamples('lab-demo', 'DEMO-0001');

    expect(outcome.kind).toBe('barcode');
    expect(idsOf(outcome)).toEqual(['sample-1']);

    const bodies = await listBodies();
    expect(bodies).toHaveLength(1);
    expect(bodies[0]).toMatchObject({ lab_id: 'lab-demo', barcode: 'DEMO-0001' });
    expect(bodies[0]).not.toHaveProperty('query');
  });

  it('falls back to the free-text query only when the barcode search found nothing', async () => {
    const outcome = await searchSamples('lab-demo', 'Plasma A');

    expect(outcome.kind).toBe('query');
    expect(idsOf(outcome)).toEqual(['sample-3']);

    const bodies = await listBodies();
    expect(bodies).toHaveLength(2);
    // First the exact barcode, which answers nothing…
    expect(bodies[0]).toMatchObject({ barcode: 'Plasma A' });
    expect(bodies[0]).not.toHaveProperty('query');
    // …then the same term as free text over name and barcode.
    expect(bodies[1]).toMatchObject({ query: 'Plasma A' });
    expect(bodies[1]).not.toHaveProperty('barcode');
  });

  it('returns every match for a term several samples share', async () => {
    const outcome = await searchSamples('lab-demo', 'Serum');

    expect(outcome.kind).toBe('query');
    expect(idsOf(outcome)).toEqual(['sample-1', 'sample-2']);
    expect(outcome.kind === 'none' ? false : outcome.hasMore).toBe(false);
  });

  it('does not send a one-character query the server would reject', async () => {
    const outcome = await searchSamples('lab-demo', 'D');

    expect(outcome).toEqual({ kind: 'none', reason: 'too-short' });
    // The barcode probe is still made — a one-character barcode is legal — but
    // the free-text request is what `ListSamples` answers INVALID_ARGUMENT for.
    expect(await listBodies()).toHaveLength(1);
  });

  it('reports no match when neither the barcode nor the free-text search hits', async () => {
    const outcome = await searchSamples('lab-demo', 'nothing-like-this');

    expect(outcome).toEqual({ kind: 'none', reason: 'no-match' });
    expect(await listBodies()).toHaveLength(2);
  });

  it('never resurrects a deleted sample', async () => {
    lab.samples.push(
      create(SampleSchema, {
        id: 'sample-tombstoned',
        labId: 'lab-demo',
        itemTypeId: 'it-serum',
        name: 'Tombstoned',
        barcode: 'DEMO-TOMB',
        status: SampleStatus.TOMBSTONED,
      }),
    );

    expect(await searchSamples('lab-demo', 'DEMO-TOMB')).toEqual({
      kind: 'none',
      reason: 'no-match',
    });
  });

  it('caps a broad match at one page and says that it did', async () => {
    lab = seedSamples(lab, 60);
    server.use(...fakeApi({ lab }));

    const outcome = await searchSamples('lab-demo', 'DEMO-00');

    expect(outcome.kind).toBe('query');
    expect(idsOf(outcome)).toHaveLength(LOOKUP_PAGE_SIZE);
    expect(outcome.kind === 'none' ? false : outcome.hasMore).toBe(true);
  });

  it('rejects rather than reporting "no match" when the search itself fails', async () => {
    // A failed request is not an empty result: falling back here would turn
    // "the server is unreachable" into "this barcode does not exist".
    server.use(...fakeApi({ lab, fail: { 'sample/list': 'UNAVAILABLE' } }));

    await expect(searchSamples('lab-demo', 'DEMO-0001')).rejects.toThrow();
  });
});
