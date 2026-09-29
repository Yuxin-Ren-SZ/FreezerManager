// SPDX-License-Identifier: AGPL-3.0-or-later
import { isApiError } from '../../api/errors';

/**
 * Which domain refusal the server sent (TODO.md G3.9).
 *
 * The screen has its own guards for both rules, so these branches exist for the
 * case the guards cannot cover: the tree the client is holding is stale. Two
 * admins in two tabs, or the same admin in two screens, can make a move or a
 * definition legal on the client and illegal in the database — the server is
 * the authority, and its refusal has to be explained rather than shown as
 * "INVALID_ARGUMENT".
 *
 * The needles are the server's own strings, kept next to the files that emit
 * them so a reworded message is a red test here:
 *
 *  - `ItemTypeRepositories.cc::check_no_cycle` →
 *    `ConstraintViolation("item type parent chain forms a cycle")`;
 *  - `ItemTypeServiceImpl.cc::reject_indexed_phi` →
 *    `"a PHI custom field may not be indexed (is_phi and indexed are mutually
 *    exclusive)"`. `storage::detail::validate_cfd_shape` has a second wording
 *    (`"PHI fields may not be indexed (see L10.3)"`), but the service's check
 *    runs first, so it is the one a client can actually receive — both are
 *    matched, because matching only the reachable one would be a classifier
 *    that stops working the day the service check moves.
 */

const ITEM_TYPE_CYCLE = 'item type parent chain forms a cycle';

/**
 * Both sentences that mean "an index on a PHI field", quoted in full: matching
 * a fragment like `may not be indexed` would also catch an unrelated refusal
 * that happens to share the phrase.
 */
const PHI_INDEXED = [
  'a PHI custom field may not be indexed (is_phi and indexed are mutually exclusive)',
  'PHI fields may not be indexed (see L10.3)',
];

/** `INVALID_ARGUMENT` from `check_no_cycle`: the move would close a cycle. */
export function isItemTypeCycleRefusal(error: unknown): boolean {
  return (
    isApiError(error) &&
    error.code === 'INVALID_ARGUMENT' &&
    error.message.includes(ITEM_TYPE_CYCLE)
  );
}

/** `INVALID_ARGUMENT` from `reject_indexed_phi`: an index on a PHI field. */
export function isPhiIndexedRefusal(error: unknown): boolean {
  return (
    isApiError(error) &&
    error.code === 'INVALID_ARGUMENT' &&
    PHI_INDEXED.some((sentence) => error.message.includes(sentence))
  );
}
