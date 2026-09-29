// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback, useEffect, useId, useRef, useState } from 'react';
import type { ReactNode, SyntheticEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { enumLabel } from '../../api/helpers';
import { useCheckoutSample } from '../../api/hooks';
import { useLabs } from '../../app/labs';
import { SampleStatus, SampleStatusSchema, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { Badge, Button, Checkbox, EmptyState, Select, TextField, type BadgeTone } from '../../ui';
import { probeBarcode } from '../lookup/lookupSearch';
import {
  ACTION_LABEL_KEY,
  AUTO_SUBMIT_GAP_MS,
  CHECKOUT_ACTION,
  OUTCOME_HINT_KEY,
  OUTCOME_LABEL_KEY,
  OUTCOME_TONE,
  SCAN_ACTIONS,
  SCAN_PROBE_PAGE_SIZE,
  outcomeForError,
  probeOutcome,
  scanKey,
  type ScanAction,
  type ScanLogEntry,
} from './scanSession';
import styles from './ScanScreen.module.css';

/**
 * Bulk check-in/out scan mode (TODO.md G3.6, F6.4).
 *
 * Pick an action once, then scan a stack of tubes and read the running log. The
 * single scan is the easy part; the loop is what the screen is built around,
 * and each of its properties is a thing that fails silently:
 *
 *  - **Every scan is `sample/list?barcode`, then `sample/checkout`.** The
 *    lookup is G3.5's exact-barcode probe (`probeBarcode`), reused rather than
 *    re-implemented — and deliberately *not* its free-text fallback: acting on a
 *    name that happens to contain the scanned characters would check out a tube
 *    the operator is not holding.
 *  - **The four outcomes stay four.** Done, no such barcode, wrong state and
 *    refused are four different next moves; `scanSession.ts` derives them and
 *    adds three that are not collapses of them (a repeat of this session's own
 *    action, a barcode that is not unique, and a request that never got an
 *    answer).
 *  - **A duplicate is decided here, not by the server.** The session remembers
 *    `(action, barcode)` pairs it has already applied and skips a repeat without
 *    sending anything, so a double-triggered scanner cannot double-apply — and a
 *    change of action is not a duplicate, because that is exactly how a
 *    check-out is reversed.
 *  - **The field never loses the focus.** It is read from the DOM at submit
 *    time (a scanner types a whole barcode faster than a human types one
 *    character, so a state copy can be a keystroke behind), and it comes back
 *    focused with its text selected after every scan — so the next tube is typed
 *    straight over the last one.
 *  - **There is no undo**, because the audit trail is the record. The screen
 *    explains how to reverse an action instead of drawing a button that could
 *    not work.
 */

/** Lifecycle state to badge tone, as in `sampleColumns.tsx` and G3.5's card. */
/** Partial on purpose: a newer server may send a status this bundle does not know. */
const STATUS_TONE: Readonly<Partial<Record<number, BadgeTone>>> = {
  [SampleStatus.ACTIVE]: 'success',
  [SampleStatus.CHECKED_OUT]: 'info',
  [SampleStatus.DEPLETED]: 'warning',
  [SampleStatus.DESTROYED]: 'neutral',
  [SampleStatus.TOMBSTONED]: 'danger',
};

/** The units `core::parse_volume_unit` accepts — the server rejects any other. */
const VOLUME_UNITS = ['µL', 'mL'] as const;

// Technical identifier, not user-visible copy: keeps `i18next/no-literal-string`
// a guard for real text. Same value as the sample form's volume field.
const STEP_ANY = 'any';

type VolumeUnit = (typeof VOLUME_UNITS)[number];

export function ScanScreen() {
  const { t } = useTranslation('scan');
  // `enums.*` lives in the default namespace.
  const { t: tCommon } = useTranslation();
  const { selectedLabId } = useLabs();
  const labId = selectedLabId ?? '';

  const inputRef = useRef<HTMLInputElement>(null);
  const fieldId = useId();
  const hintId = `${fieldId}-hint`;
  const logHeadingId = useId();

  const [action, setAction] = useState<ScanAction>('out');
  const [reason, setReason] = useState('');
  const [volume, setVolume] = useState('');
  const [volumeUnit, setVolumeUnit] = useState<VolumeUnit>('µL');
  const [autoSubmit, setAutoSubmit] = useState(false);
  const [entries, setEntries] = useState<readonly ScanLogEntry[]>([]);

  /**
   * The pairs this session has already applied, so a repeat is a skip rather
   * than a second action. A ref, not state: it is read inside the scan chain,
   * never rendered.
   */
  const appliedRef = useRef(new Map<string, ScanLogEntry>());
  const nextIdRef = useRef(1);
  /**
   * Scans run one at a time. Two Enter presses in the same tick (a
   * double-triggered scanner) still produce two log lines in the order they
   * were scanned, rather than two responses racing for the same slot.
   */
  const chainRef = useRef<Promise<void>>(Promise.resolve());
  const timerRef = useRef<number | null>(null);

  const checkout = useCheckoutSample(labId);

  const cancelAutoSubmit = useCallback(() => {
    if (timerRef.current !== null) {
      window.clearTimeout(timerRef.current);
      timerRef.current = null;
    }
  }, []);

  /**
   * Put the caret back in the field, selecting only what was actually scanned:
   * if the operator has already started the next barcode, selecting it would
   * make the next keystroke overwrite their characters.
   */
  const focusField = useCallback((scanned: string) => {
    const input = inputRef.current;
    if (input === null) {
      return;
    }
    input.focus();
    if (input.value.trim() === scanned) {
      input.select();
    }
  }, []);

  const append = useCallback((entry: ScanLogEntry) => {
    setEntries((current) => [...current, entry]);
  }, []);

  /** One scan, start to finish: resolve, act, log, hand the field back. */
  const runScan = async (barcode: string): Promise<void> => {
    const id = nextIdRef.current++;
    const key = scanKey(action, barcode);

    // The duplicate check runs before any request: the guarantee is that a
    // repeat never reaches the server, not that the server refuses it.
    const applied = appliedRef.current.get(key);
    if (applied !== undefined) {
      append({ id, barcode, action, outcome: 'duplicate', sample: applied.sample });
      focusField(barcode);
      return;
    }

    let samples: readonly Sample[];
    try {
      samples = (await probeBarcode(labId, barcode, SCAN_PROBE_PAGE_SIZE)).samples;
    } catch (error) {
      // A request that failed is not a barcode that does not exist.
      append({ id, barcode, action, outcome: outcomeForError(error), sample: null });
      focusField(barcode);
      return;
    }

    const miss = probeOutcome(samples);
    if (miss !== null) {
      append({ id, barcode, action, outcome: miss, sample: null });
      focusField(barcode);
      return;
    }

    const target = samples[0];
    const typed = Number(volume);
    // `CheckoutSampleRequest` needs both `volume_used` and `volume_unit`, or the
    // server drops the volume silently (`SampleServiceImpl::CheckoutSample`).
    const consumes = action === 'in' && volume.trim() !== '' && Number.isFinite(typed) && typed > 0;

    try {
      const response = await checkout.mutateAsync({
        sampleId: target.id,
        action: CHECKOUT_ACTION[action],
        ...(consumes ? { volumeUsed: typed, volumeUnit } : {}),
        ...(reason.trim() === '' ? {} : { reason: reason.trim() }),
      });
      const entry: ScanLogEntry = {
        id,
        barcode,
        action,
        outcome: 'done',
        sample: response.sample ?? target,
      };
      appliedRef.current.set(key, entry);
      append(entry);
    } catch (error) {
      append({ id, barcode, action, outcome: outcomeForError(error), sample: target });
    }
    focusField(barcode);
  };

  const enqueue = (barcode: string) => {
    chainRef.current = chainRef.current.then(() => runScan(barcode));
  };

  /** Read the field's own value: a scanner's burst is already in the DOM. */
  const submitScan = () => {
    const barcode = (inputRef.current?.value ?? '').trim();
    // No lab, nothing to scan into — the field is disabled, and a scan that
    // arrived anyway must not become a `sample/list` for lab `""`.
    if (labId === '' || barcode === '') {
      return;
    }
    enqueue(barcode);
  };

  const onSubmit = (event: SyntheticEvent<HTMLFormElement>) => {
    event.preventDefault();
    submitScan();
  };

  const onFieldChange = (value: string) => {
    cancelAutoSubmit();
    // Off by default, and never for an empty field: a scanner that sends Enter
    // needs no timer, and one that fires mid-burst swallows the scan.
    if (!autoSubmit || value.trim() === '') {
      return;
    }
    timerRef.current = window.setTimeout(() => {
      timerRef.current = null;
      // Select the scan before submitting it: with no Enter, the operator's
      // next keystroke would otherwise append to this barcode.
      focusField(value.trim());
      submitScan();
    }, AUTO_SUBMIT_GAP_MS);
  };

  const startNewSession = () => {
    cancelAutoSubmit();
    appliedRef.current = new Map();
    nextIdRef.current = 1;
    setEntries([]);
    const input = inputRef.current;
    if (input !== null) {
      input.value = '';
    }
    focusField('');
  };

  // The selected lab comes from the session, so the field renders before it is
  // usable — and a disabled input silently drops a focus asked for too early.
  useEffect(() => {
    if (labId !== '') {
      inputRef.current?.focus();
    }
  }, [labId]);

  useEffect(() => {
    if (!autoSubmit) {
      cancelAutoSubmit();
    }
  }, [autoSubmit, cancelAutoSubmit]);

  // A timer that outlived the screen would submit into nothing.
  useEffect(() => cancelAutoSubmit, [cancelAutoSubmit]);

  const actionLabel = (value: ScanAction) => t(ACTION_LABEL_KEY[value]);
  const statusLabel = (status: number) => enumLabel(tCommon, SampleStatusSchema, status);

  const renderEntry = (entry: ScanLogEntry): ReactNode => (
    <li key={entry.id} className={styles.entry}>
      <Badge tone={OUTCOME_TONE[entry.outcome]}>{t(OUTCOME_LABEL_KEY[entry.outcome])}</Badge>
      <span className={styles.entryBarcode}>{entry.barcode}</span>
      {entry.sample !== null ? (
        <>
          <Link
            className={styles.entrySample}
            to={`/labs/${encodeURIComponent(labId)}/samples/${encodeURIComponent(entry.sample.id)}`}
          >
            {entry.sample.name}
          </Link>
          <Badge tone={STATUS_TONE[entry.sample.status] ?? 'neutral'}>
            {statusLabel(entry.sample.status)}
          </Badge>
        </>
      ) : null}
      <span className={styles.entryMessage}>
        {t(OUTCOME_HINT_KEY[entry.outcome], {
          action: actionLabel(entry.action),
          status: statusLabel(entry.sample?.status ?? SampleStatus.UNSPECIFIED),
        })}
      </span>
    </li>
  );

  return (
    <section className={styles.screen}>
      <h1 className={styles.title}>{t('title')}</h1>

      <div className={styles.controls}>
        <Select
          label={t('action.label')}
          hint={t('action.hint')}
          value={action}
          onChange={(event) => {
            setAction(event.target.value as ScanAction);
          }}
          options={SCAN_ACTIONS.map((value) => ({ value, label: actionLabel(value) }))}
        />
        <TextField
          label={t('reason.label')}
          hint={t('reason.hint')}
          value={reason}
          autoComplete="off"
          onChange={(event) => {
            setReason(event.target.value);
          }}
        />
        {action === 'in' ? (
          <>
            {/* Volume is a check-in's business only: a check-out consumes
                nothing and a discard takes whatever is left. */}
            <TextField
              label={t('volume.label')}
              hint={t('volume.hint')}
              type="number"
              min={0}
              step={STEP_ANY}
              inputMode="decimal"
              value={volume}
              onChange={(event) => {
                setVolume(event.target.value);
              }}
            />
            <Select
              label={t('volume.unitLabel')}
              value={volumeUnit}
              onChange={(event) => {
                setVolumeUnit(event.target.value as VolumeUnit);
              }}
              options={VOLUME_UNITS.map((unit) => ({ value: unit, label: unit }))}
            />
          </>
        ) : null}
      </div>

      <form
        className={styles.scan}
        role="search"
        aria-label={t('field.label')}
        onSubmit={onSubmit}
        noValidate
      >
        <input
          ref={inputRef}
          id={fieldId}
          type="search"
          className={styles.input}
          aria-label={t('field.label')}
          aria-describedby={hintId}
          placeholder={t('field.placeholder')}
          autoComplete="off"
          autoCorrect="off"
          spellCheck={false}
          enterKeyHint="search"
          disabled={labId === ''}
          onChange={(event) => {
            onFieldChange(event.target.value);
          }}
        />
        <Button type="submit" variant="primary" size="lg" disabled={labId === ''}>
          {t('field.submit')}
        </Button>
      </form>

      <p className={styles.hint} id={hintId}>
        {labId === '' ? t('field.noLab') : t('field.hint')}
      </p>

      <Checkbox
        label={t('autoSubmit.label')}
        hint={t('autoSubmit.hint')}
        checked={autoSubmit}
        onChange={(event) => {
          setAutoSubmit(event.target.checked);
        }}
      />

      <section className={styles.session} aria-labelledby={logHeadingId}>
        <div className={styles.sessionHeader}>
          <h2 className={styles.sessionTitle} id={logHeadingId}>
            {t('log.title')}
          </h2>
          {entries.length > 0 ? (
            <p className={styles.count}>{t('log.count', { count: entries.length })}</p>
          ) : null}
          <Button size="sm" disabled={entries.length === 0} onClick={startNewSession}>
            {t('session.new')}
          </Button>
        </div>
        <p className={styles.hint}>{t('session.newHint')}</p>

        {entries.length === 0 ? (
          <EmptyState title={t('log.emptyTitle')} description={t('log.emptyHint')} />
        ) : (
          // `aria-live` so a screen reader hears each line as it is appended:
          // the log is the only feedback the scan gets.
          <ol className={styles.log} aria-label={t('log.title')} aria-live="polite">
            {entries.map(renderEntry)}
          </ol>
        )}
      </section>

      <div className={styles.noUndo}>
        <h2 className={styles.noUndoTitle}>{t('noUndo.title')}</h2>
        <p className={styles.noUndoBody}>{t('noUndo.body')}</p>
        <p className={styles.noUndoBody}>{t('noUndo.reverse')}</p>
        <p className={styles.noUndoBody}>{t('noUndo.session')}</p>
      </div>
    </section>
  );
}
