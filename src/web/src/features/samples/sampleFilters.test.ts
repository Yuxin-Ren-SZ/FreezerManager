// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { SampleStatus } from '../../gen/fmgr/v1/sample_pb';
import {
  EMPTY_SAMPLE_FILTERS,
  hasActiveFilters,
  parseSampleFilters,
  queryTooShort,
  sampleFiltersToSearch,
  toListFilters,
  type SampleFilters,
} from './sampleFilters';

/**
 * The sample browser's filters live in the URL (TODO.md G3.2): a filtered view
 * has to be a link someone can paste, and a reload has to restore it. That makes
 * this module a parsing boundary — the URL is user input — so the interesting
 * cases are the malformed ones, not the happy path.
 */

const parse = (search: string) => parseSampleFilters(new URLSearchParams(search));
const toSearch = (filters: Partial<SampleFilters>) =>
  sampleFiltersToSearch({ ...EMPTY_SAMPLE_FILTERS, ...filters }).toString();

describe('parseSampleFilters', () => {
  it('reads every filter out of the URL', () => {
    const filters = parse(
      'status=SAMPLE_STATUS_CHECKED_OUT&boxId=box-2&itemTypeId=it-plasma&barcode=DEMO-0003&q=plasma',
    );

    expect(filters).toEqual({
      status: SampleStatus.CHECKED_OUT,
      boxId: 'box-2',
      itemTypeId: 'it-plasma',
      barcode: 'DEMO-0003',
      query: 'plasma',
    });
  });

  it('returns "any" for a URL with no filters', () => {
    expect(parse('')).toEqual(EMPTY_SAMPLE_FILTERS);
  });

  it('ignores a status it does not know, instead of sending a numeric guess', () => {
    // A hand-edited or stale link must not turn into `status=0` (UNSPECIFIED)
    // or into a raw `NaN` on the wire.
    expect(parse('status=SAMPLE_STATUS_GONE').status).toBe('');
    expect(parse('status=3').status).toBe('');
    expect(parse('status=').status).toBe('');
  });

  it('ignores unknown parameters, so a link with extras still works', () => {
    expect(parse('sort=name&page=4&q=serum')).toEqual({
      ...EMPTY_SAMPLE_FILTERS,
      query: 'serum',
    });
  });

  it('keeps an empty value as "no filter" rather than an empty-string filter', () => {
    expect(parse('boxId=&barcode=')).toEqual(EMPTY_SAMPLE_FILTERS);
  });
});

describe('sampleFiltersToSearch', () => {
  it('writes every filter into the URL', () => {
    const search = toSearch({
      status: SampleStatus.DEPLETED,
      boxId: 'box-1',
      itemTypeId: 'it-serum',
      barcode: 'DEMO-0001',
      query: 'serum',
    });
    const params = new URLSearchParams(search);

    expect(params.get('status')).toBe('SAMPLE_STATUS_DEPLETED');
    expect(params.get('boxId')).toBe('box-1');
    expect(params.get('itemTypeId')).toBe('it-serum');
    expect(params.get('barcode')).toBe('DEMO-0001');
    expect(params.get('q')).toBe('serum');
  });

  it('omits the defaults, so an unfiltered view has a clean link', () => {
    expect(toSearch({})).toBe('');
  });

  it('round-trips: a shared link parses back to the same filters', () => {
    const filters: SampleFilters = {
      status: SampleStatus.CHECKED_OUT,
      boxId: 'box-2',
      itemTypeId: 'it-plasma',
      barcode: 'DEMO-0003',
      query: 'plasma',
    };

    expect(parse(toSearch(filters))).toEqual(filters);
    // Idempotent: re-serialising a parsed link produces the same link, which is
    // what makes a copied URL stable across a reload.
    expect(toSearch(parse(toSearch(filters)))).toBe(toSearch(filters));
  });

  it('escapes values that would otherwise change the URL', () => {
    const search = toSearch({ query: 'a&b=c d' });

    expect(new URLSearchParams(search).get('q')).toBe('a&b=c d');
  });
});

describe('toListFilters', () => {
  it('sends only the filters that are set', () => {
    expect(toListFilters(EMPTY_SAMPLE_FILTERS)).toEqual({});
    expect(toListFilters({ ...EMPTY_SAMPLE_FILTERS, boxId: 'box-1' })).toEqual({ boxId: 'box-1' });
  });

  it('maps the status to its enum value', () => {
    expect(toListFilters({ ...EMPTY_SAMPLE_FILTERS, status: SampleStatus.ACTIVE })).toEqual({
      status: SampleStatus.ACTIVE,
    });
  });

  it('holds back a one-character query, which the server rejects', () => {
    // `ListSamples` answers INVALID_ARGUMENT below two bytes. Sending it anyway
    // would turn "still typing" into an error state.
    expect(toListFilters({ ...EMPTY_SAMPLE_FILTERS, query: 's' })).toEqual({});
    expect(toListFilters({ ...EMPTY_SAMPLE_FILTERS, query: '' })).toEqual({});
  });

  it('counts bytes, not characters, so a single umlaut is a real query', () => {
    // The server's rule: "a single multi-byte character such as ü is already
    // selective and is accepted".
    expect(toListFilters({ ...EMPTY_SAMPLE_FILTERS, query: 'ü' })).toEqual({ query: 'ü' });
    expect(toListFilters({ ...EMPTY_SAMPLE_FILTERS, query: 'se' })).toEqual({ query: 'se' });
  });
});

describe('hasActiveFilters', () => {
  it('is false only for the empty set', () => {
    expect(hasActiveFilters(EMPTY_SAMPLE_FILTERS)).toBe(false);
    expect(hasActiveFilters({ ...EMPTY_SAMPLE_FILTERS, barcode: 'X' })).toBe(true);
    expect(hasActiveFilters({ ...EMPTY_SAMPLE_FILTERS, status: SampleStatus.ACTIVE })).toBe(true);
  });
});

describe('queryTooShort', () => {
  it('is true only while the user is still typing', () => {
    expect(queryTooShort('')).toBe(false);
    expect(queryTooShort('s')).toBe(true);
    expect(queryTooShort('se')).toBe(false);
    expect(queryTooShort('ü')).toBe(false);
  });
});
