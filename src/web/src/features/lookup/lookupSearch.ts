// SPDX-License-Identifier: AGPL-3.0-or-later
import { call } from '../../api/client';
import { sampleKeys } from '../../api/hooks/samples';
import type { Sample } from '../../gen/fmgr/v1/sample_pb';
import { queryTooShort } from '../samples/sampleFilters';

/**
 * The lookup's search, as a value (TODO.md G3.5, PRD §9).
 *
 * **Barcode first, and alone.** An exact scan must resolve to the tube in the
 * user's hand, not to the first sample whose *name* happens to contain the same
 * characters — so the barcode filter is tried on its own and a hit ends the
 * search there. The free-text fallback (`query`, G0.4) is reached only when the
 * barcode probe came back **empty**: a failed request is not an empty result,
 * and turning "the server is unreachable" into "this barcode does not exist"
 * would be the worst possible answer at the bench.
 *
 * **The term is sent whole.** Nothing here debounces or coerces: the caller
 * hands over what the scanner typed, and by the time this runs the whole string
 * has arrived. A debounce belongs to search-as-you-type, not to a scan.
 *
 * Pure apart from `call()`, so the screen's tests can pin the request order
 * without rendering anything.
 *
 * **`probeBarcode` is that first step on its own**, and it has a second caller:
 * G3.6's scan mode (`features/scan/`) resolves the tube with it and then acts on
 * it. Scan mode deliberately never reaches the free-text fallback — applying a
 * check-out to a *name* that happens to contain the scanned characters changes a
 * tube the operator is not holding — so the ordering above stays the single
 * definition of how a scanned term is looked up.
 */

/**
 * Rows per request. The barcode probe can only ever match a handful (a barcode
 * is unique per lab), but the fallback is a substring search over names and can
 * match thousands; one page keeps the pick list bounded, and `hasMore` is what
 * lets the screen say so instead of silently truncating.
 */
export const LOOKUP_PAGE_SIZE = 25;

/** Why a lookup found nothing, which the screen says in different words. */
export type LookupMiss = 'no-match' | 'too-short';

export type LookupOutcome =
  /** The exact barcode matched; `samples` is what the screen shows. */
  | { readonly kind: 'barcode'; readonly samples: readonly Sample[]; readonly hasMore: boolean }
  /** No barcode matched and the free-text search did. */
  | { readonly kind: 'query'; readonly samples: readonly Sample[]; readonly hasMore: boolean }
  | { readonly kind: 'none'; readonly reason: LookupMiss };

/**
 * Query keys for the lookup.
 *
 * Nested under `sampleKeys.all(labId)` on purpose: `useCheckoutSample`
 * invalidates that prefix, so checking a sample out from the card refreshes the
 * card's own entry as well — a key outside the prefix would leave the card
 * showing the status the sample had before the check-out.
 */
export const lookupKeys = {
  all: (labId: string) => [...sampleKeys.all(labId), 'lookup'] as const,
  search: (labId: string, term: string) => [...lookupKeys.all(labId), term] as const,
};

interface SamplePage {
  readonly samples: readonly Sample[];
  readonly hasMore: boolean;
}

/** One `sample/list` page, with the server's opaque token read as "there is more". */
async function listSamples(
  labId: string,
  filters: { readonly barcode?: string; readonly query?: string },
  pageSize: number = LOOKUP_PAGE_SIZE,
): Promise<SamplePage> {
  const response = await call('sample/list', {
    labId,
    // Deleted samples are not a lookup result, whoever scans the old label.
    includeArchived: false,
    ...filters,
    page: { pageSize, pageToken: '' },
  });

  return {
    samples: response.samples,
    hasMore: (response.page?.nextPageToken ?? '') !== '',
  };
}

/**
 * The exact-barcode step on its own: one page of `sample/list?barcode=`, no
 * fallback, and no interpretation of the answer.
 *
 * A caller that is going to *act* on the result (G3.6's scan mode) passes a
 * small `pageSize` — it only needs to know whether the barcode is unique — and
 * decides for itself what zero or several hits mean. Throws only when the
 * request itself failed; a caller must not read that as "no match".
 */
export async function probeBarcode(
  labId: string,
  term: string,
  pageSize: number = LOOKUP_PAGE_SIZE,
): Promise<SamplePage> {
  return listSamples(labId, { barcode: term }, pageSize);
}

/**
 * Where the sample is, for the scanned term.
 *
 * Throws only when the request itself failed; the caller renders that as the
 * error state, not as a miss.
 */
export async function searchSamples(labId: string, term: string): Promise<LookupOutcome> {
  const barcode = await probeBarcode(labId, term);
  if (barcode.samples.length > 0) {
    return { kind: 'barcode', ...barcode };
  }

  // The server requires two bytes for `query` and answers INVALID_ARGUMENT
  // below that, so a one-character non-barcode is a miss the screen explains
  // rather than a request it knows will fail.
  if (queryTooShort(term)) {
    return { kind: 'none', reason: 'too-short' };
  }

  const query = await listSamples(labId, { query: term });
  if (query.samples.length === 0) {
    return { kind: 'none', reason: 'no-match' };
  }
  return { kind: 'query', ...query };
}
