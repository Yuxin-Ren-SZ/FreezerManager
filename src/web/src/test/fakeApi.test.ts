// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { call } from '../api/client';
import { ApiError, GRPC_CODES } from '../api/errors';
import { apiRoutes, type RpcName } from '../api/routes';
import { subscribeSse } from '../api/sse';
import { SampleSchema, SampleStatus, CheckoutAction } from '../gen/fmgr/v1/sample_pb';
import { FakeEventSource, fakeEventSource } from './fakeEventSource';
import {
  createDemoLab,
  DEMO_MFA_EMAIL,
  DEMO_PASSWORD,
  DEMO_TOTP_CODE,
  DEMO_USER_EMAIL,
  fakeApi,
  HTTP_STATUS_FOR,
  seedSamples,
} from './fakeApi';
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

/**
 * The routes whose *implemented* answer to an empty body is a refusal rather
 * than a payload: `Login` refuses an unknown email and `SubmitMfa` refuses a
 * code with no half-finished login behind it, both `UNAUTHENTICATED` — exactly
 * as `freezerd` does. Listed per route rather than widening the blanket list,
 * which would let any other route start answering 401 unnoticed.
 */
const EMPTY_BODY_STATUS: Partial<Record<RpcName, number>> = {
  'auth/login': 401,
  'auth/browser/login': 401,
  'auth/submit-mfa': 401,
  'auth/browser/submit-mfa': 401,
};

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
      const pinned = EMPTY_BODY_STATUS[rpc];
      if (pinned !== undefined) {
        expect(response.status, `${rpc} (${apiRoutes[rpc].path})`).toBe(pinned);
        continue;
      }
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
    // `SampleServiceImpl` never sets `total_count` (nor does any other service),
    // so the fake must not invent one: a screen that rendered it would show a
    // number the real gateway never sends. The paging-contract block below is
    // the full statement of that rule.
    expect(response.page?.totalCount).toBe(0);
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

describe('fakeApi paging and filtering contract (G3.2)', () => {
  it('returns every row for page_size 0, as ListSamples does (0 means "no limit")', async () => {
    const lab = seedSamples(createDemoLab(), 150);
    server.use(...fakeApi({ lab }));

    const response = await call('sample/list', { labId: 'lab-demo' });

    // 150, not the fake's old DEFAULT_PAGE_SIZE of 100: the gateway only limits
    // when `page_size > 0`, so a fake cap silently truncated this screen's data.
    expect(response.samples).toHaveLength(150);
    expect(response.page?.nextPageToken).toBe('');
  });

  it('pages past 100 rows and ends only on a short page, like SampleServiceImpl', async () => {
    const lab = seedSamples(createDemoLab(), 150);
    server.use(...fakeApi({ lab }));

    const first = await call('sample/list', { labId: 'lab-demo', page: { pageSize: 100 } });
    const second = await call('sample/list', {
      labId: 'lab-demo',
      page: { pageSize: 100, pageToken: first.page?.nextPageToken ?? '' },
    });

    expect(first.samples).toHaveLength(100);
    expect(first.page?.nextPageToken).toBe('100');
    expect(second.samples).toHaveLength(50);
    expect(second.page?.nextPageToken).toBe('');
  });

  it('hands back a token after a full page even when nothing follows, like the server', async () => {
    const lab = seedSamples(createDemoLab(), 100);
    server.use(...fakeApi({ lab }));

    const first = await call('sample/list', { labId: 'lab-demo', page: { pageSize: 100 } });
    const second = await call('sample/list', {
      labId: 'lab-demo',
      page: { pageSize: 100, pageToken: first.page?.nextPageToken ?? '' },
    });

    // "A full page implies there may be more" is the server's rule, and being
    // faithful here is what makes `hasNextPage` behave the same in both.
    expect(first.page?.nextPageToken).toBe('100');
    expect(second.samples).toHaveLength(0);
    expect(second.page?.nextPageToken).toBe('');
  });

  it('applies the status filter', async () => {
    server.use(...fakeApi());

    const response = await call('sample/list', {
      labId: 'lab-demo',
      status: SampleStatus.CHECKED_OUT,
    });

    expect(response.samples.map((sample) => sample.id)).toEqual(['sample-3']);
  });

  it('searches name and barcode case-insensitively, as contains_ci_any does', async () => {
    server.use(...fakeApi());

    const byName = await call('sample/list', { labId: 'lab-demo', query: 'plasma' });
    const byBarcode = await call('sample/list', { labId: 'lab-demo', query: 'demo-0002' });

    expect(byName.samples.map((sample) => sample.id)).toEqual(['sample-3']);
    expect(byBarcode.samples.map((sample) => sample.id)).toEqual(['sample-2']);
  });

  it('rejects a query shorter than two bytes with INVALID_ARGUMENT', async () => {
    server.use(...fakeApi());

    const error = (await call('sample/list', { labId: 'lab-demo', query: 'a' }).catch(
      (caught: unknown) => caught,
    )) as ApiError;

    // The server refuses short queries (PRD §9) instead of scanning the lab; a
    // fake that answered them would let a screen ship a request the real
    // gateway 400s.
    expect(error.code).toBe('INVALID_ARGUMENT');
    expect(error.httpStatus).toBe(400);
  });

  it('serves the lab custom-field definitions the column chooser is built from', async () => {
    server.use(...fakeApi());

    const response = await call('custom-field-def/list', { labId: 'lab-demo' });

    expect(response.cfds.map((cfd) => cfd.key)).toContain('concentration');
    expect(response.cfds.every((cfd) => cfd.labId === 'lab-demo')).toBe(true);
  });

  it('filters custom-field definitions by item type', async () => {
    server.use(...fakeApi());

    const response = await call('custom-field-def/list', {
      labId: 'lab-demo',
      itemTypeId: 'it-plasma',
    });

    expect(response.cfds.map((cfd) => cfd.key)).toEqual(['freeze_thaw_count']);
  });

  it('serves a CSV body from sample/export, with the CLI column schema', async () => {
    server.use(...fakeApi());

    const response = await call('sample/export', { labId: 'lab-demo' });
    const lines = response.csvContent.trim().split('\n');

    expect(lines[0]).toBe(
      'id,lab_id,item_type_id,name,barcode,container_type_id,box_id,position_label,' +
        'volume_value,volume_unit,mass_value,mass_unit,status,parent_sample_id,created_by,' +
        'created_at,last_modified_by,last_modified_at,custom_fields_json',
    );
    expect(lines).toHaveLength(4); // header + the three non-archived demo samples
    expect(response.csvContent).toContain('Serum A');
  });

  it('keeps a tombstoned sample out of the export unless include_archived is set', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));

    await call('sample/delete', { sampleId: 'sample-1' });

    const visible = await call('sample/export', { labId: 'lab-demo' });
    const all = await call('sample/export', { labId: 'lab-demo', includeArchived: true });

    expect(visible.csvContent).not.toContain('Serum A');
    expect(all.csvContent).toContain('Serum A');
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

describe('fakeApi checkout volume contract (#100)', () => {
  /**
   * The demo lab's `sample-3` is already checked out; give it a tracked volume
   * so a check-in has something to subtract from.
   */
  function checkedOutLab() {
    const lab = createDemoLab();
    const sample = sampleById(lab, 'sample-3');
    sample.volumeValue = 100;
    sample.volumeUnit = 'µL';
    return lab;
  }

  /** The seeded sample, by id. A missing fixture id is a broken test. */
  function sampleById(lab: ReturnType<typeof createDemoLab>, id: string) {
    const found = lab.samples.find((candidate) => candidate.id === id);
    if (found === undefined) throw new Error(`fixture has no sample ${id}`);
    return found;
  }

  it('refuses volume_used without volume_unit, as CheckoutSample now does', async () => {
    const lab = checkedOutLab();
    server.use(...fakeApi({ lab }));

    // This is the request the shipped check-in form sent: the operator typed a
    // volume, the server dropped it, and the screen said "checked in".
    const error = (await call('sample/checkout', {
      sampleId: 'sample-3',
      action: CheckoutAction.CHECKIN,
      volumeUsed: 40,
    }).catch((caught: unknown) => caught)) as ApiError;

    expect(error.code).toBe('INVALID_ARGUMENT');
    expect(error.httpStatus).toBe(400);
    // Refused, not half-applied: still checked out, volume untouched, no event.
    expect(sampleById(lab, 'sample-3').status).toBe(SampleStatus.CHECKED_OUT);
    expect(sampleById(lab, 'sample-3').volumeValue).toBe(100);
    expect(lab.checkoutEvents).toEqual([]);
  });

  it('refuses volume_unit without volume_used', async () => {
    const lab = checkedOutLab();
    server.use(...fakeApi({ lab }));

    const error = (await call('sample/checkout', {
      sampleId: 'sample-3',
      action: CheckoutAction.CHECKIN,
      volumeUnit: 'µL',
    }).catch((caught: unknown) => caught)) as ApiError;

    expect(error.code).toBe('INVALID_ARGUMENT');
    expect(sampleById(lab, 'sample-3').status).toBe(SampleStatus.CHECKED_OUT);
    expect(lab.checkoutEvents).toEqual([]);
  });

  it('refuses a unit core::parse_volume_unit does not know', async () => {
    const lab = checkedOutLab();
    server.use(...fakeApi({ lab }));

    const error = (await call('sample/checkout', {
      sampleId: 'sample-3',
      action: CheckoutAction.CHECKIN,
      volumeUsed: 40,
      volumeUnit: 'furlong',
    }).catch((caught: unknown) => caught)) as ApiError;

    expect(error.code).toBe('INVALID_ARGUMENT');
    expect(sampleById(lab, 'sample-3').volumeValue).toBe(100);
    expect(lab.checkoutEvents).toEqual([]);
  });

  it('refuses a negative volume_used, which would otherwise add stock (#112)', async () => {
    const lab = checkedOutLab();
    server.use(...fakeApi({ lab }));

    const error = (await call('sample/checkout', {
      sampleId: 'sample-3',
      action: CheckoutAction.CHECKIN,
      volumeUsed: -40,
      volumeUnit: 'µL',
    }).catch((caught: unknown) => caught)) as ApiError;

    expect(error.code).toBe('INVALID_ARGUMENT');
    // Refused, not applied in reverse: the subtraction that would have left the
    // vial holding 140 µL never ran, so nothing is stopped and nothing is logged.
    expect(sampleById(lab, 'sample-3').status).toBe(SampleStatus.CHECKED_OUT);
    expect(sampleById(lab, 'sample-3').volumeValue).toBe(100);
    expect(lab.checkoutEvents).toEqual([]);
  });

  it('accepts an explicit zero volume, which is not a negative one', async () => {
    const lab = checkedOutLab();
    server.use(...fakeApi({ lab }));

    await call('sample/checkout', {
      sampleId: 'sample-3',
      action: CheckoutAction.CHECKIN,
      volumeUsed: 0,
      volumeUnit: 'µL',
    });

    // The boundary: zero is a recorded no-op (`volumeDelta: 0`), distinct from
    // the absent pair above (`volumeDelta` unset). The sign rule refuses `< 0`.
    expect(sampleById(lab, 'sample-3').volumeValue).toBe(100);
    expect(lab.checkoutEvents).toMatchObject([
      { sampleId: 'sample-3', action: CheckoutAction.CHECKIN, volumeDelta: 0, volumeUnit: 'µL' },
    ]);
  });

  it('checks in without a volume when neither field is sent', async () => {
    const lab = checkedOutLab();
    server.use(...fakeApi({ lab }));

    await call('sample/checkout', {
      sampleId: 'sample-3',
      action: CheckoutAction.CHECKIN,
      reason: 'no volume taken',
    });

    expect(sampleById(lab, 'sample-3').status).toBe(SampleStatus.ACTIVE);
    expect(sampleById(lab, 'sample-3').volumeValue).toBe(100);
    // The event exists, it just carries no delta (`volume_delta` is nullopt).
    expect(lab.checkoutEvents).toMatchObject([
      { sampleId: 'sample-3', action: CheckoutAction.CHECKIN, reason: 'no volume taken' },
    ]);
    expect(lab.checkoutEvents[0]).not.toHaveProperty('volumeDelta');
  });

  it('converts the request unit into the sample unit and records the signed delta', async () => {
    const lab = createDemoLab();
    const sample = sampleById(lab, 'sample-3');
    sample.volumeValue = 5000;
    sample.volumeUnit = 'µL';
    server.use(...fakeApi({ lab }));

    // 2 mL of a sample tracked in µL is 2000 µL: `core::Volume::to_unit`.
    await call('sample/checkout', {
      sampleId: 'sample-3',
      action: CheckoutAction.CHECKIN,
      volumeUsed: 2,
      volumeUnit: 'mL',
    });

    expect(sampleById(lab, 'sample-3').volumeValue).toBe(3000);
    expect(lab.checkoutEvents).toMatchObject([
      {
        sampleId: 'sample-3',
        action: CheckoutAction.CHECKIN,
        volumeDelta: -2000,
        volumeUnit: 'µL',
      },
    ]);
  });

  it('truncates the amount before converting, as Volume::from_raw does', async () => {
    const lab = checkedOutLab();
    server.use(...fakeApi({ lab }));

    // `Volume::from_raw` casts `volume_used` to an integer *in the request unit*
    // before `to_unit` runs, so 0.04 mL is raw 0 mL — 0 µL, not 40 µL. The fake
    // must not be more generous than the server: a screen that ships 0.04 mL
    // consumes nothing against real `freezerd`, and a test here has to say so.
    await call('sample/checkout', {
      sampleId: 'sample-3',
      action: CheckoutAction.CHECKIN,
      volumeUsed: 0.04,
      volumeUnit: 'mL',
    });

    expect(sampleById(lab, 'sample-3').volumeValue).toBe(100);
    expect(lab.checkoutEvents).toMatchObject([
      { sampleId: 'sample-3', action: CheckoutAction.CHECKIN, volumeDelta: 0, volumeUnit: 'µL' },
    ]);
  });

  it('records the discard delta as the whole remaining volume', async () => {
    const lab = createDemoLab();
    sampleById(lab, 'sample-1').volumeValue = 100;
    sampleById(lab, 'sample-1').volumeUnit = 'µL';
    server.use(...fakeApi({ lab }));

    await call('sample/checkout', { sampleId: 'sample-1', action: CheckoutAction.DISCARD });

    expect(sampleById(lab, 'sample-1').volumeValue).toBe(0);
    expect(lab.checkoutEvents).toMatchObject([
      { sampleId: 'sample-1', action: CheckoutAction.DISCARD, volumeDelta: -100, volumeUnit: 'µL' },
    ]);
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

describe('fakeApi browser session (G2.1)', () => {
  it('accepts the seeded password and answers without a token for the browser route', async () => {
    server.use(...fakeApi());

    const response = await call('auth/browser/login', {
      email: DEMO_USER_EMAIL,
      password: DEMO_PASSWORD,
    });

    expect(response.userId).toBe('user-demo');
    expect(response.mfaRequired).toBe(false);
    // The gateway moves the token into an HttpOnly cookie and omits it from the
    // body (`RestGateway.cc`'s LoginResponse overload). A fake that echoed it
    // would let the SPA read a credential G0.1 deliberately withholds.
    expect(response.sessionToken).toBe('');
  });

  it('refuses a wrong password and an unknown email the same way', async () => {
    server.use(...fakeApi());

    for (const credentials of [
      { email: DEMO_USER_EMAIL, password: 'not-the-password' },
      { email: 'nobody@example.test', password: DEMO_PASSWORD },
    ]) {
      const error = (await call('auth/browser/login', credentials).catch(
        (caught: unknown) => caught,
      )) as ApiError;

      expect(error.code).toBe('UNAUTHENTICATED');
      expect(error.httpStatus).toBe(401);
    }
  });

  it('answers mfa_required for an account with a second factor', async () => {
    server.use(...fakeApi());

    const response = await call('auth/browser/login', {
      email: DEMO_MFA_EMAIL,
      password: DEMO_PASSWORD,
    });

    expect(response.mfaRequired).toBe(true);
  });

  it('completes a pending login with the seeded code and refuses a wrong one', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));

    await call('auth/browser/login', { email: DEMO_MFA_EMAIL, password: DEMO_PASSWORD });
    expect(lab.auth.pendingMfaUserId).toBe('user-mfa');

    const wrong = (await call('auth/browser/submit-mfa', { totpCode: '000000' }).catch(
      (caught: unknown) => caught,
    )) as ApiError;
    expect(wrong.code).toBe('UNAUTHENTICATED');
    // Still outstanding: a refused code does not consume the login.
    expect(lab.auth.pendingMfaUserId).toBe('user-mfa');

    await call('auth/browser/submit-mfa', { totpCode: DEMO_TOTP_CODE });
    expect(lab.auth.pendingMfaUserId).toBeNull();
  });

  it('refuses a code with no pending login behind it', async () => {
    server.use(...fakeApi());

    const error = (await call('auth/browser/submit-mfa', { totpCode: DEMO_TOTP_CODE }).catch(
      (caught: unknown) => caught,
    )) as ApiError;

    expect(error.code).toBe('UNAUTHENTICATED');
  });

  it('refuses every other route while the second factor is outstanding, and lets logout through', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));

    await call('auth/browser/login', { email: DEMO_MFA_EMAIL, password: DEMO_PASSWORD });

    // `AuthMiddleware`'s TokenAndMfa rule: a half-finished login cannot read lab
    // data, and the refusal carries the `mfa_required: ` prefix the SPA keys on.
    const refused = (await call('sample/list', { labId: 'lab-demo' }).catch(
      (caught: unknown) => caught,
    )) as ApiError;
    expect(refused.code).toBe('UNAUTHENTICATED');
    expect(refused.message).toContain('mfa_required:');

    // ...but it can give the credential up (#62), which is what makes an
    // abandoned login recoverable rather than a dead end.
    await call('auth/browser/logout', {});
    expect(lab.auth.pendingMfaUserId).toBeNull();
    await expect(call('sample/list', { labId: 'lab-demo' })).resolves.toBeDefined();
  });

  it('signs out a completed session without touching the other routes', async () => {
    const lab = createDemoLab();
    server.use(...fakeApi({ lab }));

    await call('auth/browser/login', { email: DEMO_USER_EMAIL, password: DEMO_PASSWORD });
    await call('auth/browser/logout', {});

    expect(lab.auth.pendingMfaUserId).toBeNull();
  });

  it('rejects a login body that is not the request message, as the gateway does', async () => {
    server.use(...fakeApi());

    const response = await fetch('/api/v1/auth/browser/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ emailAddress: DEMO_USER_EMAIL, password: DEMO_PASSWORD }),
    });

    expect(response.status).toBe(400);
  });
});
