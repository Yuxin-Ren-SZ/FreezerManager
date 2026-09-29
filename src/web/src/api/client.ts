// SPDX-License-Identifier: AGPL-3.0-or-later
import {
  create,
  fromJson,
  toJson,
  type JsonValue,
  type MessageInitShape,
} from '@bufbuild/protobuf';
import { ApiError, isGrpcCode } from './errors';
import {
  apiRoutes,
  type ApiRoutes,
  type RequestInitOf,
  type ResponseOf,
  type RpcName,
} from './routes';

/**
 * The one transport wrapper every screen goes through (TODO.md G1.2, G-arch 5).
 *
 * The gateway speaks proto3 JSON with `preserve_proto_field_names`
 * (`src/rest/JsonProtoMapping.cc`), so this module is the single place that
 * knows the wire format:
 *
 *   - requests are serialized with `{ useProtoFieldName: true }`;
 *   - responses are parsed with `{ ignoreUnknownFields: true }`, so a server
 *     that adds a field does not break an older bundle;
 *   - every call carries a fresh `X-Request-Id` and, when the `fmgr_csrf`
 *     cookie is present, that value in `X-CSRF-Token` (G0.1: cookie-auth
 *     mutations are CSRF-checked).
 *
 * Failures are always thrown as `ApiError`, never a raw `Response` or
 * `TypeError`, so a screen has exactly one thing to handle.
 */

/** Cookie the gateway sets for the CSRF double-submit check (G0.1). */
export const CSRF_COOKIE_NAME = 'fmgr_csrf';

/** Header carrying the CSRF token (G0.1). */
export const CSRF_HEADER_NAME = 'X-CSRF-Token';

/** Header carrying the correlation id (C-12, PRD §17). */
export const REQUEST_ID_HEADER_NAME = 'X-Request-Id';

/**
 * Reads a cookie by name. `document.cookie` is the only way to see
 * `fmgr_csrf`: it is deliberately *not* `HttpOnly`, unlike `fmgr_session`,
 * which JavaScript must never be able to read (G-arch 6).
 */
export function readCookie(name: string): string | null {
  for (const part of document.cookie.split(';')) {
    const trimmed = part.trim();
    if (trimmed.startsWith(`${name}=`)) {
      return decodeURIComponent(trimmed.slice(name.length + 1));
    }
  }
  return null;
}

/**
 * A UUID v4 for correlation. `crypto.randomUUID` needs a secure context; the
 * dev loop runs on `127.0.0.1`, which qualifies, but a self-hosted instance
 * reached over plain HTTP on a LAN address does not, so fall back to
 * `getRandomValues` rather than throwing on every call.
 */
export function newRequestId(): string {
  if (typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID();
  }
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const hex = [...bytes].map((byte) => byte.toString(16).padStart(2, '0')).join('');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

type SessionExpiredListener = (error: ApiError) => void;

const sessionExpiredListeners = new Set<SessionExpiredListener>();

/**
 * Register a callback for "the session is gone" (G-arch 7: logout, session
 * expiry and any 401 clear the TanStack Query cache). Returns its own
 * unsubscribe; `client.ts` intentionally does not know about React.
 */
export function onSessionExpired(listener: SessionExpiredListener): () => void {
  sessionExpiredListeners.add(listener);
  return () => {
    sessionExpiredListeners.delete(listener);
  };
}

/**
 * Drop every listener. Only for test teardown — the module keeps its registry
 * across tests in one file, and an assertion that nobody was notified must not
 * see a listener a previous test left behind.
 */
export function resetSessionExpiredListeners(): void {
  sessionExpiredListeners.clear();
}

function notifySessionExpired(error: ApiError): void {
  for (const listener of sessionExpiredListeners) {
    listener(error);
  }
}

/** The gateway's error body: `{"code": "<GRPC_CODE>", "message": "..."}`. */
function toApiError(json: unknown, response: Response, fallbackRequestId: string): ApiError {
  const requestId = response.headers.get(REQUEST_ID_HEADER_NAME) ?? fallbackRequestId;
  const body = json as { code?: unknown; message?: unknown } | null;

  if (isGrpcCode(body?.code)) {
    return new ApiError(body.code, typeof body.message === 'string' ? body.message : '', {
      httpStatus: response.status,
      requestId,
    });
  }

  return new ApiError(
    'INTERNAL',
    `unexpected HTTP ${String(response.status)} from ${response.url || 'the gateway'}`,
    { httpStatus: response.status, requestId },
  );
}

/**
 * Call one unary RPC and get its typed response.
 *
 * `call('sample/list', { labId })` is typed end to end from the generated
 * schemas in `routes.ts`: the argument is the request message's init shape and
 * the result is the response message.
 */
export async function call<K extends RpcName>(
  rpc: K,
  request: RequestInitOf<K> = {} as RequestInitOf<K>,
): Promise<ResponseOf<K>> {
  const route: ApiRoutes[K] = apiRoutes[rpc];
  const requestId = newRequestId();

  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    [REQUEST_ID_HEADER_NAME]: requestId,
  };
  // Only cookie-authenticated calls need it, and only the browser knows whether
  // the cookie is there. Sending it when absent would be a header with an empty
  // value, which the gateway would (correctly) reject as a mismatch.
  const csrfToken = readCookie(CSRF_COOKIE_NAME);
  if (csrfToken !== null && csrfToken !== '') {
    headers[CSRF_HEADER_NAME] = csrfToken;
  }

  const body = toJson(
    route.input,
    create(route.input, request as MessageInitShape<typeof route.input>),
    {
      useProtoFieldName: true,
    },
  );

  let response: Response;
  try {
    response = await fetch(route.path, {
      // Unary routes are POST by construction: the `FMGR_ROUTE` macro in
      // src/rest/RestGateway.cc bakes in `{drogon::Post}`, and
      // scripts/check-routes.mjs fails if that changes.
      method: 'POST',
      headers,
      body: JSON.stringify(body),
      // Same-origin only (G-arch 5); the session cookie rides along because the
      // SPA is served by the same origin as the gateway.
      credentials: 'same-origin',
    });
  } catch (cause) {
    throw new ApiError('UNAVAILABLE', 'the server could not be reached', { requestId, cause });
  }

  const text = await response.text();
  let json: JsonValue = {};
  if (text !== '') {
    try {
      json = JSON.parse(text) as JsonValue;
    } catch (cause) {
      throw new ApiError('INTERNAL', `response was not JSON (HTTP ${String(response.status)})`, {
        httpStatus: response.status,
        requestId: response.headers.get(REQUEST_ID_HEADER_NAME) ?? requestId,
        cause,
      });
    }
  }

  if (!response.ok) {
    const error = toApiError(json, response, requestId);
    if (error.code === 'UNAUTHENTICATED') {
      notifySessionExpired(error);
    }
    throw error;
  }

  try {
    return fromJson(route.output, json, { ignoreUnknownFields: true }) as ResponseOf<K>;
  } catch (cause) {
    throw new ApiError('INTERNAL', 'response body did not match the expected message', {
      httpStatus: response.status,
      requestId: response.headers.get(REQUEST_ID_HEADER_NAME) ?? requestId,
      cause,
    });
  }
}
