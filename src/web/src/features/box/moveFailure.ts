// SPDX-License-Identifier: AGPL-3.0-or-later
import { isApiError, type GrpcCode } from '../../api/errors';

/**
 * Why a `sample/move` was refused (TODO.md G3.4).
 *
 * The acceptance criterion is a distinction, not a message: *"'size mismatch'
 * and 'position taken' (`ALREADY_EXISTS`) are different failures and must be
 * distinguishable in the toast. They have different fixes for the user; a
 * single generic toast loses that."* A sample that will not fit needs a
 * different position; a position that is taken needs the sample already there
 * to move first. So this returns *which* failure it was, and the screen picks
 * the words.
 *
 * The two causes are readable from the gateway's `{"code", "message"}` body
 * (`RestErrorTranslation.h`) the same way G3.3's form does it
 * (`src/features/sample-detail/serverErrors.ts`):
 *
 *  - `ALREADY_EXISTS` is `UniqueViolation` → `samples_position_unique` on
 *    `(box_id, position_label)`. On a move it has no other meaning;
 *  - the size-class rule is `validate_sample()`'s
 *    `container_type size_class is not accepted at this box position`, checked
 *    against `box_type_position_accepts` in both backends. It arrives as
 *    `INVALID_ARGUMENT`, as does the neighbouring `position_label does not
 *    exist in this box's BoxType` — which is *not* a size problem, and telling
 *    the user their tube does not fit would send them looking for the wrong fix.
 *
 * Anything else stays `other`: the toast then says the move was refused and
 * names the status code, rather than guessing a cause it cannot know.
 * `moveFailure.test.ts` pins the two classifiers together so a needle changed on
 * one side cannot leave the other claiming the wrong cause.
 */

export type MoveFailureKind = 'position-taken' | 'size-mismatch' | 'other';

export interface MoveFailure {
  readonly kind: MoveFailureKind;
  /** The gateway's status, or `null` when the thrown value was not an `ApiError`. */
  readonly code: GrpcCode | null;
}

/** `validate_sample()` in `SampleRepositories.cc`, verbatim. */
const SIZE_CLASS_REJECTED = 'container_type size_class is not accepted at this box position';

export function classifyMoveFailure(error: unknown): MoveFailure {
  if (!isApiError(error)) {
    return { kind: 'other', code: null };
  }
  if (error.code === 'ALREADY_EXISTS') {
    return { kind: 'position-taken', code: error.code };
  }
  if (error.code === 'INVALID_ARGUMENT' && error.message.includes(SIZE_CLASS_REJECTED)) {
    return { kind: 'size-mismatch', code: error.code };
  }
  return { kind: 'other', code: error.code };
}
