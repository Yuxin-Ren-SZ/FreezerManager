// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * Every gRPC status name the gateway can put in a `{"code", "message"}` body —
 * the names `grpc_code_name()` emits in `src/rest/RestErrorTranslation.h`.
 * Kept as a runtime list (not just a type) so `isGrpcCode()` can reject a body
 * that is not the gateway's error shape at all.
 */
export const GRPC_CODES = [
  'OK',
  'CANCELLED',
  'UNKNOWN',
  'INVALID_ARGUMENT',
  'DEADLINE_EXCEEDED',
  'NOT_FOUND',
  'ALREADY_EXISTS',
  'PERMISSION_DENIED',
  'RESOURCE_EXHAUSTED',
  'FAILED_PRECONDITION',
  'ABORTED',
  'OUT_OF_RANGE',
  'UNIMPLEMENTED',
  'INTERNAL',
  'UNAVAILABLE',
  'UNAUTHENTICATED',
] as const;

export type GrpcCode = (typeof GRPC_CODES)[number];

const GRPC_CODE_SET: ReadonlySet<string> = new Set<string>(GRPC_CODES);

export function isGrpcCode(value: unknown): value is GrpcCode {
  return typeof value === 'string' && GRPC_CODE_SET.has(value);
}

/** Extra facts about a failure that a screen may want to show or act on. */
export interface ApiErrorOptions {
  /** HTTP status, or `null` when the request never reached the server. */
  httpStatus?: number | null;
  /** Correlation id: the gateway's `X-Request-Id`, else the one we sent. */
  requestId?: string | null;
  /** True when the refusal means "a session, but its second factor is outstanding". */
  mfaRequired?: boolean;
  cause?: unknown;
}

/**
 * One failure type for everything the API layer can go wrong with (TODO.md
 * G1.2): a gRPC status the gateway translated, a network failure
 * (`UNAVAILABLE`), or a response that was not the shape we expected
 * (`INTERNAL`). Screens switch on `code`; `helpers.apiErrorMessage()` turns the
 * whole thing into a translated sentence.
 */
export class ApiError extends Error {
  readonly code: GrpcCode;
  readonly httpStatus: number | null;
  readonly requestId: string | null;
  /**
   * `auth::MfaRequired` on the wire: a session exists but its TOTP code is
   * still outstanding, so the SPA resumes the code prompt instead of signing
   * the user out.
   *
   * A flag rather than a `code`, because the *gRPC* code deliberately stays
   * `UNAUTHENTICATED` — no RPC's authentication semantics change — and what
   * identifies the state is the gateway's envelope, not the status. See
   * `isMfaRequired()`.
   */
  readonly mfaRequired: boolean;

  constructor(code: GrpcCode, message: string, options: ApiErrorOptions = {}) {
    super(message, { cause: options.cause });
    this.name = 'ApiError';
    this.code = code;
    this.httpStatus = options.httpStatus ?? null;
    this.requestId = options.requestId ?? null;
    this.mfaRequired = options.mfaRequired ?? false;
  }

  /** True for the codes the UI must treat as "sign in again". */
  get isAuthError(): boolean {
    return this.code === 'UNAUTHENTICATED';
  }
}

export function isApiError(value: unknown): value is ApiError {
  return value instanceof ApiError;
}

/**
 * The marker `GrpcErrorTranslation.h` prefixes the gRPC status message with for
 * an `auth::MfaRequired` refusal (`rpc::k_mfa_required_marker`).
 */
export const MFA_REQUIRED_PREFIX = 'mfa_required:';

/**
 * The envelope code `RestErrorTranslation.h` puts in the `{"code","message"}`
 * body of that same refusal, next to HTTP 401 (`rpc::k_mfa_required_code`,
 * #140).
 *
 * It exists because a sentence is not an interface: `UNAUTHENTICATED` is what an
 * expired or revoked session gets too, and the SPA has to *resume* the TOTP step
 * for one of those while it re-authenticates for the others.
 */
export const MFA_REQUIRED_ENVELOPE_CODE = 'MFA_REQUIRED';

/**
 * Whether a failure is `auth::MfaRequired` — a session whose second factor is
 * still outstanding, which is a resumable state, not a sign-out.
 *
 * Both shapes are accepted on purpose. The envelope code is what the gateway
 * sends once #140 lands; the message prefix is what it sends before that, and
 * what a gRPC-level client or a log still sees afterwards. Tolerating both means
 * that whichever of the two merges first, "enter your code" cannot silently
 * become "sign in again" — the state a demo would dead-end on.
 */
export function isMfaRequired(cause: unknown): boolean {
  if (!(cause instanceof ApiError)) {
    return false;
  }
  if (cause.mfaRequired) {
    return true;
  }
  return cause.code === 'UNAUTHENTICATED' && cause.message.startsWith(MFA_REQUIRED_PREFIX);
}
