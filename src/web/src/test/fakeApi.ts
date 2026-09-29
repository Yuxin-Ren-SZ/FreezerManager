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
import {
  BoxPositionSchema,
  BoxSchema,
  BoxTypeSchema,
  ContainerKind,
  FreezerSchema,
  StorageContainerSchema,
  type Box,
  type BoxPosition,
  type BoxType,
  type Freezer,
  type StorageContainer,
} from '../gen/fmgr/v1/box_pb';
import { TimestampSchema } from '../gen/fmgr/v1/common/types_pb';
import {
  CustomFieldDefinitionSchema,
  FieldDataType,
  ItemTypeSchema,
  ScopeKind,
  type CustomFieldDefinition,
  type ItemType,
} from '../gen/fmgr/v1/item_type_pb';
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
 * 3. **Paging and filtering mirror the server, not a convenience default**
 *    (TODO.md G3.2). `sample/list` follows `SampleServiceImpl::ListSamples`
 *    (`page_size = 0` means no limit, a token only after a full page, no
 *    `total_count`), the other list routes return everything because their
 *    services ignore `page`, and the `status` / `query` filters behave as the
 *    server's do — including the two-byte minimum on `query`. A fake that is
 *    more generous than `freezerd` is how a screen ships a request production
 *    rejects. See `samplePage`.
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
  /**
   * The lab's custom-field definitions (`custom-field-def/list`), which the
   * G3.2 column chooser turns into columns. Note the *route* needs
   * `custom_field.define`, so a read-only member's request fails and the screen
   * has to survive that — see `useCustomFieldDefinitions`.
   */
  customFieldDefs: CustomFieldDefinition[];
  samples: Sample[];
  /** Layout (BoxService): the physical tree the G3.1 screen renders. */
  freezers: Freezer[];
  storageContainers: StorageContainer[];
  boxTypes: BoxType[];
  boxes: Box[];
}

/** A seeded, in-memory demo lab. Pass your own to `fakeApi({ lab })` to inspect it. */
/** A real `Timestamp` message, not a bare object: nested messages must be messages. */
const seedTimestamp = () => create(TimestampSchema, { unixMicros: 1_758_931_200_000_000n });

/** `rows × cols` positions, labelled `A1`… like a real box map. */
function seedPositions(rows: number, cols: number): BoxPosition[] {
  return Array.from({ length: rows * cols }, (_, index) => {
    const row = Math.floor(index / cols) + 1;
    const col = (index % cols) + 1;
    return create(BoxPositionSchema, {
      label: `${String.fromCharCode(64 + row)}${String(col)}`,
      row,
      col,
    });
  });
}

/**
 * The seeded storage layout (`lab-demo`) from G3.1:
 *
 * ```
 * Freezer A ─ Rack 1 ─┬ Drawer 1 ─┬ Box A (96 positions)
 *                     │           └ Box B (96 positions)
 *                     ├ Drawer 2
 *                     └ Old tower            (archived)
 * Freezer B ─ Rack 2 ── Shelf 1 ─── Box C (9 positions)
 * Old freezer                                (archived)
 * ```
 *
 * The archived rows are seeded on purpose: the list RPCs have no
 * `include_archived` field, so the server sends them and the *client* is what
 * hides them — a fake that filtered them out would make that test vacuous.
 * `box-1` and `box-2` are the boxes `samples` already sits in.
 */
function seedLayout(createdAt: ReturnType<typeof seedTimestamp>) {
  const archivedAt = seedTimestamp();

  const layout = {
    freezers: [
      create(FreezerSchema, {
        id: 'fz-front',
        labId: 'lab-demo',
        name: 'Freezer A',
        location: 'Room 101',
        layoutRootId: 'ct-rack-1',
        createdAt,
      }),
      create(FreezerSchema, {
        id: 'fz-back',
        labId: 'lab-demo',
        name: 'Freezer B',
        location: 'Room 102',
        layoutRootId: 'ct-rack-2',
        createdAt,
      }),
      create(FreezerSchema, {
        id: 'fz-old',
        labId: 'lab-demo',
        name: 'Old freezer',
        layoutRootId: 'ct-rack-1',
        createdAt,
        archivedAt,
      }),
      create(FreezerSchema, {
        id: 'fz-second',
        labId: 'lab-second',
        name: 'Second freezer',
        layoutRootId: 'ct-second-root',
        createdAt,
      }),
    ],
    storageContainers: [
      create(StorageContainerSchema, {
        id: 'ct-rack-1',
        labId: 'lab-demo',
        kind: ContainerKind.RACK,
        name: 'Rack 1',
        createdAt,
      }),
      create(StorageContainerSchema, {
        id: 'ct-drawer-1',
        labId: 'lab-demo',
        parentId: 'ct-rack-1',
        kind: ContainerKind.DRAWER,
        name: 'Drawer 1',
        label: 'Top drawer',
        orderingIndex: 0,
        createdAt,
      }),
      create(StorageContainerSchema, {
        id: 'ct-drawer-2',
        labId: 'lab-demo',
        parentId: 'ct-rack-1',
        kind: ContainerKind.DRAWER,
        name: 'Drawer 2',
        orderingIndex: 1,
        createdAt,
      }),
      create(StorageContainerSchema, {
        id: 'ct-tower-old',
        labId: 'lab-demo',
        parentId: 'ct-rack-1',
        kind: ContainerKind.TOWER,
        name: 'Old tower',
        orderingIndex: 2,
        createdAt,
        archivedAt,
      }),
      create(StorageContainerSchema, {
        id: 'ct-rack-2',
        labId: 'lab-demo',
        kind: ContainerKind.RACK,
        name: 'Rack 2',
        createdAt,
      }),
      create(StorageContainerSchema, {
        id: 'ct-shelf-1',
        labId: 'lab-demo',
        parentId: 'ct-rack-2',
        kind: ContainerKind.SHELF,
        name: 'Shelf 1',
        createdAt,
      }),
      create(StorageContainerSchema, {
        id: 'ct-second-root',
        labId: 'lab-second',
        kind: ContainerKind.RACK,
        name: 'Second rack',
        createdAt,
      }),
    ],
    boxTypes: [
      create(BoxTypeSchema, {
        id: 'bt-96',
        labId: 'lab-demo',
        name: '96-well',
        positions: seedPositions(8, 12),
        createdAt,
      }),
      create(BoxTypeSchema, {
        id: 'bt-9',
        labId: 'lab-demo',
        name: '9-place',
        positions: seedPositions(3, 3),
        createdAt,
      }),
      create(BoxTypeSchema, {
        id: 'bt-old',
        labId: 'lab-demo',
        name: 'Legacy 4-place',
        positions: seedPositions(2, 2),
        createdAt,
        archivedAt,
      }),
    ],
    boxes: [
      create(BoxSchema, {
        id: 'box-1',
        labId: 'lab-demo',
        boxTypeId: 'bt-96',
        storageContainerId: 'ct-drawer-1',
        label: 'Box A',
        barcode: 'BOX-0001',
        createdAt,
      }),
      create(BoxSchema, {
        id: 'box-2',
        labId: 'lab-demo',
        boxTypeId: 'bt-96',
        storageContainerId: 'ct-drawer-1',
        label: 'Box B',
        createdAt,
      }),
      create(BoxSchema, {
        id: 'box-3',
        labId: 'lab-demo',
        boxTypeId: 'bt-9',
        storageContainerId: 'ct-shelf-1',
        label: 'Box C',
        createdAt,
      }),
      create(BoxSchema, {
        id: 'box-old',
        labId: 'lab-demo',
        boxTypeId: 'bt-old',
        storageContainerId: 'ct-drawer-1',
        label: 'Old box',
        createdAt,
        archivedAt,
      }),
      create(BoxSchema, {
        id: 'box-second',
        labId: 'lab-second',
        boxTypeId: 'bt-9',
        storageContainerId: 'ct-second-root',
        label: 'Second box',
        createdAt,
      }),
    ],
  };

  return layout;
}

export function createDemoLab(): DemoLab {
  const createdAt = seedTimestamp();

  return {
    ...seedLayout(createdAt),
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
    // Two item-type fields and one lab-wide field, which is the shape
    // `ListCustomFieldDefinitions` filters on: `item_type_id` is compared for
    // equality, so a lab-scoped definition is absent from an item-type query.
    customFieldDefs: [
      create(CustomFieldDefinitionSchema, {
        id: 'cfd-concentration',
        labId: 'lab-demo',
        scopeKind: ScopeKind.SAMPLE,
        itemTypeId: 'it-serum',
        key: 'concentration',
        label: 'Concentration',
        dataType: FieldDataType.FLOAT,
        createdAt,
      }),
      create(CustomFieldDefinitionSchema, {
        id: 'cfd-freeze-thaw',
        labId: 'lab-demo',
        scopeKind: ScopeKind.SAMPLE,
        itemTypeId: 'it-plasma',
        key: 'freeze_thaw_count',
        label: 'Freeze/thaw count',
        dataType: FieldDataType.INT,
        createdAt,
      }),
      create(CustomFieldDefinitionSchema, {
        id: 'cfd-storage-note',
        labId: 'lab-demo',
        scopeKind: ScopeKind.SAMPLE,
        key: 'storage_note',
        label: 'Storage note',
        dataType: FieldDataType.TEXT,
        createdAt,
      }),
      create(CustomFieldDefinitionSchema, {
        id: 'cfd-second-kit',
        labId: 'lab-second',
        scopeKind: ScopeKind.SAMPLE,
        itemTypeId: 'it-dna',
        key: 'extraction_kit',
        label: 'Extraction kit',
        dataType: FieldDataType.TEXT,
        createdAt,
      }),
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
        customFieldsJson: JSON.stringify({ concentration: '12.5' }),
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
        customFieldsJson: JSON.stringify({ freeze_thaw_count: '3' }),
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

/**
 * Replace a lab's samples with `count` generated ones (TODO.md G3.2).
 *
 * Deliberately a generator and not a literal list: the 100k-row screen has to be
 * tested against a fake that is *able* to serve 100k rows, otherwise the
 * "never loads them all" assertion proves nothing. The generated rows cover the
 * three filters a screen can apply server-side and the two it cannot:
 *
 * - `item_type_id`: even index → `it-serum`, odd → `it-plasma`;
 * - `box_id`: every 4th row `box-1`, the next `box-2`, the rest unplaced;
 * - `status`: every 5th row `CHECKED_OUT`, the rest `ACTIVE`;
 * - `name`/`barcode`: `Serum 000123` / `DEMO-000123`, so `query` has both a
 *   name and a barcode to match.
 *
 * The samples belong to `lab-demo`; the ids are stable (`sample-000001`), so a
 * test can assert on a page boundary without depending on insertion timing.
 */
export function seedSamples(lab: DemoLab, count: number): DemoLab {
  return {
    ...lab,
    samples: Array.from({ length: count }, (_, index) =>
      seedSample({
        id: `sample-${String(index + 1).padStart(6, '0')}`,
        labId: 'lab-demo',
        itemTypeId: index % 2 === 0 ? 'it-serum' : 'it-plasma',
        name:
          index % 2 === 0
            ? `Serum ${String(index + 1).padStart(6, '0')}`
            : `Plasma ${String(index + 1).padStart(6, '0')}`,
        barcode: `DEMO-${String(index + 1).padStart(6, '0')}`,
        ...(index % 4 === 0 ? { boxId: 'box-1' } : {}),
        ...(index % 4 === 1 ? { boxId: 'box-2' } : {}),
        status: index % 5 === 0 ? SampleStatus.CHECKED_OUT : SampleStatus.ACTIVE,
      }),
    ),
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

/**
 * Paging for `sample/list` — the one route this fake serves whose service
 * actually implements `page_token`.
 *
 * It is written to `SampleServiceImpl::ListSamples` and *not* to a convenient
 * default, because a fake that is more generous than the server lets a screen
 * pass a test it would fail in production (the opposite mistake, a cap the
 * server does not have, is what made a 150-row seed silently exercise 100 rows
 * before G3.2):
 *
 * - `page_size = 0` means **no limit**, so an un-paged request gets every row.
 * - A `next_page_token` comes back **only after a full page** ("a full page
 *   implies there may be more"), even when that page happened to be the last.
 * - The token is the next offset, and `page.token` is read with the same
 *   `parseInt` tolerance for a garbage token as the server's `stoull` on an
 *   empty string (`0`).
 * - There is no `total_count`: no `*ServiceImpl` in `src/server/` ever sets
 *   one, so the real gateway always sends the proto default.
 *
 * The other list routes are *not* paged here. `LabServiceImpl`,
 * `BoxServiceImpl` and `ItemTypeServiceImpl` ignore `page` entirely and answer
 * with the whole result, so `lab/list`, `freezer/list`, `storage-container/list`,
 * `box-type/list`, `box/list`, `item-type/list` and `custom-field-def/list` do
 * the same instead of inventing a cap the server does not have.
 */
function samplePage<T>(items: T[], page: JsonValue | undefined): { slice: T[]; token: string } {
  // Decoded messages carry the TypeScript field names, not the wire names.
  const { pageSize: size = 0, pageToken: token = '' } = (page ?? {}) as {
    pageSize?: number;
    pageToken?: string;
  };
  const offset = Number.parseInt(token, 10) || 0;
  const limited = size > 0;
  const slice = limited ? items.slice(offset, offset + size) : items.slice(offset);
  return {
    slice,
    token: limited && slice.length === size ? String(offset + slice.length) : '',
  };
}

function requireId(id: string, kind: string): void {
  if (id === '') {
    throw new FakeRpcError('INVALID_ARGUMENT', `${kind} id is required`);
  }
}

/** `SampleServiceImpl::k_min_query_length`: measured in bytes, not characters. */
const MIN_QUERY_BYTES = 2;

function utf8ByteLength(value: string): number {
  return new TextEncoder().encode(value).length;
}

/**
 * `contains_ci_any({name, barcode}, query)`: a case-insensitive substring match
 * over the name or the barcode. Custom fields and PHI are never searched, so
 * neither does this. SQLite's `LIKE` folds ASCII case; JavaScript's
 * `toLowerCase` folds more, which can only make a match *more* likely here —
 * the same direction a real deployment with a case-insensitive collation goes.
 */
function matchesQuery(sample: Sample, query: string): boolean {
  const needle = query.toLowerCase();
  return (
    sample.name.toLowerCase().includes(needle) ||
    (sample.barcode ?? '').toLowerCase().includes(needle)
  );
}

/** The columns `cli/SampleCsv.cc` writes, in order (`phi_fields_enc_json` aside). */
const SAMPLE_CSV_COLUMNS = [
  'id',
  'lab_id',
  'item_type_id',
  'name',
  'barcode',
  'container_type_id',
  'box_id',
  'position_label',
  'volume_value',
  'volume_unit',
  'mass_value',
  'mass_unit',
  'status',
  'parent_sample_id',
  'created_by',
  'created_at',
  'last_modified_by',
  'last_modified_at',
  'custom_fields_json',
] as const;

/** `core::to_string(SampleStatus)`, which is what the CSV's status column holds. */
const CSV_STATUS: Readonly<Record<number, string>> = {
  [SampleStatus.UNSPECIFIED]: '',
  [SampleStatus.ACTIVE]: 'active',
  [SampleStatus.CHECKED_OUT]: 'checked_out',
  [SampleStatus.DEPLETED]: 'depleted',
  [SampleStatus.DESTROYED]: 'destroyed',
  [SampleStatus.TOMBSTONED]: 'tombstoned',
};

/** RFC 4180 quoting: only what needs it, doubled quotes inside. */
function csvCell(value: string): string {
  return /[",\n\r]/.test(value) ? `"${value.replaceAll('"', '""')}"` : value;
}

/**
 * The CSV body of `ExportSamplesCsv` — the same chain-of-custody schema as
 * `freezerctl sample export`, with no PHI column. Enough of a body for a screen
 * test to download something real and assert on its shape.
 */
function exportSamplesCsv(samples: readonly Sample[]): string {
  const rows = samples.map((sample) =>
    [
      sample.id,
      sample.labId,
      sample.itemTypeId,
      sample.name,
      sample.barcode ?? '',
      sample.containerTypeId ?? '',
      sample.boxId ?? '',
      sample.positionLabel ?? '',
      sample.volumeValue === undefined ? '' : String(sample.volumeValue),
      sample.volumeUnit,
      sample.massValue === undefined ? '' : String(sample.massValue),
      sample.massUnit,
      CSV_STATUS[sample.status] ?? '',
      sample.parentSampleId ?? '',
      sample.createdBy,
      String(sample.createdAt?.unixMicros ?? 0n),
      sample.lastModifiedBy ?? '',
      String(sample.lastModifiedAt?.unixMicros ?? 0n),
      sample.customFieldsJson,
    ]
      .map(csvCell)
      .join(','),
  );
  return [...[SAMPLE_CSV_COLUMNS.join(',')], ...rows].join('\n').concat('\n');
}

/**
 * The routes that answer with real demo data. Every other route in `routes.ts`
 * still gets a handler and can still be made to fail, but replies with the
 * response message's default values. A feature task that needs real data for
 * another route adds a resolver here — cheaper than every test stubbing its
 * own, and it keeps the fake one place.
 */
const resolvers: Partial<Record<RpcName, Resolver>> = {
  // ---- The un-paged lists: the server ignores `page`, so these do too ----
  'lab/list': (lab) => ({ labs: lab.labs }),

  'lab/get': (lab, message) => {
    const { labId } = fields(message) as { labId: string };
    requireId(labId, 'lab');
    const found = lab.labs.find((candidate) => candidate.id === labId);
    if (found === undefined) throw new FakeRpcError('NOT_FOUND', 'no such lab');
    return { lab: found };
  },

  // ---- BoxService: the layout tree (G3.1) ----
  //
  // Each list RPC is lab-scoped and has no `include_archived` field, so the
  // archived rows come back and the client decides. `parent_id` /
  // `storage_container_id` are optional filters, exactly as in the proto.
  'freezer/list': (lab, message) => {
    const { labId } = fields(message) as { labId: string };
    requireId(labId, 'lab');
    return { freezers: lab.freezers.filter((freezer) => freezer.labId === labId) };
  },

  'storage-container/list': (lab, message) => {
    const { labId, parentId } = fields(message) as { labId: string; parentId?: string };
    requireId(labId, 'lab');
    return {
      containers: lab.storageContainers.filter((container) => {
        if (container.labId !== labId) return false;
        if (parentId !== undefined && container.parentId !== parentId) return false;
        return true;
      }),
    };
  },

  'box-type/list': (lab, message) => {
    const { labId } = fields(message) as { labId: string };
    requireId(labId, 'lab');
    return { boxTypes: lab.boxTypes.filter((boxType) => boxType.labId === labId) };
  },

  'box/list': (lab, message) => {
    const { labId, storageContainerId } = fields(message) as {
      labId: string;
      storageContainerId?: string;
    };
    requireId(labId, 'lab');
    return {
      boxes: lab.boxes.filter((box) => {
        if (box.labId !== labId) return false;
        if (storageContainerId !== undefined && box.storageContainerId !== storageContainerId) {
          return false;
        }
        return true;
      }),
    };
  },

  'item-type/list': (lab, message) => {
    const { labId, includeArchived } = fields(message) as {
      labId: string;
      includeArchived: boolean;
    };
    return {
      itemTypes: lab.itemTypes.filter(
        (candidate) =>
          candidate.labId === labId && (includeArchived || candidate.archivedAt === undefined),
      ),
    };
  },

  // `ListCustomFieldDefinitions` filters `item_type_id` by equality, so a
  // lab-scoped definition is not returned for an item-type query.
  'custom-field-def/list': (lab, message) => {
    const { labId, itemTypeId } = fields(message) as { labId: string; itemTypeId?: string };
    requireId(labId, 'lab');
    return {
      cfds: lab.customFieldDefs.filter((cfd) => {
        if (cfd.labId !== labId) return false;
        if (itemTypeId !== undefined && cfd.itemTypeId !== itemTypeId) return false;
        return true;
      }),
    };
  },

  // ---- SampleService ----
  'sample/list': (lab, message) => {
    const {
      labId,
      includeArchived,
      boxId,
      itemTypeId,
      barcode,
      status,
      query,
      page: pageRequest,
    } = fields(message) as {
      labId: string;
      includeArchived: boolean;
      boxId?: string;
      itemTypeId?: string;
      barcode?: string;
      status?: SampleStatus;
      query?: string;
      page?: JsonValue;
    };
    requireId(labId, 'lab');
    if (query !== undefined && utf8ByteLength(query) < MIN_QUERY_BYTES) {
      throw new FakeRpcError(
        'INVALID_ARGUMENT',
        `query must be at least ${String(MIN_QUERY_BYTES)} characters`,
      );
    }
    const matching = lab.samples.filter((candidate) => {
      if (candidate.labId !== labId) return false;
      if (!includeArchived && candidate.status === SampleStatus.TOMBSTONED) return false;
      if (boxId !== undefined && candidate.boxId !== boxId) return false;
      if (itemTypeId !== undefined && candidate.itemTypeId !== itemTypeId) return false;
      if (barcode !== undefined && candidate.barcode !== barcode) return false;
      if (
        status !== undefined &&
        status !== SampleStatus.UNSPECIFIED &&
        candidate.status !== status
      ) {
        return false;
      }
      if (query !== undefined && !matchesQuery(candidate, query)) return false;
      return true;
    });
    const { slice, token } = samplePage(matching, pageRequest);
    return { samples: slice, page: { nextPageToken: token } };
  },

  'sample/get': (lab, message) => {
    const { sampleId } = fields(message) as { sampleId: string };
    requireId(sampleId, 'sample');
    const found = lab.samples.find((candidate) => candidate.id === sampleId);
    if (found === undefined) throw new FakeRpcError('NOT_FOUND', 'no such sample');
    return { sample: found };
  },

  // `ExportSamplesCsvRequest` carries only `lab_id` and `include_archived`: the
  // export is lab-wide, so the screen's filters do not narrow it (G3.2 notes
  // this as a known limit rather than pretending otherwise).
  'sample/export': (lab, message) => {
    const { labId, includeArchived } = fields(message) as {
      labId: string;
      includeArchived: boolean;
    };
    requireId(labId, 'lab');
    return {
      csvContent: exportSamplesCsv(
        lab.samples.filter(
          (candidate) =>
            candidate.labId === labId &&
            (includeArchived || candidate.status !== SampleStatus.TOMBSTONED),
        ),
      ),
    };
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
