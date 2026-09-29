// SPDX-License-Identifier: AGPL-3.0-or-later
import { isApiError } from '../../api/errors';
import { CheckoutAction, type Sample } from '../../gen/fmgr/v1/sample_pb';
import type { BadgeTone } from '../../ui';

/**
 * The scan session's rules as values (TODO.md G3.6, F6.4).
 *
 * A scan is `sample/list?barcode` and then `sample/checkout` with the chosen
 * action — but the *loop* is where the decisions are, and they are made here so
 * the screen only renders them:
 *
 *  - **What one scan can answer.** The four outcomes the issue names are four
 *    different instructions to the person holding the tube (do the next one /
 *    check the label / look at the tube's state / ask an administrator), so they
 *    are four values and never one "failed". Three more are not collapses of
 *    them: a repeat of this session's own action (`duplicate`), a barcode that
 *    is not unique (`ambiguous`), and a request that never got an answer
 *    (`unavailable` — "the server is unreachable" is *not* "this barcode does
 *    not exist", the G3.5 lesson that costs somebody a tube).
 *  - **What makes a scan a repeat.** The key is `(action, barcode)`, because
 *    checking a tube out and then checking the same tube back in is the
 *    reversal the screen tells the operator about, not a double-apply. The
 *    barcode is trimmed (a scanner or a keyboard adds whitespace) but never
 *    case-folded: `SampleServiceImpl::ListSamples` filters barcodes exactly, so
 *    two barcodes that differ by case are two different tubes.
 */

/** The actions the screen offers, in the order it offers them. */
export const SCAN_ACTIONS = ['out', 'in', 'discard'] as const;

export type ScanAction = (typeof SCAN_ACTIONS)[number];

/** The RPC action each scan action applies (`CheckoutSampleRequest.action`). */
export const CHECKOUT_ACTION: Readonly<Record<ScanAction, CheckoutAction>> = {
  out: CheckoutAction.CHECKOUT,
  in: CheckoutAction.CHECKIN,
  discard: CheckoutAction.DISCARD,
};

export type ScanOutcome =
  /** The action succeeded. */
  | 'done'
  /** The barcode matched nothing in this lab. */
  | 'not-found'
  /** The sample exists and the action does not apply to its state. */
  | 'wrong-state'
  /** The server refused: not this user's to do, or the session is no longer valid. */
  | 'denied'
  /** This session already applied this action to this barcode; nothing was sent. */
  | 'duplicate'
  /** More than one sample carries this barcode, so nothing was changed. */
  | 'ambiguous'
  /** The request failed without an answer; the same scan can be tried again. */
  | 'unavailable';

export const OUTCOME_LABEL_KEY = {
  done: 'outcome.done',
  'not-found': 'outcome.notFound',
  'wrong-state': 'outcome.wrongState',
  denied: 'outcome.denied',
  duplicate: 'outcome.duplicate',
  ambiguous: 'outcome.ambiguous',
  unavailable: 'outcome.unavailable',
} as const;

export const OUTCOME_HINT_KEY = {
  done: 'outcome.doneHint',
  'not-found': 'outcome.notFoundHint',
  'wrong-state': 'outcome.wrongStateHint',
  denied: 'outcome.deniedHint',
  duplicate: 'outcome.duplicateHint',
  ambiguous: 'outcome.ambiguousHint',
  unavailable: 'outcome.unavailableHint',
} as const;

/**
 * Tone per outcome. "No such barcode" is `neutral` rather than `danger` on
 * purpose: a mistyped label is not an error state, and dressing it up as one
 * teaches the operator to ignore the colour that *does* mean "ask an admin".
 */
export const OUTCOME_TONE: Readonly<Record<ScanOutcome, BadgeTone>> = {
  done: 'success',
  'not-found': 'neutral',
  'wrong-state': 'warning',
  denied: 'danger',
  duplicate: 'info',
  ambiguous: 'warning',
  unavailable: 'danger',
};

/** The label key for one action, for the select and for the log's hints. */
export const ACTION_LABEL_KEY = {
  out: 'action.out',
  in: 'action.in',
  discard: 'action.discard',
} as const;

/**
 * How many rows the barcode probe asks for.
 *
 * Two, not one: the only question this screen asks of the probe is "exactly one
 * tube, or not exactly one". Barcodes are not unique per lab in the schema
 * (`samples` has no unique index on `barcode`), so asking for a second row is
 * what turns "there are more" from a guess into an answer.
 */
export const SCAN_PROBE_PAGE_SIZE = 2;

/**
 * How long the field must be quiet before auto-submit fires.
 *
 * A scanner delivers a whole barcode in a few milliseconds; a human types a
 * character every ~100 ms or more. The option is *off* by default — a scanner
 * that sends Enter needs none of this, and a timer that fires mid-burst is
 * exactly what swallows a scan (the G3.5 failure mode).
 */
export const AUTO_SUBMIT_GAP_MS = 100;

/** One line of the session log: what was scanned, what was done, what happened. */
export interface ScanLogEntry {
  /** Stable, monotonic, and unique across sessions — the React key. */
  readonly id: number;
  /** The barcode as it was scanned, trimmed. */
  readonly barcode: string;
  readonly action: ScanAction;
  readonly outcome: ScanOutcome;
  /**
   * The sample the scan was about, when one was resolved: the probe's hit, or —
   * for a duplicate — the sample the earlier scan in this session acted on.
   * `null` when the barcode resolved to nothing or to more than one sample.
   */
  readonly sample: Sample | null;
}

/** The session key of a scan: same action, same barcode, same tube. */
export function scanKey(action: ScanAction, barcode: string): string {
  return `${action}\u0000${barcode.trim()}`;
}

/**
 * What a barcode probe answered, when it did not answer with exactly one tube.
 *
 * `null` means "exactly one sample, go ahead"; the screen acts on
 * `samples[0]` only in that case.
 */
export function probeOutcome(
  samples: readonly Sample[],
): Extract<ScanOutcome, 'not-found' | 'ambiguous'> | null {
  if (samples.length === 0) {
    return 'not-found';
  }
  return samples.length === 1 ? null : 'ambiguous';
}

/**
 * What a failed `sample/checkout` or `sample/list` means to the operator.
 *
 * The gateway's statuses reach here as `ApiError`s (`src/api/errors.ts`):
 *
 *  - `NOT_FOUND` — the sample was there during the probe and is gone now, or
 *    the tube was deleted; either way it is not something to act on;
 *  - `FAILED_PRECONDITION` / `INVALID_ARGUMENT` — the state machine refused the
 *    transition (`storage::apply_checkout` throws `ConstraintViolation`, which
 *    `GrpcErrorTranslation.h` maps to `INVALID_ARGUMENT`; the MSW fake answers
 *    the same condition as `FAILED_PRECONDITION`). Both are "wrong state";
 *  - `PERMISSION_DENIED` — refused for this user, and `UNAUTHENTICATED` — the
 *    session expired or MFA is needed: both are "ask somebody", which is what
 *    `denied` says;
 *  - everything else — `UNAVAILABLE`, `INTERNAL`, a parse failure, a thrown
 *    string — is `unavailable`: nothing was applied, and the same scan can be
 *    sent again.
 */
export function outcomeForError(error: unknown): ScanOutcome {
  if (!isApiError(error)) {
    return 'unavailable';
  }
  switch (error.code) {
    case 'NOT_FOUND':
      return 'not-found';
    case 'FAILED_PRECONDITION':
    case 'INVALID_ARGUMENT':
      return 'wrong-state';
    case 'PERMISSION_DENIED':
    case 'UNAUTHENTICATED':
      return 'denied';
    default:
      return 'unavailable';
  }
}
