// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { ApiError } from '../../api/errors';
import { isItemTypeCycleRefusal, isPhiIndexedRefusal } from './serverErrors';

/**
 * Which server refusal the admin screen is looking at (TODO.md G3.9).
 *
 * The messages below are copied from the C++ that produces them, because the
 * gateway's error body is only `{"code", "message"}` (`RestErrorTranslation.h`)
 * — the text is the only place the cause can come from:
 *
 *  - `check_no_cycle` in `src/storage/sqlite/ItemTypeRepositories.cc` (and the
 *    Postgres equivalent) throws
 *    `ConstraintViolation("item type parent chain forms a cycle")`, and
 *    `GrpcErrorTranslation.h` maps `ConstraintViolation` → `INVALID_ARGUMENT`;
 *  - `ItemTypeServiceImpl.cc::reject_indexed_phi` throws
 *    `"a PHI custom field may not be indexed (is_phi and indexed are mutually
 *    exclusive)"` — the wording a client actually receives, because it runs
 *    before `storage::detail::validate_cfd_shape`, whose own wording is
 *    `"PHI fields may not be indexed (see L10.3)"`.
 *
 * `tests/unit/item_type_repository_test.cpp::ItemTypeRejectsCycle` and
 * `::ItemTypeRejectsSelfParent` are the server's own proof that the first one
 * is enforced in both backends; these classifiers are what turn that refusal
 * into a sentence on this screen.
 */

const failure = (code: ApiError['code'], message: string) => new ApiError(code, message);

describe('isItemTypeCycleRefusal', () => {
  it('recognises the cycle the repository refuses', () => {
    expect(
      isItemTypeCycleRefusal(failure('INVALID_ARGUMENT', 'item type parent chain forms a cycle')),
    ).toBe(true);
  });

  it('does not claim another INVALID_ARGUMENT is a cycle', () => {
    // Same code, neighbouring cause: telling the user their move closed a cycle
    // when the server was in fact rejecting a name would send them looking at
    // the wrong thing.
    expect(isItemTypeCycleRefusal(failure('INVALID_ARGUMENT', 'item type name is required'))).toBe(
      false,
    );
    expect(
      isItemTypeCycleRefusal(failure('ALREADY_EXISTS', 'item type parent chain forms a cycle')),
    ).toBe(false);
  });

  it('does not claim a value that never reached the server', () => {
    expect(isItemTypeCycleRefusal(new Error('network down'))).toBe(false);
    expect(isItemTypeCycleRefusal(null)).toBe(false);
  });
});

describe('isPhiIndexedRefusal', () => {
  it('recognises the service refusal a write receives', () => {
    expect(
      isPhiIndexedRefusal(
        failure(
          'INVALID_ARGUMENT',
          'a PHI custom field may not be indexed (is_phi and indexed are mutually exclusive)',
        ),
      ),
    ).toBe(true);
  });

  it('recognises the storage-layer wording too, in case the service check moves', () => {
    expect(
      isPhiIndexedRefusal(failure('INVALID_ARGUMENT', 'PHI fields may not be indexed (see L10.3)')),
    ).toBe(true);
  });

  it('does not match a different INVALID_ARGUMENT that mentions indexing', () => {
    expect(
      isPhiIndexedRefusal(failure('INVALID_ARGUMENT', 'indexed may only be set on a text field')),
    ).toBe(false);
  });
});
