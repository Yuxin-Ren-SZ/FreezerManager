// SPDX-License-Identifier: AGPL-3.0-or-later
import {
  create,
  fromJson,
  toJson,
  type DescMessage,
  type JsonValue,
  type Message,
  type MessageInitShape,
} from '@bufbuild/protobuf';
import { HttpResponse, http, type HttpHandler } from 'msw';
import { isGrpcCode, type GrpcCode } from '../api/errors';
import { apiRoutes, type RpcName } from '../api/routes';
import { TimestampSchema } from '../gen/fmgr/v1/common/types_pb';
import { ItemTypeSchema, type ItemType } from '../gen/fmgr/v1/item_type_pb';
import { LabSchema, type Lab } from '../gen/fmgr/v1/lab_pb';
import { SampleSchema, SampleStatus, type Sample } from '../gen/fmgr/v1/sample_pb';

/**
 * The MSW fake for the whole REST surface (TODO.md G1.2, G-arch 10).
 *
 * Two rules shape it:
 *
 * 1. **Every route in `routes.ts` gets a handler**, so a screen test can never
 *    silently depend on a request that no fake answers (the MSW server in
 *    `src/test/setup.ts` runs with `onUnhandledRequest: 'error'`).
 * 2. **Every route can fail, per RPC**:
 *    `fakeApi({ fail: { 'sample/list': 'PERMISSION_DENIED' } })`. A fake that
 *    can only answer `OK` hides every error branch — the failure mode
 *    `doc/TEST_COVERAGE_AUDIT_2026-07-01.md` records for the C++ fakes, and the
 *    reason AGENTS.md §6 requires `fail_<method>` injection there.
 *
 * Requests are parsed exactly as the gateway parses them (`fromJson` with
 * `ignore_unknown_fields = false`, `JsonProtoMapping.cc`), so a client that
 * sends camelCase field names fails here the same way it would against
 * `freezerd`.
 *
 * The seeded data is synthetic demo data. No fixture may contain PHI
 * (AGENTS.md §5).
 */

export interface DemoLab {
  labs: Lab[];
  itemTypes: ItemType[];
  samples: Sample[];
}

/** A seeded, in-memory demo lab. Pass your own to `fakeApi({ lab })` to inspect it. */
/** A real `Timestamp` message, not a bare object: nested messages must be messages. */
const seedTimestamp = () => create(TimestampSchema, { unixMicros: 1_758_931_200_000_000n });

export function createDemoLab(): DemoLab {
  const createdAt = seedTimestamp();

  return {
    labs: [
      create(LabSchema, {
        id: 'lab-demo',
        name: 'Demo Lab',
        contact: 'demo@example.test',
        createdAt,
        settingsJson: '{}',
      }),
      create(LabSchema, {
        id: 'lab-second',
        name: 'Second Demo Lab',
        contact: 'second@example.test',
        createdAt,
        settingsJson: '{}',
      }),
    ],
    itemTypes: [
      create(ItemTypeSchema, { id: 'it-serum', labId: 'lab-demo', name: 'Serum', createdAt }),
      create(ItemTypeSchema, { id: 'it-plasma', labId: 'lab-demo', name: 'Plasma', createdAt }),
      create(ItemTypeSchema, { id: 'it-dna', labId: 'lab-second', name: 'DNA', createdAt }),
    ],
    samples: [
      seedSample({
        id: 'sample-1',
        labId: 'lab-demo',
        itemTypeId: 'it-serum',
        name: 'Serum A',
        barcode: 'DEMO-0001',
        boxId: 'box-1',
        positionLabel: 'A1',
      }),
      seedSample({
        id: 'sample-2',
        labId: 'lab-demo',
        itemTypeId: 'it-serum',
        name: 'Serum B',
        barcode: 'DEMO-0002',
        boxId: 'box-1',
        positionLabel: 'A2',
      }),
      seedSample({
        id: 'sample-3',
        labId: 'lab-demo',
        itemTypeId: 'it-plasma',
        name: 'Plasma A',
        barcode: 'DEMO-0003',
        boxId: 'box-2',
        positionLabel: 'B1',
        status: SampleStatus.CHECKED_OUT,
      }),
      seedSample({
        id: 'sample-4',
        labId: 'lab-second',
        itemTypeId: 'it-dna',
        name: 'DNA A',
        barcode: 'DEMO-0004',
      }),
    ],
  };
}

/** The fields a caller must supply to seed a sample; the rest come from defaults. */
type SampleSeed = MessageInitShape<typeof SampleSchema>;

function seedSample(seed: SampleSeed): Sample {
  return create(SampleSchema, {
    status: SampleStatus.ACTIVE,
    createdAt: seedTimestamp(),
    lastModifiedAt: seedTimestamp(),
    customFieldsJson: '{}',
    ...seed,
  });
}

/** Mirrors `http_status_for()` in `src/rest/RestErrorTranslation.h`. */
export const HTTP_STATUS_FOR: Readonly<Record<GrpcCode, number>> = {
  OK: 200,
  CANCELLED: 500,
  UNKNOWN: 500,
  INVALID_ARGUMENT: 400,
  DEADLINE_EXCEEDED: 504,
  NOT_FOUND: 404,
  ALREADY_EXISTS: 409,
  PERMISSION_DENIED: 403,
  RESOURCE_EXHAUSTED: 429,
  FAILED_PRECONDITION: 412,
  ABORTED: 409,
  OUT_OF_RANGE: 400,
  UNIMPLEMENTED: 501,
  INTERNAL: 500,
  UNAVAILABLE: 503,
  UNAUTHENTICATED: 401,
};

/** Throw from a resolver to answer with a gRPC error instead of a body. */
export class FakeRpcError extends Error {
  readonly code: GrpcCode;

  constructor(code: GrpcCode, message = '') {
    super(message);
    this.name = 'FakeRpcError';
    this.code = code;
  }
}

/**
 * A decoded request, read through the resolver's own field list. The fake does
 * not restate every request message: `fromJson` already validated the body
 * against the generated schema, so the cast only names the fields in use.
 */
const fields = (message: Message): Record<string, unknown> => message;

type Resolver = (lab: DemoLab, message: Message) => MessageInitShape<DescMessage> | undefined;

const DEFAULT_PAGE_SIZE = 100;

function paginate<T>(items: T[], page: JsonValue | undefined): { slice: T[]; token: string } {
  // Decoded messages carry the TypeScript field names, not the wire names.
  const { pageSize: size = 0, pageToken: token = '' } = (page ?? {}) as {
    pageSize?: number;
    pageToken?: string;
  };
  const limit = size > 0 ? size : DEFAULT_PAGE_SIZE;
  const offset = Number.parseInt(token, 10) || 0;
  return {
    slice: items.slice(offset, offset + limit),
    token: offset + limit < items.length ? String(offset + limit) : '',
  };
}

const page = (nextPageToken: string, totalCount: number) => ({ nextPageToken, totalCount });

function requireId(id: string, kind: string): void {
  if (id === '') {
    throw new FakeRpcError('INVALID_ARGUMENT', `${kind} id is required`);
  }
}

/**
 * The routes that answer with real demo data. Every other route in `routes.ts`
 * still gets a handler and can still be made to fail, but replies with the
 * response message's default values. A feature task that needs real data for
 * another route adds a resolver here — cheaper than every test stubbing its
 * own, and it keeps the fake one place.
 */
const resolvers: Partial<Record<RpcName, Resolver>> = {
  'lab/list': (lab, message) => {
    const { page: pageRequest } = fields(message) as { page?: JsonValue };
    const { slice, token } = paginate(lab.labs, pageRequest);
    return { labs: slice, page: page(token, lab.labs.length) };
  },

  'lab/get': (lab, message) => {
    const { labId } = fields(message) as { labId: string };
    requireId(labId, 'lab');
    const found = lab.labs.find((candidate) => candidate.id === labId);
    if (found === undefined) throw new FakeRpcError('NOT_FOUND', 'no such lab');
    return { lab: found };
  },

  'item-type/list': (lab, message) => {
    const {
      labId,
      includeArchived,
      page: pageRequest,
    } = fields(message) as {
      labId: string;
      includeArchived: boolean;
      page?: JsonValue;
    };
    const matching = lab.itemTypes.filter(
      (candidate) =>
        candidate.labId === labId && (includeArchived || candidate.archivedAt === undefined),
    );
    const { slice, token } = paginate(matching, pageRequest);
    return { itemTypes: slice, page: page(token, matching.length) };
  },

  'sample/list': (lab, message) => {
    const {
      labId,
      includeArchived,
      boxId,
      itemTypeId,
      barcode,
      page: pageRequest,
    } = fields(message) as {
      labId: string;
      includeArchived: boolean;
      boxId?: string;
      itemTypeId?: string;
      barcode?: string;
      page?: JsonValue;
    };
    const matching = lab.samples.filter((candidate) => {
      if (candidate.labId !== labId) return false;
      if (!includeArchived && candidate.status === SampleStatus.TOMBSTONED) return false;
      if (boxId !== undefined && candidate.boxId !== boxId) return false;
      if (itemTypeId !== undefined && candidate.itemTypeId !== itemTypeId) return false;
      if (barcode !== undefined && candidate.barcode !== barcode) return false;
      return true;
    });
    const { slice, token } = paginate(matching, pageRequest);
    return { samples: slice, page: page(token, matching.length) };
  },

  'sample/get': (lab, message) => {
    const { sampleId } = fields(message) as { sampleId: string };
    requireId(sampleId, 'sample');
    const found = lab.samples.find((candidate) => candidate.id === sampleId);
    if (found === undefined) throw new FakeRpcError('NOT_FOUND', 'no such sample');
    return { sample: found };
  },

  'sample/create': (lab, message) => {
    const init = fields(message) as MessageInitShape<typeof SampleSchema>;
    requireId(init.labId ?? '', 'lab');
    if ((init.name ?? '').trim() === '') {
      throw new FakeRpcError('INVALID_ARGUMENT', 'name is required');
    }
    const created = create(SampleSchema, {
      ...init,
      id: `sample-${String(lab.samples.length + 1)}-created`,
      status: SampleStatus.ACTIVE,
      createdAt: seedTimestamp(),
      lastModifiedAt: seedTimestamp(),
      customFieldsJson: init.customFieldsJson ?? '{}',
    });
    lab.samples.push(created);
    return { sample: created };
  },

  'sample/update': (lab, message) => {
    const { sample: incoming } = fields(message) as { sample?: Sample };
    if (incoming === undefined) throw new FakeRpcError('INVALID_ARGUMENT', 'sample is required');
    const index = lab.samples.findIndex((candidate) => candidate.id === incoming.id);
    if (index < 0) throw new FakeRpcError('NOT_FOUND', 'no such sample');
    lab.samples[index] = incoming;
    return { sample: incoming };
  },

  'sample/delete': (lab, message) => {
    const { sampleId } = fields(message) as { sampleId: string };
    const found = lab.samples.find((candidate) => candidate.id === sampleId);
    if (found === undefined) throw new FakeRpcError('NOT_FOUND', 'no such sample');
    found.status = SampleStatus.TOMBSTONED;
    return {};
  },

  'sample/move': (lab, message) => {
    const { sampleId, destBoxId, destPosition } = fields(message) as {
      sampleId: string;
      destBoxId?: string;
      destPosition?: string;
    };
    const found = lab.samples.find((candidate) => candidate.id === sampleId);
    if (found === undefined) throw new FakeRpcError('NOT_FOUND', 'no such sample');
    if (destBoxId !== undefined) found.boxId = destBoxId;
    if (destPosition !== undefined) found.positionLabel = destPosition;
    return { sample: found };
  },

  'sample/checkout': (lab, message) => {
    const { sampleId } = fields(message) as { sampleId: string };
    const found = lab.samples.find((candidate) => candidate.id === sampleId);
    if (found === undefined) throw new FakeRpcError('NOT_FOUND', 'no such sample');
    if (found.status !== SampleStatus.ACTIVE) {
      throw new FakeRpcError('FAILED_PRECONDITION', 'only an active sample can be checked out');
    }
    found.status = SampleStatus.CHECKED_OUT;
    return { sample: found };
  },
};

export interface FakeApiOptions {
  /**
   * Per-RPC error injection, keyed by the route key:
   * `{ 'sample/list': 'PERMISSION_DENIED' }`. An unknown key or an unknown
   * code throws, so a typo cannot silently disable the fault.
   */
  readonly fail?: Partial<Record<RpcName, GrpcCode>>;
  /** Delay every response by this many ms (loading states). */
  readonly latencyMs?: number;
  /** Seed with an existing store, so a test can inspect what the fake holds. */
  readonly lab?: DemoLab;
}

function errorResponse(code: GrpcCode, message: string) {
  return HttpResponse.json({ code, message }, { status: HTTP_STATUS_FOR[code] });
}

/**
 * Handlers for every route in `routes.ts`. Spread them into the MSW server:
 *
 * ```ts
 * server.use(...fakeApi({ fail: { 'sample/list': 'PERMISSION_DENIED' } }));
 * ```
 */
export function fakeApi(options: FakeApiOptions = {}): HttpHandler[] {
  for (const [rpc, code] of Object.entries(options.fail ?? {})) {
    if (!(rpc in apiRoutes)) {
      throw new Error(
        `fakeApi: "${rpc}" is not a route in src/api/routes.ts — check the key for a typo`,
      );
    }
    if (!isGrpcCode(code)) {
      throw new Error(`fakeApi: "${String(code)}" is not a gRPC status name`);
    }
  }

  const lab = options.lab ?? createDemoLab();
  const latencyMs = options.latencyMs ?? 0;

  return Object.entries(apiRoutes).map(([rpc, route]) =>
    http.post(route.path, async ({ request: httpRequest }) => {
      const injected = options.fail?.[rpc as RpcName];
      if (injected !== undefined) {
        return errorResponse(injected, `injected failure for ${rpc}`);
      }

      if (latencyMs > 0) {
        await new Promise((resolve) => setTimeout(resolve, latencyMs));
      }

      let parsed: Message;
      try {
        const body = (await httpRequest.json()) as JsonValue;
        // The same options as JsonProtoMapping.cc: unknown fields are an error,
        // so a client that sends camelCase fails here exactly as it would
        // against freezerd.
        parsed = fromJson(route.input, body, { ignoreUnknownFields: false });
      } catch {
        return errorResponse('INVALID_ARGUMENT', 'request body did not match the message');
      }

      try {
        const resolved = resolvers[rpc as RpcName]?.(lab, parsed);
        const response = create(route.output, resolved ?? {});
        return HttpResponse.json(toJson(route.output, response, { useProtoFieldName: true }));
      } catch (error) {
        if (error instanceof FakeRpcError) {
          return errorResponse(error.code, error.message);
        }
        throw error;
      }
    }),
  );
}
