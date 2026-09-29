// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useParams, useSearchParams } from 'react-router-dom';
import { isApiError } from '../../api/errors';
import { apiErrorMessage, enumLabel } from '../../api/helpers';
import {
  useCustomFieldDefinitions,
  useExportSamples,
  useItemTypes,
  useSampleLive,
  useSamples,
} from '../../api/hooks';
import { useCan } from '../../app/session';
import { SampleStatusSchema } from '../../gen/fmgr/v1/sample_pb';
import {
  Button,
  EmptyState,
  ErrorState,
  Select,
  Spinner,
  Table,
  TextField,
  type SelectOption,
} from '../../ui';
import { useLabLayout } from '../layout/useLabLayout';
import { downloadTextFile, exportFileName } from './exportSamples';
import { placementPath } from './placement';
import {
  EMPTY_SAMPLE_FILTERS,
  hasActiveFilters,
  parseSampleFilters,
  queryTooShort,
  sampleFiltersToSearch,
  sampleStatusFromParam,
  sampleStatusParamValue,
  SELECTABLE_SAMPLE_STATUSES,
  toListFilters,
  type SampleFilters,
} from './sampleFilters';
import { buildSampleColumns } from './sampleColumns';
import styles from './SampleBrowserScreen.module.css';

/**
 * Samples (TODO.md G3.2, F6.2/F6.6/F7) — the screen people live in.
 *
 * **Paging, not loading.** The lab can hold 100k samples; the browser holds one
 * page of them. `useSamples` walks the server's opaque `page_token`, `Table`
 * virtualizes the window, and `onEndReached` asks for the next page when the
 * window reaches the rows already loaded. Nothing on this screen ever needs the
 * whole result set, which is also why the row count shows what is *loaded*
 * rather than a total: `ListSamples` does not send one (no `*ServiceImpl` sets
 * `total_count`), so a total here would be an invention.
 *
 * **The URL is the filter state.** `sampleFilters.ts` owns the encoding; this
 * screen only writes it back on every control change, with `replace: true` so
 * typing a search does not fill the history with one entry per keystroke.
 *
 * **Live frames are not detail data.** `useSampleLive` merges `sample/watch`
 * into the list caches and invalidates the matching `sample/get` entry instead
 * of writing it; the stream never carries PHI, so it must never become what a
 * detail view shows.
 *
 * The custom-field columns need `custom_field.define`, which a read-only member
 * does not hold: `useCan` switches that request off, and the screen is complete
 * without it (G-arch 8 — the server is still the enforcement point).
 */

/**
 * Rows per request.
 *
 * `SampleServiceImpl` treats `page_size = 0` as "no limit" and F8 wants a
 * default page size, so the client picks one explicitly rather than asking a
 * 100k-row lab for everything at once.
 */
const PAGE_SIZE = 100;

export function SampleBrowserScreen() {
  const { t } = useTranslation('samples');
  // `errors.*` and `enums.*` live in the default namespace.
  const { t: tCommon } = useTranslation();
  const labId = useParams().labId ?? '';
  const [searchParams, setSearchParams] = useSearchParams();

  const filters = useMemo(() => parseSampleFilters(searchParams), [searchParams]);
  const listFilters = useMemo(() => toListFilters(filters), [filters]);

  const samplesQuery = useSamples({ labId, pageSize: PAGE_SIZE, ...listFilters });
  // G3.1's hook: the box filter's labels and the location column both need it.
  const { boxes, locationPath } = useLabLayout(labId);
  const itemTypesQuery = useItemTypes(labId);
  const canDefineFields = useCan('custom_field.define', labId);
  const cfdsQuery = useCustomFieldDefinitions(labId, { enabled: canDefineFields });
  const live = useSampleLive({ labId, boxId: filters.boxId, itemTypeId: filters.itemTypeId });
  const exportSamples = useExportSamples(labId);
  const [exportError, setExportError] = useState<unknown>(null);

  // Memoised so the column set is rebuilt when the data changes and not on
  // every render (`?? []` would be a fresh array each time).
  const itemTypes = useMemo(() => itemTypesQuery.data?.itemTypes ?? [], [itemTypesQuery.data]);
  const cfds = useMemo(() => cfdsQuery.data?.cfds ?? [], [cfdsQuery.data]);

  const columns = useMemo(
    () => buildSampleColumns({ labId, t, tEnums: tCommon, itemTypes, cfds, locationPath }),
    [labId, t, tCommon, itemTypes, cfds, locationPath],
  );

  const rows = useMemo(
    () => (samplesQuery.data?.pages ?? []).flatMap((page) => page.samples),
    [samplesQuery.data],
  );

  const statusOptions = useMemo<SelectOption[]>(
    () =>
      SELECTABLE_SAMPLE_STATUSES.map((status) => ({
        value: sampleStatusParamValue(status),
        label: enumLabel(tCommon, SampleStatusSchema, status),
      })),
    [tCommon],
  );

  const boxOptions = useMemo<SelectOption[]>(
    () =>
      boxes.map((box) => ({
        value: box.id,
        // The path, not just the label: two freezers can each hold a "Box A".
        label: placementPath(locationPath(box.id)) || box.label,
      })),
    [boxes, locationPath],
  );

  const update = useCallback(
    (patch: Partial<SampleFilters>) => {
      setSearchParams(sampleFiltersToSearch({ ...filters, ...patch }), { replace: true });
    },
    [filters, setSearchParams],
  );

  const clear = useCallback(() => {
    setSearchParams(sampleFiltersToSearch(EMPTY_SAMPLE_FILTERS), { replace: true });
  }, [setSearchParams]);

  const fetchNextPage = samplesQuery.fetchNextPage;
  const { hasNextPage, isFetchingNextPage } = samplesQuery;
  const onEndReached = useCallback(() => {
    // Both halves matter: `onEndReached` fires whenever the window covers the
    // end, including while a page is already on its way back.
    if (hasNextPage && !isFetchingNextPage) {
      void fetchNextPage();
    }
  }, [hasNextPage, isFetchingNextPage, fetchNextPage]);

  const exportPending = exportSamples.isPending;
  const mutateExport = exportSamples.mutate;
  const onExport = useCallback(() => {
    setExportError(null);
    mutateExport(
      {},
      {
        onSuccess: (response) => {
          downloadTextFile(exportFileName(labId), response.csvContent);
        },
        onError: (error: unknown) => {
          setExportError(error);
        },
      },
    );
  }, [mutateExport, labId]);

  const isPending = samplesQuery.isPending;
  const isError = samplesQuery.isError;
  const filtered = hasActiveFilters(filters);
  const liveLabel =
    live.status === 'live'
      ? t('live.live')
      : live.status === 'connecting'
        ? t('live.connecting')
        : t('live.error');

  return (
    <section className={styles.screen}>
      <div className={styles.header}>
        <h1 className={styles.title}>{t('title')}</h1>
        <div className={styles.actions}>
          <Button variant="secondary" onClick={onExport} disabled={exportPending}>
            {exportPending ? t('export.pending') : t('export.action')}
          </Button>
          <span className={styles.note}>{t('export.note')}</span>
        </div>
      </div>

      <p className={styles.live} role="status">
        {liveLabel}
      </p>

      <form
        className={styles.filters}
        aria-label={t('filters.legend')}
        onSubmit={(event) => {
          event.preventDefault();
        }}
      >
        <TextField
          label={t('filters.search')}
          type="search"
          value={filters.query}
          hint={
            queryTooShort(filters.query) ? t('filters.searchTooShort') : t('filters.searchHint')
          }
          onChange={(event) => {
            update({ query: event.target.value });
          }}
        />
        <TextField
          label={t('filters.barcode')}
          value={filters.barcode}
          hint={t('filters.barcodeHint')}
          onChange={(event) => {
            update({ barcode: event.target.value });
          }}
        />
        <Select
          label={t('filters.status')}
          placeholder={t('filters.statusAny')}
          value={sampleStatusParamValue(filters.status)}
          options={statusOptions}
          onChange={(event) => {
            update({ status: sampleStatusFromParam(event.target.value) });
          }}
        />
        <Select
          label={t('filters.itemType')}
          placeholder={t('filters.itemTypeAny')}
          value={filters.itemTypeId}
          options={itemTypes.map((itemType) => ({
            value: itemType.id,
            label: itemType.name,
          }))}
          onChange={(event) => {
            update({ itemTypeId: event.target.value });
          }}
        />
        <Select
          label={t('filters.box')}
          placeholder={t('filters.boxAny')}
          value={filters.boxId}
          options={boxOptions}
          onChange={(event) => {
            update({ boxId: event.target.value });
          }}
        />
        <Button variant="ghost" onClick={clear} disabled={!filtered}>
          {t('filters.clear')}
        </Button>
      </form>

      {exportError !== null ? (
        <p className={styles.exportError} role="alert">
          <strong>{t('export.errorTitle')}</strong>{' '}
          <span>{apiErrorMessage(tCommon, exportError)}</span>
        </p>
      ) : null}

      {isPending ? <Spinner /> : null}

      {!isPending && isError ? (
        <ErrorState
          title={t('errorTitle')}
          description={apiErrorMessage(tCommon, samplesQuery.error)}
          requestId={
            isApiError(samplesQuery.error) ? (samplesQuery.error.requestId ?? undefined) : undefined
          }
          onRetry={() => {
            void samplesQuery.refetch();
          }}
        />
      ) : null}

      {!isPending && !isError && rows.length === 0 ? (
        <EmptyState
          title={filtered ? t('table.emptyFilteredTitle') : t('table.emptyTitle')}
          description={filtered ? t('table.emptyFilteredHint') : t('table.emptyHint')}
        />
      ) : null}

      {!isPending && !isError && rows.length > 0 ? (
        <>
          <p className={styles.count}>
            <span>{t('table.loaded', { count: rows.length })}</span>
            {samplesQuery.hasNextPage ? <span>{t('table.more')}</span> : null}
          </p>
          <Table
            caption={t('table.caption')}
            columns={columns}
            data={rows}
            getRowId={(row) => row.id}
            onEndReached={onEndReached}
          />
        </>
      ) : null}
    </section>
  );
}
