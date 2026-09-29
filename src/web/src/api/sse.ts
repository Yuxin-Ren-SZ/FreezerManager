// SPDX-License-Identifier: AGPL-3.0-or-later
import { fromJson, type DescMessage, type JsonValue, type MessageShape } from '@bufbuild/protobuf';
import { ApiError, isGrpcCode, type GrpcCode } from './errors';

/**
 * The SSE half of the API layer (TODO.md G1.2): typed frames over
 * `EventSource` for the gateway's `…/watch` routes (G-arch 5).
 *
 * Four decisions worth knowing about:
 *
 * 1. **The stream authenticates with the session cookie, never a URL token.**
 *    `EventSource` cannot set request headers, so the gateway used to accept a
 *    bearer in `?access_token=`; G0.1 removed that, because a token in a URL
 *    ends up in proxy and access logs. The credential is now the `HttpOnly`
 *    `fmgr_session` cookie, which the browser attaches to a same-origin
 *    `EventSource` on its own — so nothing here ever holds, reads or appends a
 *    token, and the URL carries ids and cursors only (G-arch 6).
 * 2. **The retry loop is ours, not the browser's.** `EventSource` reconnects on
 *    its own and resends `Last-Event-ID`, but that retry has no cap and no way
 *    to say "this error is permanent". So we close the stream on error and
 *    reopen it ourselves with a capped backoff, carrying the same cursor
 *    explicitly as `?since=` — which is the parameter the gateway reads when
 *    `Last-Event-ID` is absent (`RestGateway.cc`, the audit/sample watch
 *    handlers). `Last-Event-ID` still wins if a browser-level retry happens
 *    first.
 * 3. **`event: error` is data, not a transport failure.** The gateway sends it
 *    as a frame with the same `{"code","message"}` body as a unary error
 *    (`SseBridge.h`), so it arrives as a `MessageEvent` with `data`; a real
 *    transport failure arrives as a plain `Event` with none. That is how the
 *    two are told apart.
 * 4. **Fail fast on a typo.** Each route declares the query parameters it
 *    accepts, and an unknown one throws at subscribe time rather than being
 *    silently ignored by the gateway (which ignores unknown query params).
 */

/** The subset of `EventSource` this module needs, so tests can supply a fake. */
export interface EventSourceLike {
  addEventListener(type: string, listener: (event: Event) => void): void;
  removeEventListener(type: string, listener: (event: Event) => void): void;
  close(): void;
}

export interface SseRoute {
  readonly path: `/api/v1/${string}`;
  /** Query parameters the gateway's handler reads for this feed. */
  readonly params: readonly string[];
}

/**
 * The streaming routes, mirroring the `registerHandler(… "/watch", …, {drogon::Get})`
 * calls in `src/rest/RestGateway.cc`. `scripts/check-routes.mjs` fails when the
 * two drift apart.
 */
export const sseRoutes = {
  'audit/watch': {
    path: '/api/v1/audit/watch',
    params: ['lab_id', 'entity_kind', 'entity_id', 'since'],
  },
  'sample/watch': {
    path: '/api/v1/sample/watch',
    params: ['lab_id', 'box_id', 'item_type_id', 'since'],
  },
} as const satisfies Record<string, SseRoute>;

export type SseRouteName = keyof typeof sseRoutes;

/** The query parameters one feed accepts, so a typo fails to compile. */
export type SseParamsOf<N extends SseRouteName> = Partial<
  Record<(typeof sseRoutes)[N]['params'][number], string>
>;

/** One decoded frame. `id` is the stream cursor (`Last-Event-ID` / `?since=`). */
export interface SseFrame<T> {
  readonly event: string;
  readonly id: string | null;
  readonly data: T;
}

export interface SseRetryPolicy {
  readonly baseDelayMs: number;
  readonly maxDelayMs: number;
}

export const DEFAULT_SSE_RETRY: SseRetryPolicy = { baseDelayMs: 1_000, maxDelayMs: 30_000 };

/**
 * Exponential backoff, capped. Deterministic on purpose: jitter would spread a
 * thundering herd, but a self-hosted instance has a handful of clients and an
 * exact delay is something a test can assert.
 */
export function reconnectDelayMs(attempt: number, baseDelayMs: number, maxDelayMs: number): number {
  return Math.min(maxDelayMs, baseDelayMs * 2 ** Math.max(0, attempt));
}

/**
 * Codes that will not fix themselves: reconnecting into them turns a permission
 * problem into a request loop against the server.
 */
const TERMINAL_CODES: ReadonlySet<GrpcCode> = new Set<GrpcCode>([
  'UNAUTHENTICATED',
  'PERMISSION_DENIED',
]);

export interface SseSubscribeOptions<Desc extends DescMessage, N extends SseRouteName> {
  /** Generated schema for one frame's `data`. */
  readonly schema: Desc;
  readonly onFrame: (frame: SseFrame<MessageShape<Desc>>) => void;
  readonly onError?: (error: ApiError) => void;
  readonly onOpen?: () => void;
  /** Query parameters; a name the route does not take is a type error. */
  readonly params?: SseParamsOf<N>;
  readonly reconnect?: Partial<SseRetryPolicy>;
  /** Test seam; defaults to the browser's `EventSource`. */
  readonly eventSourceFactory?: (url: string) => EventSourceLike;
}

function buildUrl(
  name: SseRouteName,
  params: Readonly<Record<string, string>>,
  since: string | null,
): string {
  const route: SseRoute = sseRoutes[name];
  const search = new URLSearchParams();

  for (const [key, value] of Object.entries(params)) {
    if (!route.params.includes(key)) {
      throw new Error(
        `sse: unknown parameter "${key}" for ${route.path} — it accepts ${route.params.join(', ')}`,
      );
    }
    search.set(key, value);
  }
  if (since !== null && since !== '' && !search.has('since')) {
    search.set('since', since);
  }

  const query = search.toString();
  return query === '' ? route.path : `${route.path}?${query}`;
}

/**
 * Subscribe to one streaming feed. Returns the cleanup function: call it on
 * unmount, and it closes the stream and cancels any pending reconnect.
 */
export function subscribeSse<Desc extends DescMessage, N extends SseRouteName>(
  name: N,
  options: SseSubscribeOptions<Desc, N>,
): () => void {
  const factory = options.eventSourceFactory ?? ((url: string) => new EventSource(url));
  const base = options.reconnect?.baseDelayMs ?? DEFAULT_SSE_RETRY.baseDelayMs;
  const max = options.reconnect?.maxDelayMs ?? DEFAULT_SSE_RETRY.maxDelayMs;
  const params = options.params ?? {};

  let source: EventSourceLike | null = null;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let attempt = 0;
  let lastEventId: string | null = null;
  let stopped = false;

  const closeSource = () => {
    source?.close();
    source = null;
  };

  const fail = (error: ApiError) => {
    options.onError?.(error);
  };

  const scheduleReconnect = () => {
    if (stopped) return;
    const delay = reconnectDelayMs(attempt, base, max);
    attempt += 1;
    timer = setTimeout(() => {
      timer = null;
      connect();
    }, delay);
  };

  const parseFrame = (raw: string): MessageShape<Desc> | null => {
    let json: JsonValue;
    try {
      json = JSON.parse(raw) as JsonValue;
    } catch (cause) {
      fail(new ApiError('INTERNAL', 'malformed SSE frame', { cause }));
      return null;
    }
    try {
      return fromJson(options.schema, json, { ignoreUnknownFields: true });
    } catch (cause) {
      fail(new ApiError('INTERNAL', 'SSE frame did not match the expected message', { cause }));
      return null;
    }
  };

  const onMessage = (event: Event) => {
    if (stopped) return;
    const message = event as MessageEvent<string>;
    if (message.lastEventId !== '') {
      lastEventId = message.lastEventId;
    }
    const data = parseFrame(message.data);
    if (data !== null) {
      options.onFrame({ event: 'message', id: lastEventId, data });
    }
  };

  const onError = (event: Event) => {
    if (stopped) return;
    const data = (event as MessageEvent<string>).data;

    if (typeof data === 'string' && data !== '') {
      // A server-sent `event: error` frame: an ApiError, and permanent for the
      // codes that describe the caller rather than the connection.
      let body: { code?: unknown; message?: unknown } = {};
      try {
        body = JSON.parse(data) as typeof body;
      } catch {
        // Fall through to the INTERNAL default below.
      }
      const code = isGrpcCode(body.code) ? body.code : 'INTERNAL';
      fail(new ApiError(code, typeof body.message === 'string' ? body.message : ''));
      if (TERMINAL_CODES.has(code)) {
        stopped = true;
        closeSource();
        return;
      }
    } else {
      fail(new ApiError('UNAVAILABLE', 'the live connection was lost'));
    }

    closeSource();
    scheduleReconnect();
  };

  const connect = () => {
    if (stopped) return;
    const created = factory(buildUrl(name, params, lastEventId));
    source = created;
    created.addEventListener('open', () => {
      if (stopped) return;
      attempt = 0;
      options.onOpen?.();
    });
    created.addEventListener('message', onMessage);
    created.addEventListener('error', onError);
  };

  connect();

  return () => {
    stopped = true;
    if (timer !== null) {
      clearTimeout(timer);
      timer = null;
    }
    closeSource();
  };
}
