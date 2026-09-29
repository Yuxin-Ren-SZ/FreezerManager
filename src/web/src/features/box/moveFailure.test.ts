// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { ApiError } from '../../api/errors';
import { mapServerFailure } from '../sample-detail/serverErrors';
import { classifyMoveFailure } from './moveFailure';

/**
 * Why a `sample/move` was refused (TODO.md G3.4).
 *
 * The acceptance criterion is a *distinction*: *"'size mismatch' and 'position
 * taken' (`ALREADY_EXISTS`) are different failures and must be distinguishable
 * in the toast. They have different fixes for the user."* Collapsing them into
 * one message is the failure mode this file exists to catch, so every test here
 * asserts which of the two a rejection is — and the last one asserts that the
 * third case does not get labelled as either.
 *
 * The server text is a mirrored contract, exactly as in
 * `src/features/sample-detail/serverErrors.ts`: the gateway's error body is only
 * `{"code", "message"}` (`RestErrorTranslation.h`), so the message is the only
 * place the cause can come from. The last test in this file pins the two
 * classifiers together, so a needle changed on one side cannot silently leave
 * the other claiming the wrong cause.
 */

const UNIQUE_VIOLATION =
  'execute sqlite sample statement: UNIQUE constraint failed: samples.box_id, samples.position_label';
const SIZE_CLASS = 'container_type size_class is not accepted at this box position';
const UNKNOWN_POSITION = "position_label does not exist in this box's BoxType";

describe('classifyMoveFailure', () => {
  it('calls a uniqueness violation a taken position', () => {
    expect(classifyMoveFailure(new ApiError('ALREADY_EXISTS', UNIQUE_VIOLATION))).toEqual({
      kind: 'position-taken',
      code: 'ALREADY_EXISTS',
    });
  });

  it('calls a rejected size class a size mismatch', () => {
    expect(classifyMoveFailure(new ApiError('INVALID_ARGUMENT', SIZE_CLASS))).toEqual({
      kind: 'size-mismatch',
      code: 'INVALID_ARGUMENT',
    });
  });

  it('does not read the size mismatch out of the wrong INVALID_ARGUMENT', () => {
    // The neighbouring rejection from the same `validate_sample()`: a position
    // that the destination box type does not declare. It is not a size problem,
    // and telling the user their tube does not fit would send them shopping.
    expect(classifyMoveFailure(new ApiError('INVALID_ARGUMENT', UNKNOWN_POSITION))).toEqual({
      kind: 'other',
      code: 'INVALID_ARGUMENT',
    });
  });

  it('keeps an unrecognised failure as other rather than guessing a cause', () => {
    expect(classifyMoveFailure(new ApiError('PERMISSION_DENIED', 'denied'))).toEqual({
      kind: 'other',
      code: 'PERMISSION_DENIED',
    });
    expect(classifyMoveFailure(new Error('not an ApiError'))).toEqual({
      kind: 'other',
      code: null,
    });
  });

  it('agrees with the G3.3 form classifier on both rejections', () => {
    // Same server, same two failures, two presentations: the box view raises a
    // toast, the sample form pins the message to a field. Both read the message
    // text, so this is the guard against the two drifting apart.
    const taken = new ApiError('ALREADY_EXISTS', UNIQUE_VIOLATION);
    const mismatch = new ApiError('INVALID_ARGUMENT', SIZE_CLASS);

    expect(classifyMoveFailure(taken).kind).toBe('position-taken');
    expect(mapServerFailure(taken, 'move').fields.positionLabel).toBeDefined();

    expect(classifyMoveFailure(mismatch).kind).toBe('size-mismatch');
    expect(mapServerFailure(mismatch, 'move').fields.containerTypeId).toBeDefined();

    // ...and neither classifier blames the container type where there is no
    // size problem: the neighbouring `position_label` rejection is a different
    // failure, and telling the user their tube does not fit would be a lie.
    const unknown = new ApiError('INVALID_ARGUMENT', UNKNOWN_POSITION);
    expect(classifyMoveFailure(unknown).kind).toBe('other');
    expect(mapServerFailure(unknown, 'move').fields.containerTypeId).toBeUndefined();
  });
});
