// SPDX-License-Identifier: AGPL-3.0-or-later
import { useQuery } from '@tanstack/react-query';
import { useCallback, useEffect, useId, useRef, useState } from 'react';
import type { KeyboardEvent, ReactNode, SyntheticEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { Link, useSearchParams } from 'react-router-dom';
import { isApiError } from '../../api/errors';
import { apiErrorMessage, enumLabel } from '../../api/helpers';
import { useCheckoutSample } from '../../api/hooks';
import { useLabs } from '../../app/labs';
import { useCan } from '../../app/session';
import {
  CheckoutAction,
  SampleStatus,
  SampleStatusSchema,
  type Sample,
} from '../../gen/fmgr/v1/sample_pb';
import {
  Badge,
  Button,
  EmptyState,
  ErrorState,
  Spinner,
  classNames,
  type BadgeTone,
} from '../../ui';
import type { LocationPath } from '../layout/layoutModel';
import { useLabLayout } from '../layout/useLabLayout';
import { placementKind, placementPath } from '../samples/placement';
import { LOOKUP_PAGE_SIZE, lookupKeys, searchSamples } from './lookupSearch';
import styles from './LookupScreen.module.css';

/**
 * Lookup — the single-handed flow (TODO.md G3.5, PRD §9).
 *
 * Scan or type, press Enter, read the location. Everything here exists to keep
 * that loop going without a mouse and without a second look at the screen:
 *
 *  - **The field is the only thing that ever holds the focus.** After every
 *    lookup — a hit, a miss, a failure, a pick from the list, a check-out — it
 *    is focused again with its text selected, so the next scan overwrites it
 *    instead of appending to it. This is the criterion that fails invisibly:
 *    every individual lookup still works when it breaks, and only the person
 *    doing the second scan notices.
 *  - **The search runs on Enter and only on Enter** (`lookupSearch.ts` sends
 *    the term whole). Nothing is debounced, so a scanner — which types a whole
 *    barcode faster than a human types one character — cannot outrun it. The
 *    value is read from the DOM at submit time rather than from a state copy,
 *    because a state copy is what a fast burst would leave behind.
 *  - **The pick list is driven from the field**: arrow keys move the active
 *    row, Enter takes it, and `aria-activedescendant` tells a screen reader
 *    what the highlight is on. The mouse works too, but never has to.
 *  - **One hit, several hits, no hit, a broken location and an unplaced sample
 *    are five different answers**, and the screen says which one it is. "No
 *    match" is not an error state, and "not in a box yet" is not "location
 *    unavailable" — G3.2's `placement.ts` draws the same three-way line.
 *  - **`?q=` is answered, not ignored.** The shell's global lookup box
 *    navigates here with the term in the URL (G1.3), so arriving from the top
 *    bar looks the term up instead of opening an empty field.
 */

/** Lifecycle state to badge tone, as in `sampleColumns.tsx`. */
/** Partial on purpose: a newer server may send a status this bundle does not know. */
const STATUS_TONE: Readonly<Partial<Record<number, BadgeTone>>> = {
  [SampleStatus.ACTIVE]: 'success',
  [SampleStatus.CHECKED_OUT]: 'info',
  [SampleStatus.DEPLETED]: 'warning',
  [SampleStatus.DESTROYED]: 'neutral',
  [SampleStatus.TOMBSTONED]: 'danger',
};

/** A stable empty list, so "no hits" is the same value on every render. */
const NO_HITS: readonly Sample[] = [];

/** Which answer the result area is showing. */
type LookupView = 'idle' | 'pending' | 'error' | 'too-short' | 'no-match' | 'list' | 'card';

export function LookupScreen() {
  const { t } = useTranslation('lookup');
  // `errors.*` and `enums.*` live in the default namespace.
  const { t: tCommon } = useTranslation();
  const { selectedLabId } = useLabs();
  const labId = selectedLabId ?? '';
  const [searchParams] = useSearchParams();

  // The shell's global lookup box navigates to `/lookup?q=…` (G1.3), so the
  // screen has to answer the question it was handed instead of opening empty.
  const routedQuery = (searchParams.get('q') ?? '').trim();

  const inputRef = useRef<HTMLInputElement>(null);
  const fieldId = useId();
  const hintId = `${fieldId}-hint`;
  const listId = useId();
  const cardHeadingId = useId();

  /** The term that was submitted — what the query and the copy refer to. */
  const [term, setTerm] = useState(routedQuery);
  /** What is in the field right now, so a half-typed term hides the last answer. */
  const [typed, setTyped] = useState(routedQuery);
  const [activeIndex, setActiveIndex] = useState(0);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [actionError, setActionError] = useState<unknown>(null);

  // A *second* trip through the top bar changes `?q=` without remounting this
  // screen. Adjusting the state during render is React's own answer to "a prop
  // changed": an effect would paint the previous query's answer first.
  const [answeredQuery, setAnsweredQuery] = useState(routedQuery);
  if (routedQuery !== answeredQuery) {
    setAnsweredQuery(routedQuery);
    if (routedQuery !== '') {
      setTerm(routedQuery);
      setTyped(routedQuery);
      setActiveIndex(0);
      setSelectedId(null);
    }
  }

  // The field is uncontrolled — its value is the scanner's, not React's — so a
  // new routed query has to be written into the DOM element itself.
  useEffect(() => {
    if (routedQuery === '' || inputRef.current === null) {
      return;
    }
    inputRef.current.value = routedQuery;
  }, [routedQuery]);

  const search = useQuery({
    queryKey: lookupKeys.search(labId, term),
    queryFn: () => searchSamples(labId, term),
    enabled: labId !== '' && term !== '',
  });

  const layout = useLabLayout(labId);
  const checkout = useCheckoutSample(labId);
  const canCheckout = useCan('sample.checkout', labId);

  const outcome = search.data;
  const hits = outcome !== undefined && outcome.kind !== 'none' ? outcome.samples : NO_HITS;
  const selected = selectedId === null ? null : (hits.find((hit) => hit.id === selectedId) ?? null);
  const single = hits.length === 1 ? hits[0] : undefined;
  const dirty = typed.trim() !== term;
  const card = dirty ? null : (selected ?? single ?? null);
  const showList = !dirty && card === null && hits.length > 1;
  // A refetch can return fewer rows than the highlight was on.
  const active = hits.length === 0 ? 0 : Math.min(activeIndex, hits.length - 1);

  const view: LookupView =
    term === ''
      ? 'idle'
      : search.isPending
        ? 'pending'
        : search.isError
          ? 'error'
          : dirty
            ? 'idle'
            : outcome?.kind === 'none'
              ? outcome.reason === 'too-short'
                ? 'too-short'
                : 'no-match'
              : showList
                ? 'list'
                : card !== null
                  ? 'card'
                  : 'idle';

  /** Put the caret back in the field; select only what was actually searched. */
  const focusField = useCallback(() => {
    const input = inputRef.current;
    if (input === null) {
      return;
    }
    input.focus();
    if (input.value.trim() === term) {
      input.select();
    }
  }, [term]);

  useEffect(() => {
    if (term === '' || search.isPending) {
      return;
    }
    focusField();
  }, [term, search.isPending, search.dataUpdatedAt, search.isError, selectedId, focusField]);

  // The selected lab comes from the session (and from `localStorage`), so the
  // field can render before it is usable — and a disabled input silently drops
  // the focus `autoFocus` asks for. Focus follows the field becoming usable.
  useEffect(() => {
    if (labId === '') {
      return;
    }
    inputRef.current?.focus();
  }, [labId]);

  // A new lookup starts a new answer: nothing from the previous one carries
  // over. Done here rather than in an effect, so the reset cannot race the
  // render that already has the new result.
  const submit = (event: SyntheticEvent<HTMLFormElement>) => {
    event.preventDefault();
    // The DOM value, not the state copy: a scanner's burst is already there,
    // and on the render path a state copy can still be a keystroke behind.
    const value = (inputRef.current?.value ?? '').trim();
    setActionError(null);
    setActiveIndex(0);
    setSelectedId(null);

    if (value === '') {
      setTerm('');
      setTyped('');
      return;
    }
    if (value === term) {
      // Scanning the same tube again is a request to check again, not a no-op.
      void search.refetch();
      return;
    }
    setTyped(inputRef.current?.value ?? '');
    setTerm(value);
  };

  const onKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    // Only while the pick list is up: otherwise Enter and the arrows belong to
    // the browser and to the field.
    if (!showList) {
      return;
    }
    switch (event.key) {
      case 'ArrowDown':
        event.preventDefault();
        setActiveIndex((current) => (current + 1) % hits.length);
        break;
      case 'ArrowUp':
        event.preventDefault();
        setActiveIndex((current) => (current - 1 + hits.length) % hits.length);
        break;
      case 'Home':
        event.preventDefault();
        setActiveIndex(0);
        break;
      case 'End':
        event.preventDefault();
        setActiveIndex(hits.length - 1);
        break;
      case 'Enter': {
        event.preventDefault();
        // `active` is clamped to the list the highlight is drawn from.
        setSelectedId(hits[active].id);
        break;
      }
      default:
        break;
    }
  };

  const statusBadge = (sample: Sample): ReactNode => (
    <Badge tone={STATUS_TONE[sample.status] ?? 'neutral'}>
      {enumLabel(tCommon, SampleStatusSchema, sample.status)}
    </Badge>
  );

  /** The location line of a pick-list row: G3.1's path, or why there is none. */
  const describeLocation = (sample: Sample): string => {
    const path = layout.locationPath(sample.boxId ?? '', sample.positionLabel ?? '');
    const kind = placementKind(path);
    if (kind === 'placed') {
      return placementPath(path);
    }
    if ((sample.boxId ?? '') !== '' && layout.isPending) {
      return t('results.resolving');
    }
    return kind === 'unplaced' ? t('results.unplacedTitle') : t('results.unknownTitle');
  };

  const renderCard = (sample: Sample, path: LocationPath): ReactNode => {
    const kind = placementKind(path);
    // While the layout is still loading, a placed sample resolves to nothing —
    // which must not be shown as "the location is missing".
    const resolving = (sample.boxId ?? '') !== '' && layout.isPending;
    const isActive = sample.status === SampleStatus.ACTIVE;

    return (
      <section className={styles.card} aria-labelledby={cardHeadingId}>
        <p className={styles.kicker}>
          {hits.length === 1 ? t('results.one') : t('results.many', { count: hits.length })}
        </p>
        <h2 className={styles.sampleName} id={cardHeadingId}>
          <Link to={`/labs/${encodeURIComponent(labId)}/samples/${encodeURIComponent(sample.id)}`}>
            {sample.name}
          </Link>
        </h2>
        <p className={styles.barcode}>{sample.barcode ?? t('results.noBarcode')}</p>
        <p className={styles.status}>{statusBadge(sample)}</p>

        <div className={styles.location}>
          <h3 className={styles.locationHeading}>{t('results.location')}</h3>
          {resolving ? (
            <p className={styles.locationHint}>{t('results.resolving')}</p>
          ) : kind === 'placed' ? (
            <ol className={styles.path} aria-label={t('results.location')}>
              {path.segments.map((segment, index) => (
                // Segments repeat (two drawers can share a label), and the path
                // is positional, so the index belongs in the key.
                <li
                  key={`${segment.kind}:${segment.label}:${String(index)}`}
                  className={styles.pathStep}
                >
                  {segment.label}
                </li>
              ))}
            </ol>
          ) : (
            <>
              <p className={styles.locationTitle}>
                {kind === 'unplaced' ? t('results.unplacedTitle') : t('results.unknownTitle')}
              </p>
              <p className={styles.locationHint}>
                {kind === 'unplaced' ? t('results.unplacedHint') : t('results.unknownHint')}
              </p>
            </>
          )}
        </div>

        {canCheckout ? (
          <div className={styles.actions}>
            <Button
              variant="primary"
              size="lg"
              loading={checkout.isPending}
              disabled={!isActive}
              onClick={() => {
                setActionError(null);
                checkout
                  .mutateAsync({ sampleId: sample.id, action: CheckoutAction.CHECKOUT })
                  .catch((error: unknown) => {
                    setActionError(error);
                  });
              }}
            >
              {t('checkout.action')}
            </Button>
            {!isActive ? <span className={styles.hint}>{t('checkout.notActive')}</span> : null}
          </div>
        ) : null}

        {actionError !== null ? (
          <p className={styles.error} role="alert">
            <strong>{t('checkout.errorTitle')}</strong>{' '}
            <span>{apiErrorMessage(tCommon, actionError)}</span>
          </p>
        ) : null}
      </section>
    );
  };

  const renderResults = (): ReactNode => {
    switch (view) {
      case 'pending':
        return (
          <p className={styles.pending}>
            <Spinner size="sm" /> <span>{t('results.searching', { term })}</span>
          </p>
        );
      case 'error':
        return (
          <ErrorState
            title={t('errorTitle')}
            description={apiErrorMessage(tCommon, search.error)}
            requestId={isApiError(search.error) ? (search.error.requestId ?? undefined) : undefined}
            onRetry={() => {
              void search.refetch();
            }}
          />
        );
      case 'too-short':
        return <EmptyState title={t('tooShort.title')} description={t('tooShort.hint')} />;
      case 'no-match':
        return <EmptyState title={t('empty.title')} description={t('empty.hint', { term })} />;
      case 'list':
        return (
          <>
            <p className={styles.count}>{t('results.many', { count: hits.length })}</p>
            {outcome?.kind !== 'none' && outcome?.hasMore === true ? (
              <p className={styles.hint}>{t('results.more', { count: LOOKUP_PAGE_SIZE })}</p>
            ) : null}
            <ul
              className={styles.list}
              id={listId}
              role="listbox"
              aria-label={t('results.listLabel')}
            >
              {hits.map((hit, index) => (
                <li
                  key={hit.id}
                  id={`${listId}-option-${String(index)}`}
                  role="option"
                  aria-selected={index === active}
                  className={classNames(styles.option, index === active && styles.optionActive)}
                  // The mouse path deliberately hangs off `onMouseDown`, not
                  // `onClick`: the caret must stay in the field (a click that
                  // moved the focus would leave the next scan typing into a
                  // button), and `preventDefault` is what keeps it there. The
                  // keyboard path is the combobox input that owns this list —
                  // ArrowDown/ArrowUp move, Enter takes.
                  onMouseDown={(event) => {
                    event.preventDefault();
                    setSelectedId(hit.id);
                  }}
                >
                  <span className={styles.optionName}>{hit.name}</span>
                  <span className={styles.optionMeta}>{hit.barcode ?? t('results.noBarcode')}</span>
                  <span>{statusBadge(hit)}</span>
                  <span className={styles.optionLocation}>{describeLocation(hit)}</span>
                </li>
              ))}
            </ul>
          </>
        );
      case 'card':
        return card === null
          ? null
          : renderCard(card, layout.locationPath(card.boxId ?? '', card.positionLabel ?? ''));
      case 'idle':
        return <EmptyState title={t('idle.title')} description={t('idle.hint')} />;
    }
  };

  return (
    <section className={styles.screen}>
      <h1 className={styles.title}>{t('title')}</h1>

      <form
        className={styles.scan}
        role="search"
        aria-label={t('field.label')}
        onSubmit={submit}
        noValidate
      >
        <input
          ref={inputRef}
          id={fieldId}
          type="search"
          className={styles.input}
          // A visible label would sit above the field; at a freezer the field is
          // found by position, and the accessible name carries the instruction.
          aria-label={t('field.label')}
          aria-describedby={hintId}
          placeholder={t('field.placeholder')}
          defaultValue={routedQuery}
          autoComplete="off"
          autoCorrect="off"
          spellCheck={false}
          enterKeyHint="search"
          role="combobox"
          aria-expanded={showList}
          aria-controls={showList ? listId : undefined}
          aria-activedescendant={showList ? `${listId}-option-${String(active)}` : undefined}
          aria-autocomplete="list"
          disabled={labId === ''}
          onChange={(event) => {
            setTyped(event.target.value);
          }}
          onKeyDown={onKeyDown}
        />
        <Button type="submit" variant="primary" size="lg" disabled={labId === ''}>
          {t('field.submit')}
        </Button>
      </form>

      <p className={styles.hint} id={hintId}>
        {labId === '' ? t('field.noLab') : t('field.hint')}
      </p>

      <div className={styles.results}>{renderResults()}</div>
    </section>
  );
}
