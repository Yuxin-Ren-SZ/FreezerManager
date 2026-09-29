// SPDX-License-Identifier: AGPL-3.0-or-later
import { HttpResponse, http } from 'msw';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { LoginResponseSchema } from '../gen/fmgr/v1/auth_pb';
import { SampleSchema } from '../gen/fmgr/v1/sample_pb';
import { server } from '../test/server';
import { call, onSessionExpired, resetSessionExpiredListeners } from './client';
import { ApiError } from './errors';

/**
 * `src/api/client.ts` (TODO.md G1.2). Every branch here is one the G-arch 10
 * rule calls out: a client that only handles 200 hides `UNAUTHENTICATED`,
 * `PERMISSION_DENIED`, conflicts and network failure from every screen built on
 * top of it.
 */

const SAMPLE_LIST = '/api/v1/sample/list';
const LOGIN = '/api/v1/auth/login';

/** Read the request MSW saw, so headers and bodies can be asserted. */
function captureRequest() {
  const seen = { headers: new Headers(), body: '' };
  server.use(
    http.post(SAMPLE_LIST, async ({ request }) => {
      seen.headers = request.headers;
      seen.body = await request.text();
      return HttpResponse.json({ samples: [], page: {} });
    }),
  );
  return seen;
}

beforeEach(() => {
  resetSessionExpiredListeners();
  document.cookie = 'fmgr_csrf=; expires=Thu, 01 Jan 1970 00:00:00 GMT; path=/';
});

describe('call()', () => {
  it('POSTs the proto3 JSON body to the route path', async () => {
    const seen = captureRequest();

    const response = await call('sample/list', {
      labId: 'lab-1',
      page: { pageSize: 10, pageToken: '' },
      includeArchived: false,
    });

    expect(seen.body).toBe(JSON.stringify({ lab_id: 'lab-1', page: { page_size: 10 } }));
    expect(response.samples).toHaveLength(0);
    expect(response.$typeName).toBe('fmgr.v1.ListSamplesResponse');
  });

  it('sends the fmgr_csrf cookie value in X-CSRF-Token', async () => {
    document.cookie = 'fmgr_csrf=csrf-token-value; path=/';
    const seen = captureRequest();

    await call('sample/list', { labId: 'lab-1' });

    expect(seen.headers.get('x-csrf-token')).toBe('csrf-token-value');
  });

  it('omits X-CSRF-Token when there is no csrf cookie', async () => {
    const seen = captureRequest();

    await call('sample/list', { labId: 'lab-1' });

    expect(seen.headers.get('x-csrf-token')).toBeNull();
  });

  it('sends a fresh X-Request-Id on every call', async () => {
    const seen = captureRequest();

    await call('sample/list', { labId: 'lab-1' });
    const first = seen.headers.get('x-request-id');
    await call('sample/list', { labId: 'lab-1' });
    const second = seen.headers.get('x-request-id');

    expect(first).toMatch(/^[0-9a-f-]{36}$/);
    expect(second).toMatch(/^[0-9a-f-]{36}$/);
    expect(second).not.toBe(first);
  });

  it('parses the response with proto field names and tolerates unknown fields', async () => {
    server.use(
      http.post(SAMPLE_LIST, () =>
        HttpResponse.json({
          samples: [
            {
              id: 'sample-1',
              lab_id: 'lab-1',
              name: 'Serum A',
              custom_fields_json: '{"volume":"1"}',
              status: 'SAMPLE_STATUS_ACTIVE',
              // A newer server field this client does not know about yet.
              something_added_later: true,
            },
          ],
          page: { next_page_token: 'next' },
        }),
      ),
    );

    const response = await call('sample/list', { labId: 'lab-1' });

    expect(response.samples[0]?.name).toBe('Serum A');
    expect(response.samples[0]?.customFieldsJson).toBe('{"volume":"1"}');
    expect(response.page?.nextPageToken).toBe('next');
  });

  it('turns a {code, message} body into a typed ApiError with status and request id', async () => {
    server.use(
      http.post(SAMPLE_LIST, () =>
        HttpResponse.json(
          { code: 'PERMISSION_DENIED', message: 'missing sample.read' },
          { status: 403, headers: { 'X-Request-Id': 'server-side-id' } },
        ),
      ),
    );

    const error = await call('sample/list', { labId: 'lab-1' }).catch((e: unknown) => e);

    expect(error).toBeInstanceOf(ApiError);
    const apiError = error as ApiError;
    expect(apiError.code).toBe('PERMISSION_DENIED');
    expect(apiError.message).toBe('missing sample.read');
    expect(apiError.httpStatus).toBe(403);
    expect(apiError.requestId).toBe('server-side-id');
  });

  it('falls back to the echoed X-Request-Id when the error body carries none', async () => {
    server.use(
      http.post(SAMPLE_LIST, () =>
        HttpResponse.json({ code: 'ALREADY_EXISTS', message: 'dup' }, { status: 409 }),
      ),
    );

    const error = (await call('sample/list', { labId: 'lab-1' }).catch(
      (e: unknown) => e,
    )) as ApiError;

    expect(error.requestId).toMatch(/^[0-9a-f-]{36}$/);
  });

  it('turns an error body that is not the gateway shape into ApiError(INTERNAL)', async () => {
    server.use(http.post(SAMPLE_LIST, () => new HttpResponse('<html>502</html>', { status: 502 })));

    const error = (await call('sample/list', { labId: 'lab-1' }).catch(
      (e: unknown) => e,
    )) as ApiError;

    expect(error.code).toBe('INTERNAL');
    expect(error.httpStatus).toBe(502);
  });

  it('turns a network failure into ApiError(UNAVAILABLE) with no HTTP status', async () => {
    server.use(http.post(SAMPLE_LIST, () => HttpResponse.error()));

    const error = (await call('sample/list', { labId: 'lab-1' }).catch(
      (e: unknown) => e,
    )) as ApiError;

    expect(error).toBeInstanceOf(ApiError);
    expect(error.code).toBe('UNAVAILABLE');
    expect(error.httpStatus).toBeNull();
  });

  it('reports a 200 response whose body is not valid JSON as ApiError(INTERNAL)', async () => {
    server.use(http.post(SAMPLE_LIST, () => HttpResponse.text('nope')));

    const error = (await call('sample/list', { labId: 'lab-1' }).catch(
      (e: unknown) => e,
    )) as ApiError;

    expect(error.code).toBe('INTERNAL');
    expect(error.httpStatus).toBe(200);
  });
});

describe('session-expired listener', () => {
  it('notifies listeners on UNAUTHENTICATED', async () => {
    const listener = vi.fn();
    onSessionExpired(listener);
    server.use(
      http.post(SAMPLE_LIST, () =>
        HttpResponse.json({ code: 'UNAUTHENTICATED', message: 'expired' }, { status: 401 }),
      ),
    );

    await call('sample/list', { labId: 'lab-1' }).catch(() => undefined);

    expect(listener).toHaveBeenCalledTimes(1);
    expect((listener.mock.calls[0]?.[0] as ApiError).code).toBe('UNAUTHENTICATED');
  });

  it('does not notify listeners for other failures', async () => {
    const listener = vi.fn();
    onSessionExpired(listener);
    server.use(
      http.post(SAMPLE_LIST, () =>
        HttpResponse.json({ code: 'PERMISSION_DENIED', message: 'nope' }, { status: 403 }),
      ),
    );

    await call('sample/list', { labId: 'lab-1' }).catch(() => undefined);

    expect(listener).not.toHaveBeenCalled();
  });

  it('notifies on a 401 that never produced the gateway error shape', async () => {
    const listener = vi.fn();
    onSessionExpired(listener);
    // A reverse proxy or load balancer answers 401 itself: valid JSON, but not
    // the gateway's {"code","message"} body, so `toApiError` falls back to
    // INTERNAL. G-arch 7 says *any* 401 clears the cache.
    server.use(
      http.post(SAMPLE_LIST, () => HttpResponse.json({ error: 'unauthorized' }, { status: 401 })),
    );

    const error = (await call('sample/list', { labId: 'lab-1' }).catch(
      (caught: unknown) => caught,
    )) as ApiError;

    expect(error.code).toBe('INTERNAL');
    expect(error.httpStatus).toBe(401);
    expect(listener).toHaveBeenCalledTimes(1);
    expect(listener.mock.calls[0]?.[0]).toBe(error);
  });

  it('notifies on a 401 whose body is not JSON at all', async () => {
    const listener = vi.fn();
    onSessionExpired(listener);
    server.use(http.post(SAMPLE_LIST, () => new HttpResponse('<html>401</html>', { status: 401 })));

    await call('sample/list', { labId: 'lab-1' }).catch(() => undefined);

    expect(listener).toHaveBeenCalledTimes(1);
  });

  it('does not notify on an INTERNAL failure that is not a 401', async () => {
    const listener = vi.fn();
    onSessionExpired(listener);
    server.use(http.post(SAMPLE_LIST, () => HttpResponse.json({ error: 'boom' }, { status: 500 })));

    await call('sample/list', { labId: 'lab-1' }).catch(() => undefined);

    expect(listener).not.toHaveBeenCalled();
  });

  it('stops notifying after the returned unsubscribe runs', async () => {
    const listener = vi.fn();
    const unsubscribe = onSessionExpired(listener);
    unsubscribe();
    server.use(
      http.post(SAMPLE_LIST, () =>
        HttpResponse.json({ code: 'UNAUTHENTICATED', message: 'expired' }, { status: 401 }),
      ),
    );

    await call('sample/list', { labId: 'lab-1' }).catch(() => undefined);

    expect(listener).not.toHaveBeenCalled();
  });
});

describe('call() typing and transport details', () => {
  it('round-trips a response message through the generated schema', async () => {
    server.use(
      http.post(LOGIN, async ({ request }) => {
        expect(await request.json()).toEqual({ email: 'a@example.test', password: 'pw' });
        return HttpResponse.json({
          session_id: 's-1',
          user_id: 'u-1',
          mfa_required: true,
        });
      }),
    );

    const response = await call('auth/login', { email: 'a@example.test', password: 'pw' });

    expect(response).toMatchObject({ sessionId: 's-1', userId: 'u-1', mfaRequired: true });
    expect(LoginResponseSchema.typeName).toBe('fmgr.v1.LoginResponse');
    expect(SampleSchema.typeName).toBe('fmgr.v1.Sample');
  });

  it('treats an undecodable csrf cookie as absent instead of throwing', async () => {
    // A raw `URIError` out of `call()` would break the module's promise that
    // every failure is an ApiError.
    document.cookie = 'fmgr_csrf=100%; path=/';
    const seen = captureRequest();

    await expect(call('sample/list', { labId: 'lab-1' })).resolves.toBeDefined();

    expect(seen.headers.get('x-csrf-token')).toBeNull();
  });

  it('sends same-origin credentials so the session cookie is used', async () => {
    const seen = captureRequest();

    await call('sample/list', { labId: 'lab-1' });

    // jsdom's fetch rejects `credentials: 'include'` for cross-origin, and the
    // SPA is same-origin by design (G-arch 5), so no Origin header is sent.
    expect(seen.headers.get('origin')).toBeNull();
    expect(seen.headers.get('content-type')).toBe('application/json');
  });
});
