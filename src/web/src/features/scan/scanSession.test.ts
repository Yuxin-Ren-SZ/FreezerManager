// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import { ApiError } from '../../api/errors';
import { CheckoutAction, SampleSchema, SampleStatus } from '../../gen/fmgr/v1/sample_pb';
import { CHECKOUT_ACTION, outcomeForError, probeOutcome, scanKey } from './scanSession';

/**
 * The scan session's rules as values (TODO.md G3.6).
 *
 * Two of them are the ones a screen gets wrong without noticing, so they are
 * pinned here rather than only through the DOM:
 *
 *  - **the four outcomes never collapse.** "The server is unreachable" is not
 *    "this barcode does not exist", and "not allowed" is not "wrong state" —
 *    each one is a different instruction to the person holding the tube;
 *  - **a duplicate is decided by the session, not by the server.** The key that
 *    decides it includes the action, because checking a tube out and then
 *    checking the same tube back in is a reversal, not a double-apply.
 */

const sample = (overrides: { id: string; barcode: string; status?: SampleStatus }) =>
  create(SampleSchema, {
    labId: 'lab-demo',
    itemTypeId: 'it-serum',
    name: overrides.id,
    status: SampleStatus.ACTIVE,
    ...overrides,
  });

describe('CHECKOUT_ACTION', () => {
  it('maps each scan action to the action the server applies', () => {
    expect(CHECKOUT_ACTION).toEqual({
      out: CheckoutAction.CHECKOUT,
      in: CheckoutAction.CHECKIN,
      discard: CheckoutAction.DISCARD,
    });
  });
});

describe('probeOutcome', () => {
  it('resolves a barcode that matches exactly one sample', () => {
    expect(probeOutcome([sample({ id: 'sample-1', barcode: 'DEMO-0001' })])).toBeNull();
  });

  it('reports a barcode that matched nothing as not found', () => {
    expect(probeOutcome([])).toBe('not-found');
  });

  it('refuses to guess when one barcode resolves to several samples', () => {
    // Barcodes are not unique per lab in the schema, so this is a real answer
    // rather than a defensive branch: acting on the first of three tubes would
    // change a sample the operator is not holding.
    expect(
      probeOutcome([
        sample({ id: 'sample-1', barcode: 'DEMO-0001' }),
        sample({ id: 'sample-2', barcode: 'DEMO-0001' }),
      ]),
    ).toBe('ambiguous');
  });
});

describe('outcomeForError', () => {
  it('keeps not found, wrong state, denied and unavailable apart', () => {
    expect(outcomeForError(new ApiError('NOT_FOUND', 'no such sample'))).toBe('not-found');
    expect(outcomeForError(new ApiError('FAILED_PRECONDITION', 'wrong state'))).toBe('wrong-state');
    expect(outcomeForError(new ApiError('INVALID_ARGUMENT', 'wrong state'))).toBe('wrong-state');
    expect(outcomeForError(new ApiError('PERMISSION_DENIED', 'nope'))).toBe('denied');
    expect(outcomeForError(new ApiError('UNAVAILABLE', 'offline'))).toBe('unavailable');

    // A refusal is never a state problem and vice versa — the operator's next
    // move differs (ask an admin vs. look at the tube).
    expect(outcomeForError(new ApiError('PERMISSION_DENIED', 'nope'))).not.toBe(
      outcomeForError(new ApiError('FAILED_PRECONDITION', 'wrong state')),
    );
  });

  it('treats an expired session as a refusal rather than as a missing barcode', () => {
    expect(outcomeForError(new ApiError('UNAUTHENTICATED', 'session expired'))).toBe('denied');
  });

  it('calls anything it cannot classify unavailable, never not found', () => {
    // The G3.5 lesson: turning "the server is unreachable" into "this barcode
    // does not exist" is the worst possible answer at a freezer.
    expect(outcomeForError(new ApiError('INTERNAL', 'boom'))).toBe('unavailable');
    expect(outcomeForError(new ApiError('ABORTED', 'retry'))).toBe('unavailable');
    expect(outcomeForError(new Error('not an ApiError'))).toBe('unavailable');
    expect(outcomeForError('a thrown string')).toBe('unavailable');
  });
});

describe('scanKey', () => {
  const key = scanKey('out', 'DEMO-0001');

  it('ignores whitespace a scanner or a keyboard adds around the barcode', () => {
    expect(scanKey('out', '  DEMO-0001\t')).toBe(key);
  });

  it('is per action: checking the same tube back in is not a duplicate', () => {
    expect(scanKey('in', 'DEMO-0001')).not.toBe(key);
    expect(scanKey('discard', 'DEMO-0001')).not.toBe(key);
  });

  it('keeps the barcode exact, because the server filter is case-sensitive', () => {
    expect(scanKey('out', 'demo-0001')).not.toBe(key);
  });
});
