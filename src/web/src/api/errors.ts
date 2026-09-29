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

  constructor(code: GrpcCode, message: string, options: ApiErrorOptions = {}) {
    super(message, { cause: options.cause });
    this.name = 'ApiError';
    this.code = code;
    this.httpStatus = options.httpStatus ?? null;
    this.requestId = options.requestId ?? null;
  }

  /** True for the codes the UI must treat as "sign in again". */
  get isAuthError(): boolean {
    return this.code === 'UNAUTHENTICATED';
  }
}

export function isApiError(value: unknown): value is ApiError {
  return value instanceof ApiError;
}
