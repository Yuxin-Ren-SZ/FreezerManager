// SPDX-License-Identifier: AGPL-3.0-or-later
import type { TFunction } from 'i18next';
import { useId, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate, useParams } from 'react-router-dom';
import { isApiError } from '../../api/errors';
import { apiErrorMessage, enumLabel, formatTimestamp } from '../../api/helpers';
import { useAuditEvents, useSample } from '../../api/hooks';
import { useCan } from '../../app/session';
import { FieldDataType, type CustomFieldDefinition } from '../../gen/fmgr/v1/item_type_pb';
import { SampleStatus, SampleStatusSchema, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { Badge, ErrorState, Spinner, VisuallyHidden, type BadgeTone } from '../../ui';
import { parseCustomFieldValues, resolveInheritedDefinitions } from './customFields';
import { SampleActions } from './SampleActions';
import styles from './SampleDetailScreen.module.css';
import { SampleForm } from './SampleForm';
import { useSampleReferenceData } from './useSampleReferenceData';

/**
 * One sample in full (TODO.md G3.3).
 *
 * Four rules are load-bearing, and each shows up as a conditional below:
 *
 *  1. **Every field is shown, custom fields according to their definition
 *     type.** A value with no definition is still rendered under its key —
 *     dropping it would hide data the database holds.
 *  2. **PHI appears only when the response contains it.** The server filters by
 *     `phi.read` (`reveal_phi()` in `SampleServiceImpl.cc`), so the client
 *     renders what arrived and marks a field as PHI from its definition. It
 *     never renders a row for an absent PHI key, which would otherwise tell
 *     someone without `phi.read` that the sample has a donor name at all.
 *  3. **`audit.read` gates the history section entirely** — no permission, no
 *     section: not an empty list, not an error. The query is not even issued.
 *  4. **Actions follow the permissions the server enforces**, and every failure
 *     they produce is mapped back onto the field it names.
 */
export function SampleDetailScreen() {
  const { t } = useTranslation('sample-detail');
  const { t: tCommon } = useTranslation();
  const navigate = useNavigate();
  const params = useParams();
  const labId = params.labId ?? '';
  const sampleId = params.sampleId ?? '';

  const reference = useSampleReferenceData(labId);
  const sampleQuery = useSample(labId, sampleId);
  const canReadAudit = useCan('audit.read', labId);

  const history = useAuditEvents({
    labId,
    entityKind: 'sample',
    entityId: sampleId,
    enabled: canReadAudit,
  });

  const sample = sampleQuery.data?.sample;
  // The parent is a sample of its own; asking for it is one request, and the
  // hook is disabled when there is no parent to ask about.
  const parentQuery = useSample(labId, sample?.parentSampleId ?? '');
  const parent = parentQuery.data?.sample;

  const [editing, setEditing] = useState(false);
  const [saved, setSaved] = useState(false);

  const definitions = useMemo(
    () =>
      sample === undefined
        ? []
        : resolveInheritedDefinitions(reference.itemTypes, reference.cfds, sample.itemTypeId),
    [reference.itemTypes, reference.cfds, sample],
  );

  const overviewHeadingId = useId();
  const customFieldsHeadingId = useId();
  const historyHeadingId = useId();

  if (sampleQuery.isPending) {
    return <Spinner />;
  }

  if (sampleQuery.isError) {
    const notFound = isApiError(sampleQuery.error) && sampleQuery.error.code === 'NOT_FOUND';
    return (
      <section className={styles.screen}>
        <ErrorState
          title={notFound ? t('notFoundTitle') : t('errorTitle')}
          description={apiErrorMessage(tCommon, sampleQuery.error)}
          onRetry={
            notFound
              ? undefined
              : () => {
                  void sampleQuery.refetch();
                }
          }
        />
      </section>
    );
  }

  if (sample === undefined) {
    return null;
  }

  if (editing) {
    return (
      <section className={styles.screen}>
        <SampleForm
          labId={labId}
          sample={sample}
          reference={reference}
          onSaved={() => {
            setEditing(false);
            setSaved(true);
          }}
          onCancel={() => {
            setEditing(false);
          }}
        />
      </section>
    );
  }

  const itemType = reference.itemTypes.find((candidate) => candidate.id === sample.itemTypeId);
  const containerType = reference.containerTypes.find(
    (candidate) => candidate.id === sample.containerTypeId,
  );

  const path = reference.locationPath(sample.boxId ?? '', sample.positionLabel ?? '');
  const stored = parseCustomFieldValues(sample.customFieldsJson);
  const knownKeys = new Set(definitions.map((cfd) => cfd.key));
  const customFieldRows = [
    ...definitions
      .filter((cfd) => stored[cfd.key] !== undefined)
      .map((cfd) => ({
        key: cfd.key,
        label: cfd.label,
        isPhi: cfd.isPhi,
        value: formatStoredValue(t, cfd, stored[cfd.key]),
      })),
    // A value with no definition: shown under its key rather than dropped,
    // which is the only honest thing left to do with it.
    ...Object.keys(stored)
      .filter((key) => !knownKeys.has(key))
      .map((key) => ({ key, label: key, isPhi: false, value: stringifyStored(stored[key]) })),
  ];

  return (
    <section className={styles.screen}>
      <header className={styles.header}>
        <h1 className={styles.title}>{sample.name}</h1>
        <Badge tone={statusTone(sample.status)}>
          {enumLabel(tCommon, SampleStatusSchema, sample.status)}
        </Badge>
        {sample.barcode === undefined ? null : <p className={styles.barcode}>{sample.barcode}</p>}
      </header>

      {reference.isError ? (
        <p className={styles.formError} role="alert">
          {apiErrorMessage(tCommon, reference.error)}
        </p>
      ) : null}

      {saved ? (
        <p className={styles.saved} role="status">
          {t('form.saved')}
        </p>
      ) : null}

      <SampleActions
        labId={labId}
        sample={sample}
        reference={reference}
        onEdit={() => {
          setSaved(false);
          setEditing(true);
        }}
        onDeleted={() => {
          void navigate(`/labs/${encodeURIComponent(labId)}/samples`);
        }}
      />

      <section className={styles.section} aria-labelledby={overviewHeadingId}>
        <h2 className={styles.sectionTitle} id={overviewHeadingId}>
          {t('title')}
        </h2>
        <dl className={styles.fields}>
          <DetailRow label={t('detail.itemType')} value={itemType?.name ?? sample.itemTypeId} />
          <DetailRow
            label={t('detail.containerType')}
            value={containerType?.name ?? sample.containerTypeId ?? ''}
          />
          <DetailRow
            label={t('detail.location')}
            value={
              path.segments.length === 0
                ? t('detail.unplaced')
                : path.segments.map((segment) => segment.label).join(' / ')
            }
          />
          <DetailRow
            label={t('detail.volume')}
            value={formatQuantity(sample.volumeValue, sample.volumeUnit)}
          />
          <DetailRow
            label={t('detail.mass')}
            value={formatQuantity(sample.massValue, sample.massUnit)}
          />
          <DetailRow
            label={t('detail.created')}
            value={formatTimestamp(sample.createdAt?.unixMicros) ?? ''}
          />
          <DetailRow label={t('detail.createdBy')} value={sample.createdBy} />
          <DetailRow
            label={t('detail.lastModified')}
            value={formatTimestamp(sample.lastModifiedAt?.unixMicros) ?? ''}
          />
          <DetailRow label={t('detail.lastModifiedBy')} value={sample.lastModifiedBy ?? ''} />
        </dl>
      </section>

      {sample.parentSampleId === undefined ? null : (
        <p className={styles.parent}>
          {parent === undefined
            ? t('detail.parentMissing', { id: sample.parentSampleId })
            : t('detail.parent', {
                name: parent.name,
                status: enumLabel(tCommon, SampleStatusSchema, parent.status),
              })}
        </p>
      )}

      <section className={styles.section} aria-labelledby={customFieldsHeadingId}>
        <h2 className={styles.sectionTitle} id={customFieldsHeadingId}>
          {t('detail.customFields')}
        </h2>
        {customFieldRows.length === 0 ? (
          <p className={styles.empty}>{t('detail.noCustomFields')}</p>
        ) : (
          <dl className={styles.fields}>
            {customFieldRows.map((row) => (
              <div className={styles.row} key={row.key}>
                <dt className={styles.term}>
                  {row.label}
                  {row.isPhi ? (
                    <span className={styles.phi}>
                      <Badge tone="warning">{t('phi.badge')}</Badge>
                      <VisuallyHidden>{t('phi.hint')}</VisuallyHidden>
                    </span>
                  ) : null}
                </dt>
                <dd className={styles.value}>{row.value}</dd>
              </div>
            ))}
          </dl>
        )}
      </section>

      {canReadAudit ? (
        <section className={styles.section} aria-labelledby={historyHeadingId}>
          <h2 className={styles.sectionTitle} id={historyHeadingId}>
            {t('detail.history')}
          </h2>
          {history.isPending ? <Spinner /> : null}
          {history.isError ? <p className={styles.empty}>{t('detail.historyFailed')}</p> : null}
          {!history.isPending && !history.isError && history.data.events.length === 0 ? (
            <p className={styles.empty}>{t('detail.historyEmpty')}</p>
          ) : null}
          <ul className={styles.history}>
            {(history.data?.events ?? []).map((event) => (
              <li className={styles.historyItem} key={event.id}>
                <span className={styles.historyAction}>{event.action}</span>
                <span className={styles.historyMeta}>
                  {[event.actorUserId, formatTimestamp(event.at?.unixMicros) ?? '']
                    .filter((part) => part !== '')
                    .join(' \u00b7 ')}
                </span>
              </li>
            ))}
          </ul>
        </section>
      ) : null}
    </section>
  );
}

function DetailRow({ label, value }: { label: string; value: string }) {
  return (
    <div className={styles.row}>
      <dt className={styles.term}>{label}</dt>
      <dd className={styles.value}>{value}</dd>
    </div>
  );
}

/** A stored custom-field value as text, according to its data type. */
function formatStoredValue(
  t: TFunction<'sample-detail'>,
  cfd: CustomFieldDefinition,
  value: unknown,
): string {
  if (cfd.dataType === FieldDataType.BOOL) {
    return value === true ? t('detail.yes') : t('detail.no');
  }
  return stringifyStored(value);
}

/**
 * Anything at all as text.
 *
 * A `switch` on `typeof` rather than `String(value)`: on an `unknown` the
 * latter is what `no-base-to-string` forbids, and it would print
 * `[object Object]` for a value the JSON happens to nest. A type no definition
 * can produce renders as empty rather than as a JavaScript internal.
 */
function stringifyStored(value: unknown): string {
  switch (typeof value) {
    case 'string':
      return value;
    case 'number':
    case 'boolean':
    case 'bigint':
      return String(value);
    case 'object':
      return value === null ? '' : JSON.stringify(value);
    default:
      return '';
  }
}

/** `100 µL`, or just the number when the unit is not set. */
function formatQuantity(value: number | undefined, unit: string | undefined): string {
  if (value === undefined) {
    return '';
  }
  return unit === undefined || unit === '' ? String(value) : `${String(value)} ${unit}`;
}

/** The badge tone for a lifecycle state; the label carries the meaning. */
function statusTone(status: Sample['status']): BadgeTone {
  switch (status) {
    case SampleStatus.ACTIVE:
      return 'success';
    case SampleStatus.CHECKED_OUT:
      return 'warning';
    case SampleStatus.DEPLETED:
      return 'neutral';
    case SampleStatus.DESTROYED:
    case SampleStatus.TOMBSTONED:
      return 'danger';
    default:
      return 'neutral';
  }
}
