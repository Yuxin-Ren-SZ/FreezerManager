// SPDX-License-Identifier: AGPL-3.0-or-later
import { enumValueName } from '../../api/helpers';
import type { SampleListFilters } from '../../api/hooks';
import { SampleStatus, SampleStatusSchema } from '../../gen/fmgr/v1/sample_pb';

/**
 * The sample browser's filters as a value, and their URL encoding (TODO.md
 * G3.2).
 *
 * Every filter lives in the URL — `status`, `boxId`, `itemTypeId`, `barcode`
 * and the free-text `q` — so a filtered view is a link someone can paste into a
 * chat and a reload restores exactly what the sender saw. That makes this module
 * a parsing boundary: the URL is user input, so a value the app does not
 * recognise is dropped rather than guessed at, and everything that reaches the
 * request is derived from the parsed value and not from the raw string.
 *
 * G-arch 7 (URLs carry ids only) is respected: `q` is the user's own search
 * text, the same parameter the shell's global lookup already puts in a URL
 * (`src/app/shell/GlobalLookup.tsx`), and none of these are secrets.
 */

export interface SampleFilters {
  /** `''` is "any status"; the enum values are the four a list can show. */
  readonly status: SampleStatus | '';
  readonly boxId: string;
  readonly itemTypeId: string;
  readonly barcode: string;
  /** Free text over name and barcode (G0.4); `''` is "no search". */
  readonly query: string;
}

export const EMPTY_SAMPLE_FILTERS: SampleFilters = {
  status: '',
  boxId: '',
  itemTypeId: '',
  barcode: '',
  query: '',
};

/** The URL parameter names. `q` matches the shell's lookup parameter. */
export const SAMPLE_FILTER_PARAMS = {
  status: 'status',
  boxId: 'boxId',
  itemTypeId: 'itemTypeId',
  barcode: 'barcode',
  query: 'q',
} as const;

/** `SampleServiceImpl::k_min_query_length`, in bytes. */
export const MIN_QUERY_BYTES = 2;

/**
 * The statuses the filter offers.
 *
 * `SAMPLE_STATUS_TOMBSTONED` is deliberately absent: the list hides tombstoned
 * rows and there is no "include deleted" control on this screen, so offering it
 * would be a filter that can only ever return nothing. `UNSPECIFIED` is not a
 * status a sample can have — it is the proto default and means "unset".
 */
export const SELECTABLE_SAMPLE_STATUSES: readonly SampleStatus[] = [
  SampleStatus.ACTIVE,
  SampleStatus.CHECKED_OUT,
  SampleStatus.DEPLETED,
  SampleStatus.DESTROYED,
];

/** The proto name of a selectable status (`SAMPLE_STATUS_ACTIVE`), or `''`. */
export function sampleStatusParamValue(status: SampleStatus | ''): string {
  return status === '' ? '' : (enumValueName(SampleStatusSchema, status) ?? '');
}

/**
 * A status parameter back to the enum, or `''` for "any".
 *
 * Shared by the URL parser and the status `<select>`, so the two can never
 * disagree about which spellings are real.
 */
export function sampleStatusFromParam(raw: string | null): SampleStatus | '' {
  if (raw === null || raw === '') {
    return '';
  }
  return SELECTABLE_SAMPLE_STATUSES.find((status) => sampleStatusParamValue(status) === raw) ?? '';
}

function parse(params: URLSearchParams, key: string): string {
  return params.get(key) ?? '';
}

/**
 * The filters a URL asks for. Unrecognised values are dropped, so a stale or
 * hand-edited link degrades to "fewer filters" instead of an error page or a
 * request the server rejects (`status=3` must not become `UNSPECIFIED`).
 */
export function parseSampleFilters(params: URLSearchParams): SampleFilters {
  return {
    status: sampleStatusFromParam(params.get(SAMPLE_FILTER_PARAMS.status)),
    boxId: parse(params, SAMPLE_FILTER_PARAMS.boxId),
    itemTypeId: parse(params, SAMPLE_FILTER_PARAMS.itemTypeId),
    barcode: parse(params, SAMPLE_FILTER_PARAMS.barcode),
    query: parse(params, SAMPLE_FILTER_PARAMS.query),
  };
}

/**
 * The URL for a set of filters: only the non-default ones, so an unfiltered
 * view has no query string at all and a filtered one is as short as it can be.
 */
export function sampleFiltersToSearch(filters: SampleFilters): URLSearchParams {
  const params = new URLSearchParams();
  const status = sampleStatusParamValue(filters.status);

  if (status !== '') params.set(SAMPLE_FILTER_PARAMS.status, status);
  if (filters.boxId !== '') params.set(SAMPLE_FILTER_PARAMS.boxId, filters.boxId);
  if (filters.itemTypeId !== '') params.set(SAMPLE_FILTER_PARAMS.itemTypeId, filters.itemTypeId);
  if (filters.barcode !== '') params.set(SAMPLE_FILTER_PARAMS.barcode, filters.barcode);
  if (filters.query !== '') params.set(SAMPLE_FILTER_PARAMS.query, filters.query);

  return params;
}

export function hasActiveFilters(filters: SampleFilters): boolean {
  return (
    filters.status !== '' ||
    filters.boxId !== '' ||
    filters.itemTypeId !== '' ||
    filters.barcode !== '' ||
    filters.query !== ''
  );
}

/** The server counts bytes, not characters, for the two-byte query minimum. */
export function utf8ByteLength(value: string): number {
  return new TextEncoder().encode(value).length;
}

/**
 * True while the user is still typing a query the server would refuse with
 * `INVALID_ARGUMENT`. An empty query is *not* too short — it means "no search".
 */
export function queryTooShort(query: string): boolean {
  const bytes = utf8ByteLength(query);
  return bytes > 0 && bytes < MIN_QUERY_BYTES;
}

/**
 * What the query key and the request body carry. A key that is absent is not
 * "the empty string": it is not sent at all, which is what keeps
 * `sampleKeys.list(labId, {})` equal to the unfiltered view.
 *
 * A query shorter than two bytes is dropped here rather than sent: the server
 * rejects it, and turning "still typing" into an error state would be worse
 * than showing the unfiltered list for one keystroke.
 */
export function toListFilters(filters: SampleFilters): SampleListFilters {
  return {
    ...(filters.status === '' ? {} : { status: filters.status }),
    ...(filters.boxId === '' ? {} : { boxId: filters.boxId }),
    ...(filters.itemTypeId === '' ? {} : { itemTypeId: filters.itemTypeId }),
    ...(filters.barcode === '' ? {} : { barcode: filters.barcode }),
    ...(utf8ByteLength(filters.query) < MIN_QUERY_BYTES ? {} : { query: filters.query }),
  };
}
