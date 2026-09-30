// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { ApiError, isApiError, isMfaRequired, MFA_REQUIRED_PREFIX } from './errors';

/**
 * `auth::MfaRequired` is the one refusal whose gRPC code does not identify the
 * state: a session with its second factor outstanding is `UNAUTHENTICATED`,
 * exactly like an expired or revoked one. The SPA has to *resume* the TOTP step
 * for the first and re-authenticate for the others, so `isMfaRequired()` has to
 * recognise both shapes the gateway has used — the envelope code (#140) and the
 * message marker that predates it.
 */
function unauthorised(message: string, mfaRequired = false): ApiError {
  return new ApiError('UNAUTHENTICATED', message, { httpStatus: 401, mfaRequired });
}

describe('isMfaRequired', () => {
  it('recognises the gateway envelope code, whatever the message says', () => {
    expect(isMfaRequired(unauthorised('MFA required before this operation', true))).toBe(true);
  });

  it('recognises the message marker a gateway without the envelope code sends', () => {
    expect(isMfaRequired(unauthorised(`${MFA_REQUIRED_PREFIX} MFA required`))).toBe(true);
  });

  it('does not mistake an expired session for a pending second factor', () => {
    // The distinction the whole state machine rests on: same code, same status,
    // different screen.
    expect(isMfaRequired(unauthorised('invalid credentials'))).toBe(false);
    expect(isMfaRequired(new ApiError('UNAVAILABLE', 'the server could not be reached'))).toBe(
      false,
    );
    expect(isMfaRequired(new Error(`${MFA_REQUIRED_PREFIX} not an ApiError`))).toBe(false);
    expect(isMfaRequired(undefined)).toBe(false);
  });

  it('keeps the message, so a log still says which refusal it was', () => {
    const error = unauthorised(`${MFA_REQUIRED_PREFIX} MFA required before this operation`);

    expect(error.message).toBe(`${MFA_REQUIRED_PREFIX} MFA required before this operation`);
    expect(error.httpStatus).toBe(401);
    expect(error.isAuthError).toBe(true);
    expect(isApiError(error)).toBe(true);
  });

  it('defaults the flag off, so an ordinary 401 is not a pending login', () => {
    expect(new ApiError('UNAUTHENTICATED', 'nope').mfaRequired).toBe(false);
  });
});
