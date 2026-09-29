// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { call } from '../api/client';
import { ApiError, GRPC_CODES } from '../api/errors';
import { apiRoutes, type RpcName } from '../api/routes';
import { subscribeSse } from '../api/sse';
import { SampleSchema, SampleStatus } from '../gen/fmgr/v1/sample_pb';
import { FakeEventSource, fakeEventSource } from './fakeEventSource';
import { createDemoLab, fakeApi, HTTP_STATUS_FOR } from './fakeApi';
import { server } from './server';

/**
 * The fake has to be able to fail (TODO.md G1.2, AGENTS.md §6). These tests
 * check the two properties that make it trustworthy: a handler for *every*
 * route, and a working fault for *every* route — not just the two or three a
 * happy-path screen happens to use.
 */

const routeNames = Object.keys(apiRoutes) as RpcName[];

/** `call()` is generic over the route key; these tests iterate all of them. */
const callAny = (rpc: RpcName, body: unknown) =>
  (call as unknown as (rpc: RpcName, request: unknown) => Promise<unknown>)(rpc, body);

/** Statuses an implemented handler can answer with when sent an empty body. */
const HANDLED_STATUSES = [200, 400, 404, 412];

describe('fakeApi coverage', () => {
  it('answers every route in routes.ts (an unhandled route would throw in MSW)', async () => {
    server.use(...fakeApi());

    expect(routeNames.length).toBeGreaterThan(0);
    for (const rpc of routeNames) {
      const response = await fetch(apiRoutes[rpc].path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: '{}',
      });
      expect(HANDLED_STATUSES, `${rpc} (${apiRoutes[rpc].path})`).toContain(response.status);
    }
  });

  it('injects a failure for every route, with the status the gateway would use', async () => {
    server.use(
      ...fakeApi({ fail: Object.fromEntries(routeNames.map((rpc) => [rpc, 'PERMISSION_DENIED'])) }),
    );

    for (const rpc of routeNames) {
      const error = (await callAny(rpc, {}).catch((caught: unknown) => caught)) as ApiError;
      expect(error, rpc).toBeInstanceOf(ApiError);
      expect(error.code, rpc).toBe('PERMISSION_DENIED');
      expect(error.httpStatus, rpc).toBe(403);
    }
  });

  it('can inject each gRPC code, not only PERMISSION_DENIED', async () => {
    for (const code of GRPC_CODES) {
      if (code === 'OK') continue;
      server.use(...fakeApi({ fail: { 'sample/list': code } }));

      const error = (await call('sample/list', { labId: 'lab-demo' }).catch(
        (caught: unknown) => caught,
      )) as ApiError;

      expect(error.code, code).toBe(code);
      expect(error.httpStatus, code).toBe(HTTP_STATUS_FOR[code]);
    }
  });

  it('rejects a fail key that is not a route, instead of silently doing nothing', () => {
    expect(() => fakeApi({ fail: { 'sample/lst': 'NOT_FOUND' } as never })).toThrow(
      /is not a route/,
    );
  });

  it('rejects a fail value that is not a gRPC code name', () => {
    expect(() => fakeApi({ fail: { 'sample/list': 'NOPE' } as never })).toThrow(
      /not a gRPC status name/,
    );
  });
});

describe('fakeApi demo lab', () => {
  it('serves the seeded samples of the requested lab only', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));

    const response = await call('sample/list', { labId: 'lab-demo' });

    expect(response.samples.map((sample) => sample.id)).toEqual([
      'sample-1',
      'sample-2',
      'sample-3',
    ]);
    expect(response.page?.totalCount).toBe(3);
  });

  it('pages with an opaque page_token, like the gateway', async () => {
    server.use(...fakeApi());

    const first = await call('sample/list', { labId: 'lab-demo', page: { pageSize: 2 } });
    const token = first.page?.nextPageToken ?? '';
    const second = await call('sample/list', {
      labId: 'lab-demo',
      page: { pageSize: 2, pageToken: token },
    });

    expect(first.samples).toHaveLength(2);
    expect(token).not.toBe('');
    expect(second.samples.map((sample) => sample.id)).toEqual(['sample-3']);
    expect(second.page?.nextPageToken).toBe('');
  });

  it('applies the box_id filter', async () => {
    server.use(...fakeApi());

    const response = await call('sample/list', { labId: 'lab-demo', boxId: 'box-2' });

    expect(response.samples.map((sample) => sample.id)).toEqual(['sample-3']);
  });

  it('hides tombstoned samples unless include_archived is set', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));

    await call('sample/delete', { sampleId: 'sample-1' });

    const visible = await call('sample/list', { labId: 'lab-demo' });
    const all = await call('sample/list', { labId: 'lab-demo', includeArchived: true });

    expect(visible.samples.map((sample) => sample.id)).not.toContain('sample-1');
    expect(all.samples.map((sample) => sample.id)).toContain('sample-1');
  });

  it('writes into the store the caller passed in', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));

    const created = await call('sample/create', {
      labId: 'lab-demo',
      itemTypeId: 'it-serum',
      name: 'Serum C',
    });

    expect(created.sample?.name).toBe('Serum C');
    expect(lab.samples.map((sample) => sample.name)).toContain('Serum C');
  });
});

describe('fakeApi error branches', () => {
  it('answers NOT_FOUND for a sample that does not exist', async () => {
    server.use(...fakeApi());

    const error = (await call('sample/get', { sampleId: 'nope' }).catch(
      (caught: unknown) => caught,
    )) as ApiError;

    expect(error.code).toBe('NOT_FOUND');
    expect(error.httpStatus).toBe(404);
  });

  it('answers FAILED_PRECONDITION for a checkout the domain forbids', async () => {
    server.use(...fakeApi());

    const error = (await call('sample/checkout', { sampleId: 'sample-3' }).catch(
      (caught: unknown) => caught,
    )) as ApiError;

    expect(error.code).toBe('FAILED_PRECONDITION');
    expect(error.httpStatus).toBe(412);
  });

  it('rejects an unknown field, as the gateway does (ignore_unknown_fields = false)', async () => {
    server.use(...fakeApi());

    const response = await fetch(apiRoutes['sample/list'].path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ lab_id: 'lab-demo', nonsense: 1 }),
    });

    expect(response.status).toBe(400);
    await expect(response.json()).resolves.toMatchObject({ code: 'INVALID_ARGUMENT' });
  });

  it('accepts both the proto and the lowerCamelCase field name, as protobuf JSON does', async () => {
    server.use(...fakeApi());

    // The gateway sets `preserve_proto_field_names` on *output*; on input,
    // protobuf's JSON parser accepts either spelling. The fake must not be
    // stricter than the real thing, or a test would pass here and fail in
    // production.
    const response = await fetch(apiRoutes['sample/list'].path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ labId: 'lab-demo' }),
    });

    expect(response.status).toBe(200);
  });
});

describe('fakeApi latency', () => {
  it('delays the response so loading states can be asserted', async () => {
    server.use(...fakeApi({ latencyMs: 30 }));

    const started = Date.now();
    await call('sample/list', { labId: 'lab-demo' });

    expect(Date.now() - started).toBeGreaterThanOrEqual(25);
  });
});

describe('fakeApi SSE', () => {
  it('drives subscribeSse through the shared EventSource fake', () => {
    FakeEventSource.reset();
    const frames: unknown[] = [];

    const unsubscribe = subscribeSse('sample/watch', {
      schema: SampleSchema,
      params: { lab_id: 'lab-demo' },
      onFrame: (frame) => frames.push(frame.data),
      eventSourceFactory: fakeEventSource,
    });

    FakeEventSource.current().open();
    FakeEventSource.current().message(
      JSON.stringify({
        id: 'sample-1',
        lab_id: 'lab-demo',
        name: 'Serum A',
        status: 'SAMPLE_STATUS_ACTIVE',
      }),
      '1758931200000000',
    );
    unsubscribe();

    expect(frames).toHaveLength(1);
    expect(frames[0]).toMatchObject({ id: 'sample-1', name: 'Serum A' });
    expect(FakeEventSource.current().closed).toBe(true);
  });

  it('exposes the same status mapping the gateway uses', () => {
    expect(HTTP_STATUS_FOR.UNAUTHENTICATED).toBe(401);
    expect(HTTP_STATUS_FOR.ALREADY_EXISTS).toBe(409);
    expect(HTTP_STATUS_FOR.ABORTED).toBe(409);
    expect(HTTP_STATUS_FOR.OUT_OF_RANGE).toBe(400);
    expect(HTTP_STATUS_FOR.UNIMPLEMENTED).toBe(501);
    expect(HTTP_STATUS_FOR.DEADLINE_EXCEEDED).toBe(504);
    expect(SampleStatus.ACTIVE).toBe(1);
  });
});
