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
import { AuditEventSchema, type AuditEvent } from '../gen/fmgr/v1/audit_pb';
import {
  BoxPositionSchema,
  BoxSchema,
  BoxTypeSchema,
  ContainerKind,
  ContainerTypeSchema,
  FreezerSchema,
  StorageContainerSchema,
  type Box,
  type BoxPosition,
  type BoxType,
  type ContainerType,
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
import { CheckoutAction, SampleSchema, SampleStatus, type Sample } from '../gen/fmgr/v1/sample_pb';

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

/**
 * One row of the chain of custody `storage::apply_checkout()` appends
 * (`core::CheckoutEvent`). The volume lands *here*, not only on the sample row:
 * `volumeDelta` is signed and expressed in the sample's own unit, so negative
 * is consumed — the same convention the C++ struct documents.
 */
export interface CheckoutEventRecord {
  sampleId: string;
  action: CheckoutAction;
  /** Signed quantity change in the sample's unit; absent when nothing moved. */
  volumeDelta?: number;
  volumeUnit?: string;
  reason?: string;
}

/** The units `core::parse_volume_unit` accepts — anything else is refused. */
const VOLUME_UNITS = ['mL', 'µL'] as const;

type VolumeUnit = (typeof VOLUME_UNITS)[number];

function isVolumeUnit(value: string): value is VolumeUnit {
  return (VOLUME_UNITS as readonly string[]).includes(value);
}

/**
 * `core::Volume::to_unit` in miniature. `rawValue` is an integer count as
 * `Volume::from_raw` stores it, and µL → mL truncates toward zero exactly as
 * the C++ integer division does.
 *
 * An unknown *target* is a broken fixture rather than a server answer, so it
 * throws instead of guessing: subtracting the wrong amount silently is the
 * failure mode this fake exists to make impossible.
 */
function convertVolume(rawValue: number, from: VolumeUnit, to: string): number {
  if (from === to) {
    return rawValue;
  }
  if (!isVolumeUnit(to)) {
    throw new Error(
      `fakeApi: sample volume_unit '${to}' is not a unit core::parse_volume_unit accepts`,
    );
  }
  return from === 'mL' ? rawValue * 1_000 : Math.trunc(rawValue / 1_000);
}

export interface DemoLab {
  labs: Lab[];
  itemTypes: ItemType[];
  /**
   * The lab's custom-field definitions (`custom-field-def/list`), which the
   * G3.2 column chooser turns into columns. The *route* is gated on
   * `sample.read` since #69 — `ItemTypeServiceImpl::ListCustomFieldDefinitions`
   * — so a ReadOnly member is served them. This fake models no permissions at
   * all: a refusal is scripted per RPC with
   * `fail: { 'custom-field-def/list': 'PERMISSION_DENIED' }`, and the screen has
   * to survive that — see `useCustomFieldDefinitions`.
   */
  customFieldDefs: CustomFieldDefinition[];
  samples: Sample[];
  /** Chain of custody for the seeded samples; `audit/list` filters it. */
  auditEvents: AuditEvent[];
  /**
   * The events `storage::apply_checkout()` appends. A separate list because
   * `checkout_event` is a separate table from `audit_event`, and it is the only
   * place the consumed volume is recorded (#100).
   */
  checkoutEvents: CheckoutEventRecord[];
  /** Layout (BoxService): the physical tree the G3.1 screen renders. */
  freezers: Freezer[];
  storageContainers: StorageContainer[];
  containerTypes: ContainerType[];
  boxTypes: BoxType[];
  boxes: Box[];
  /** The browser-session state `auth/*` reads and mutates (G2.1). */
  auth: FakeAuth;
}

/**
 * The accounts the `auth/*` routes accept.
 *
 * Synthetic credentials, deliberately not shared with any real deployment: the
 * login route's whole job is to separate an accepted password from a refused
 * one, and a fake that accepts everything (or answers with the response
 * message's defaults, which is what an unimplemented resolver did) makes every
 * "wrong password" branch untestable while looking tested.
 */
export interface FakeAuthAccount {
  readonly userId: string;
  readonly email: string;
  readonly password: string;
  /** `true` when the account has a TOTP secret — login answers `mfa_required`. */
  readonly requiresMfa: boolean;
}

export interface FakeAuth {
  readonly accounts: readonly FakeAuthAccount[];
  /**
   * The account whose password was accepted and whose TOTP code is still
   * outstanding — the session the browser holds between `login` and
   * `submit-mfa`. `null` when no login is half-finished.
   *
   * This is the fake's stand-in for the session cookie: jsdom's `fetch` does not
   * apply `Set-Cookie` to `document.cookie`, so the real cookie jar cannot be
   * modelled here. What matters for the tests is the *state* it implies.
   */
  pendingMfaUserId: string | null;
}

/** The password `fakeApi()` accepts for every seeded account. */
export const DEMO_PASSWORD = 'demo-password';

/** The TOTP code `fakeApi()` accepts (`submit-mfa`). */
export const DEMO_TOTP_CODE = '123456';

/** The account without a second factor: password login is enough. */
export const DEMO_USER_EMAIL = 'demo@example.test';

/** The account with TOTP: login answers `mfa_required: true`. */
export const DEMO_MFA_EMAIL = 'mfa@example.test';

/** A seeded, in-memory demo lab. Pass your own to `fakeApi({ lab })` to inspect it. */
/** A real `Timestamp` message, not a bare object: nested messages must be messages. */
const seedTimestamp = () => create(TimestampSchema, { unixMicros: 1_758_931_200_000_000n });

/**
 * `rows × cols` positions, labelled `A1`… like a real box map.
 *
 * `accepts` is the sorted list of container-type size classes the position
 * takes (`BoxPosition.accepts`, filled by `BoxServiceImpl::fill_box_type` from
 * `box_type_position_accepts`). An empty list means "no constraint is known",
 * which is what a box type created without accepts looks like — the server then
 * rejects every container type at that position, and only the server can say so.
 */
function seedPositions(rows: number, cols: number, accepts: readonly string[] = []): BoxPosition[] {
  return Array.from({ length: rows * cols }, (_, index) => {
    const row = Math.floor(index / cols) + 1;
    const col = (index % cols) + 1;
    return create(BoxPositionSchema, {
      label: `${String.fromCharCode(64 + row)}${String(col)}`,
      row,
      col,
      accepts: [...accepts],
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
    containerTypes: [
      create(ContainerTypeSchema, {
        id: 'ct-15ml',
        labId: 'lab-demo',
        name: '15 mL tube',
        sizeClass: 'tube-15',
        createdAt,
      }),
      create(ContainerTypeSchema, {
        id: 'ct-50ml',
        labId: 'lab-demo',
        name: '50 mL tube',
        sizeClass: 'tube-50',
        createdAt,
      }),
      create(ContainerTypeSchema, {
        id: 'ct-second',
        labId: 'lab-second',
        name: 'Second lab tube',
        sizeClass: 'tube-15',
        createdAt,
      }),
    ],
    boxTypes: [
      create(BoxTypeSchema, {
        id: 'bt-96',
        labId: 'lab-demo',
        name: '96-well',
        // Both demo container types fit a 96-well position, so a sample that
        // merely *has* a container type is not rejected here.
        positions: seedPositions(8, 12, ['tube-15', 'tube-50']),
        createdAt,
      }),
      create(BoxTypeSchema, {
        id: 'bt-9',
        labId: 'lab-demo',
        name: '9-place',
        // Only the 15 mL tube fits: placing a 50 mL tube here is the
        // size-class rejection the server returns as INVALID_ARGUMENT.
        positions: seedPositions(3, 3, ['tube-15']),
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
      // The inheritance chain G3.3 walks: Blood → {Serum, Plasma}. Inherited
      // definitions are exactly the case a leaf-only resolver gets wrong.
      create(ItemTypeSchema, { id: 'it-blood', labId: 'lab-demo', name: 'Blood', createdAt }),
      create(ItemTypeSchema, {
        id: 'it-serum',
        labId: 'lab-demo',
        parentId: 'it-blood',
        name: 'Serum',
        createdAt,
      }),
      create(ItemTypeSchema, {
        id: 'it-plasma',
        labId: 'lab-demo',
        parentId: 'it-blood',
        name: 'Plasma',
        createdAt,
      }),
      // A leaf with a required field, kept off the item types the other feature
      // tests use so the required rule cannot change their results.
      create(ItemTypeSchema, {
        id: 'it-tissue',
        labId: 'lab-demo',
        parentId: 'it-blood',
        name: 'Tissue',
        createdAt,
      }),
      create(ItemTypeSchema, { id: 'it-dna', labId: 'lab-second', name: 'DNA', createdAt }),
    ],
    // G3.2's four: two item-type fields, one lab-wide field and one in the other
    // lab — the shape `ListCustomFieldDefinitions` filters on, since
    // `item_type_id` is compared for equality and a lab-scoped definition is
    // therefore absent from an item-type query.
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
      // G3.3 adds the inheritance chain (Blood → Serum/Plasma/Tissue), one
      // definition per `FieldDataType`, a PHI field, a key that is redefined on
      // the child, and a required field on an item type the other feature tests
      // do not use.
      ...seedCustomFieldDefinitions(createdAt),
    ],
    auditEvents: seedAuditEvents(createdAt),
    checkoutEvents: [],
    samples: [
      seedSample({
        id: 'sample-1',
        labId: 'lab-demo',
        itemTypeId: 'it-serum',
        name: 'Serum A',
        barcode: 'DEMO-0001',
        boxId: 'box-1',
        positionLabel: 'A1',
        volumeValue: 100,
        volumeUnit: 'µL',
        customFieldsJson: JSON.stringify({
          // G3.2's browser column chooser renders this one; the rest are G3.3's,
          // spread over the definition types and the inheritance chain.
          concentration: '12.5',
          notes: 'ok',
          aliquot_count: 3,
          is_hemolyzed: true,
          collection_date: '2026-01-05',
        }),
      }),
      seedSample({
        id: 'sample-2',
        labId: 'lab-demo',
        itemTypeId: 'it-serum',
        name: 'Serum B',
        barcode: 'DEMO-0002',
        boxId: 'box-1',
        positionLabel: 'A2',
        parentSampleId: 'sample-1',
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
      // Depleted parents and other states are left to the tests that need them:
      // growing this list would change every `sample/list` page assertion.
      seedSample({
        id: 'sample-4',
        labId: 'lab-second',
        itemTypeId: 'it-dna',
        name: 'DNA A',
        barcode: 'DEMO-0004',
      }),
    ],
    auth: {
      accounts: [
        {
          userId: 'user-demo',
          email: DEMO_USER_EMAIL,
          password: DEMO_PASSWORD,
          requiresMfa: false,
        },
        {
          userId: 'user-mfa',
          email: DEMO_MFA_EMAIL,
          password: DEMO_PASSWORD,
          requiresMfa: true,
        },
      ],
      pendingMfaUserId: null,
    },
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

/**
 * The lab's custom-field definitions (G3.3), deliberately spread over the
 * inheritance chain and over every `FieldDataType`:
 *
 * `concentration` is deliberately **not** defined here: G3.2's seed already has
 * that key on `it-serum`, and a second definition would make "which one wins"
 * depend on the resolver rather than on the test.
 *
 * | key | attached to | type | why it is here |
 * |---|---|---|---|
 * | `notes` | Blood **and** Serum | TEXT | the most-derived definition must win |
 * | `aliquot_count` | Blood | INT | inherited from the parent |
 * | `donor_name` | Blood | TEXT, PHI | server-filtered by `phi.read` |
 * | `is_hemolyzed` | Serum | BOOL | leaf-only |
 * | `collection_date` | Blood | DATE | inherited |
 * | `received_at` | Blood | DATETIME | inherited |
 * | `tube_type` | Blood | ENUM | inherited, `values` constraint |
 * | `parent_aliquot` | Serum | REFERENCE | leaf-only UUID |
 * | `tissue_grade` | Tissue | TEXT, required | the required rule, on an unused item type |
 * | `ploidy` | DNA (lab-second) | TEXT | another lab's field must not leak |
 */
function seedCustomFieldDefinitions(createdAt: ReturnType<typeof seedTimestamp>) {
  const cfd = (init: MessageInitShape<typeof CustomFieldDefinitionSchema>): CustomFieldDefinition =>
    create(CustomFieldDefinitionSchema, {
      labId: 'lab-demo',
      scopeKind: ScopeKind.SAMPLE,
      required: false,
      validationJson: '{}',
      indexed: false,
      isPhi: false,
      createdAt,
      ...init,
    });

  return [
    cfd({
      id: 'cfd-notes-blood',
      itemTypeId: 'it-blood',
      key: 'notes',
      label: 'Notes',
      dataType: FieldDataType.TEXT,
      validationJson: JSON.stringify({ max_length: 20 }),
    }),
    cfd({
      id: 'cfd-notes-serum',
      itemTypeId: 'it-serum',
      key: 'notes',
      label: 'Serum notes',
      dataType: FieldDataType.TEXT,
      validationJson: JSON.stringify({ max_length: 5 }),
    }),
    cfd({
      id: 'cfd-aliquot-count',
      itemTypeId: 'it-blood',
      key: 'aliquot_count',
      label: 'Aliquot count',
      dataType: FieldDataType.INT,
      validationJson: JSON.stringify({ min: 1, max: 10 }),
    }),
    cfd({
      id: 'cfd-donor-name',
      itemTypeId: 'it-blood',
      key: 'donor_name',
      label: 'Donor name',
      dataType: FieldDataType.TEXT,
      isPhi: true,
    }),
    cfd({
      id: 'cfd-hemolyzed',
      itemTypeId: 'it-serum',
      key: 'is_hemolyzed',
      label: 'Hemolyzed',
      dataType: FieldDataType.BOOL,
    }),
    cfd({
      id: 'cfd-collection-date',
      itemTypeId: 'it-blood',
      key: 'collection_date',
      label: 'Collection date',
      dataType: FieldDataType.DATE,
    }),
    cfd({
      id: 'cfd-received-at',
      itemTypeId: 'it-blood',
      key: 'received_at',
      label: 'Received at',
      dataType: FieldDataType.DATETIME,
    }),
    cfd({
      id: 'cfd-tube-type',
      itemTypeId: 'it-blood',
      key: 'tube_type',
      label: 'Tube type',
      dataType: FieldDataType.ENUM,
      validationJson: JSON.stringify({ values: ['EDTA', 'heparin', 'plain'] }),
    }),
    cfd({
      id: 'cfd-parent-aliquot',
      itemTypeId: 'it-serum',
      key: 'parent_aliquot',
      label: 'Parent aliquot',
      dataType: FieldDataType.REFERENCE,
    }),
    cfd({
      id: 'cfd-tissue-grade',
      itemTypeId: 'it-tissue',
      key: 'tissue_grade',
      label: 'Tissue grade',
      dataType: FieldDataType.TEXT,
      required: true,
    }),
    // Another lab's field: a resolver that keys on `key` alone would leak it.
    cfd({ id: 'cfd-ploidy', labId: 'lab-second', key: 'ploidy', label: 'Ploidy' }),
  ];
}

/** Chain of custody for `sample-1`, plus rows that must be filtered out. */
function seedAuditEvents(createdAt: ReturnType<typeof seedTimestamp>) {
  const event = (init: MessageInitShape<typeof AuditEventSchema>): AuditEvent =>
    create(AuditEventSchema, {
      actorUserId: 'user-1',
      entityKind: 'sample',
      at: createdAt,
      ...init,
    });

  return [
    event({ id: 'audit-1', labId: 'lab-demo', entityId: 'sample-1', action: 'sample.create' }),
    event({ id: 'audit-2', labId: 'lab-demo', entityId: 'sample-1', action: 'sample.update' }),
    event({ id: 'audit-3', labId: 'lab-demo', entityId: 'sample-1', action: 'sample.checkout' }),
    // A different sample in the same lab, and the same sample id in another lab.
    event({ id: 'audit-other', labId: 'lab-demo', entityId: 'sample-2', action: 'sample.create' }),
    event({ id: 'audit-lab2', labId: 'lab-second', entityId: 'sample-1', action: 'sample.create' }),
  ];
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

/**
 * `AuthServiceImpl::Login` / `LocalAuthProvider::authenticate`: an unknown email
 * and a wrong password are the *same* refusal on purpose (no account
 * enumeration), and both are `UNAUTHENTICATED`.
 */
function passwordLogin(lab: DemoLab, message: Message, options: { withToken: boolean }) {
  const { email, password } = fields(message) as { email: string; password: string };
  const account = lab.auth.accounts.find((candidate) => candidate.email === email);
  if (account?.password !== password) {
    throw new FakeRpcError('UNAUTHENTICATED', 'invalid email or password');
  }

  // The second factor is still outstanding until `submit-mfa` is accepted.
  lab.auth.pendingMfaUserId = account.requiresMfa ? account.userId : null;

  return {
    ...(options.withToken ? { sessionToken: `token-${account.userId}` } : {}),
    sessionId: `session-${account.userId}`,
    userId: account.userId,
    mfaRequired: account.requiresMfa,
  };
}

/**
 * `LocalAuthProvider::verify_totp`: `InvalidCredentials` — `UNAUTHENTICATED`
 * with no `mfa_required:` prefix — for a missing session, an already-complete
 * one and a wrong code alike. The prefix distinction is why the SPA cannot read
 * "wrong code" out of the status alone and has to re-ask who it is.
 */
function verifyTotp(lab: DemoLab, message: Message) {
  const { totpCode } = fields(message) as { totpCode: string };
  const pending = lab.auth.pendingMfaUserId;
  if (pending === null) {
    throw new FakeRpcError('UNAUTHENTICATED', 'invalid session for TOTP verification');
  }
  if (totpCode !== DEMO_TOTP_CODE) {
    throw new FakeRpcError('UNAUTHENTICATED', 'invalid TOTP code');
  }
  lab.auth.pendingMfaUserId = null;
  return {};
}

/**
 * The routes a session with an outstanding second factor may still call, and
 * the reason the server registers them `token_only`/`no_credential` (#62): a
 * half-finished login must be resumable *and* abandonable.
 */
const PENDING_MFA_EXEMPT_ROUTES: ReadonlySet<RpcName> = new Set([
  'auth/login',
  'auth/browser/login',
  'auth/submit-mfa',
  'auth/browser/submit-mfa',
  'auth/logout',
  'auth/browser/logout',
]);

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
/** Partial on purpose: a newer server may send a status this bundle does not know. */
const CSV_STATUS: Readonly<Partial<Record<number, string>>> = {
  [SampleStatus.UNSPECIFIED]: '',
  [SampleStatus.ACTIVE]: 'active',
  [SampleStatus.CHECKED_OUT]: 'checked_out',
  [SampleStatus.DEPLETED]: 'depleted',
  [SampleStatus.DESTROYED]: 'destroyed',
  [SampleStatus.TOMBSTONED]: 'tombstoned',
};

/** RFC 4180 quoting: only what needs it, doubled quotes inside. */
function csvCell(value: string | undefined): string {
  // An unset optional field, or a status this bundle does not know: the column
  // is empty, exactly as `freezerctl` writes it.
  if (value === undefined) {
    return '';
  }
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
 * The fake's own copy of `storage::resolve_custom_field_defs` — the *server's*
 * inheritance rule, written independently of the screen's resolver on purpose.
 * If the fake reused the feature's resolver, a bug in it would make every
 * inheritance test pass for the wrong reason.
 *
 * Lab-global definitions apply to every item type; a definition attached to an
 * ancestor applies to its descendants; on a duplicate `key` the most-derived
 * definition wins (leaf > parent > … > global).
 */
function fakeResolveCfds(lab: DemoLab, itemTypeId: string): CustomFieldDefinition[] {
  const lineage: string[] = [];
  const seen = new Set<string>();
  let cursor: string | undefined = itemTypeId;
  while (cursor !== undefined && !seen.has(cursor)) {
    seen.add(cursor);
    lineage.push(cursor);
    cursor = lab.itemTypes.find((candidate) => candidate.id === cursor)?.parentId;
  }
  // Rank: leaf highest, then its parent, …; lab-global definitions rank 0.
  const rank = new Map(lineage.map((id, index) => [id, lineage.length - index]));
  const best = new Map<string, { rank: number; cfd: CustomFieldDefinition }>();

  for (const cfd of lab.customFieldDefs) {
    if (cfd.labId !== lab.itemTypes.find((it) => it.id === itemTypeId)?.labId) continue;
    if (cfd.scopeKind !== ScopeKind.SAMPLE) continue;
    if (cfd.archivedAt !== undefined) continue;
    let specificity = 0;
    if (cfd.itemTypeId !== undefined) {
      const found = rank.get(cfd.itemTypeId);
      if (found === undefined) continue; // outside this lineage
      specificity = found;
    }
    const slot = best.get(cfd.key);
    if (slot === undefined || specificity >= slot.rank) {
      best.set(cfd.key, { rank: specificity, cfd });
    }
  }
  return [...best.values()].map((entry) => entry.cfd);
}

/**
 * The server's `core::validate_custom_fields` message, rendered exactly as
 * `prepare_custom_fields()` in `SampleServiceImpl.cc` renders it:
 * `custom field validation failed: [key: message] […]`. Only the rules the
 * seeded definitions exercise are implemented; anything else is the caller's
 * problem on a real server, and the injection hook still covers it.
 */
function fakeFieldErrors(
  definitions: readonly CustomFieldDefinition[],
  values: Record<string, unknown>,
): string[] {
  const errors: string[] = [];
  for (const def of definitions) {
    const value = values[def.key];
    const present = value !== undefined && value !== null;
    if (def.required && !present) {
      errors.push(`${def.key}: required field is missing or null`);
      continue;
    }
    if (!present) continue;

    const constraints = JSON.parse(def.validationJson || '{}') as Record<string, unknown>;
    switch (def.dataType) {
      case FieldDataType.TEXT:
        if (typeof value !== 'string') {
          errors.push(`${def.key}: expected string value`);
        } else if (
          typeof constraints.max_length === 'number' &&
          value.length > constraints.max_length
        ) {
          errors.push(
            `${def.key}: string length ${String(value.length)} exceeds max_length ${String(constraints.max_length)}`,
          );
        }
        break;
      case FieldDataType.INT:
        if (typeof value !== 'number' || !Number.isInteger(value)) {
          errors.push(`${def.key}: expected integer value`);
        } else if (typeof constraints.min === 'number' && value < constraints.min) {
          errors.push(`${def.key}: value is below minimum`);
        } else if (typeof constraints.max === 'number' && value > constraints.max) {
          errors.push(`${def.key}: value exceeds maximum`);
        }
        break;
      case FieldDataType.FLOAT:
        if (typeof value !== 'number') {
          errors.push(`${def.key}: expected numeric value`);
        } else if (typeof constraints.min === 'number' && value < constraints.min) {
          errors.push(`${def.key}: value is below minimum`);
        } else if (typeof constraints.max === 'number' && value > constraints.max) {
          errors.push(`${def.key}: value exceeds maximum`);
        }
        break;
      case FieldDataType.BOOL:
        if (typeof value !== 'boolean') errors.push(`${def.key}: expected boolean value`);
        break;
      case FieldDataType.ENUM: {
        const allowed = Array.isArray(constraints.values) ? constraints.values : undefined;
        if (typeof value !== 'string') {
          errors.push(`${def.key}: expected string value for enum`);
        } else if (allowed !== undefined && !allowed.includes(value)) {
          errors.push(`${def.key}: value '${value}' is not in the allowed enum set`);
        }
        break;
      }
      case FieldDataType.DATE:
      case FieldDataType.DATETIME:
      case FieldDataType.REFERENCE:
      case FieldDataType.UNSPECIFIED:
        // Not exercised by the seeded definitions; the injection hook covers it.
        break;
    }
  }
  return errors;
}

/** Throws the server's `INVALID_ARGUMENT` for a rejected custom-field blob. */
function requireValidCustomFields(lab: DemoLab, itemTypeId: string, json: string): void {
  const values = (json === '' ? {} : JSON.parse(json)) as Record<string, unknown>;
  const errors = fakeFieldErrors(fakeResolveCfds(lab, itemTypeId), values);
  if (errors.length > 0) {
    const message = errors.reduce(
      (acc, error) => `${acc} [${error}]`,
      'custom field validation failed:',
    );
    throw new FakeRpcError('INVALID_ARGUMENT', message);
  }
}

/** The audit action name per checkout action, as the server records it. */
const CHECKOUT_AUDIT_ACTION: Readonly<Partial<Record<CheckoutAction, string>>> = {
  [CheckoutAction.UNSPECIFIED]: 'checkout',
  [CheckoutAction.CHECKOUT]: 'checkout',
  [CheckoutAction.CHECKIN]: 'checkin',
  [CheckoutAction.DISCARD]: 'discard',
};

/** The active sample already holding `(boxId, positionLabel)`, if any. */
function positionHolder(
  lab: DemoLab,
  boxId: string,
  positionLabel: string,
  exceptSampleId = '',
): Sample | undefined {
  return lab.samples.find(
    (candidate) =>
      candidate.id !== exceptSampleId &&
      candidate.boxId === boxId &&
      candidate.positionLabel === positionLabel &&
      candidate.status !== SampleStatus.TOMBSTONED,
  );
}

/**
 * The `container_type size_class is not accepted at this box position` rule
 * (`validate_sample()` in `SampleRepositories.cc`). Only enforced when both the
 * container type and the box are in the seed, so a test that posts an id the
 * fake has never heard of is not rejected for a reason the real server would
 * not use.
 */
function requireAcceptedSizeClass(
  lab: DemoLab,
  boxId: string,
  positionLabel: string,
  containerTypeId: string,
): void {
  const containerType = lab.containerTypes.find((candidate) => candidate.id === containerTypeId);
  const box = lab.boxes.find((candidate) => candidate.id === boxId);
  const boxType = lab.boxTypes.find((candidate) => candidate.id === box?.boxTypeId);
  const position = boxType?.positions.find((candidate) => candidate.label === positionLabel);
  if (containerType === undefined || position === undefined || position.accepts.length === 0) {
    return;
  }
  if (!position.accepts.includes(containerType.sizeClass)) {
    throw new FakeRpcError(
      'INVALID_ARGUMENT',
      'container_type size_class is not accepted at this box position',
    );
  }
}

/**
 * The server's item-type write rules (G3.9), so a write route is not more
 * generous than `freezerd`.
 *
 * `ItemTypeRepositories::check_no_cycle`, verbatim: walk the proposed parent
 * chain and refuse a repeat — the *entity's own id* appearing in it, or any
 * node visited twice because the stored data already contains a cycle. The
 * visited set is what makes the walk terminate on the data it is guarding
 * against; a depth limit would not.
 */
const ITEM_TYPE_CYCLE = 'item type parent chain forms a cycle';

function requireNoItemTypeCycle(lab: DemoLab, id: string, parentId: string | undefined): void {
  if (parentId === undefined) {
    return;
  }
  const visited = new Set<string>();
  let cursor: string | undefined = parentId;
  while (cursor !== undefined) {
    if (cursor === id || visited.has(cursor)) {
      throw new FakeRpcError('INVALID_ARGUMENT', ITEM_TYPE_CYCLE);
    }
    visited.add(cursor);
    cursor = lab.itemTypes.find((candidate) => candidate.id === cursor)?.parentId;
  }
}

/** `item_types.parent_id REFERENCES item_types(id)`: a dangling parent is a FK error. */
function requireItemTypeParent(lab: DemoLab, parentId: string | undefined): void {
  if (parentId === undefined || lab.itemTypes.some((candidate) => candidate.id === parentId)) {
    return;
  }
  throw new FakeRpcError(
    'FAILED_PRECONDITION',
    'execute sqlite item_type statement: FOREIGN KEY constraint failed',
  );
}

/**
 * `validate_item_type` plus `item_types_lab_name_unique`: the name is required,
 * and two live item types in one lab cannot share it.
 */
function requireItemTypeName(lab: DemoLab, labId: string, name: string, exceptId = ''): void {
  if (name === '') {
    throw new FakeRpcError('INVALID_ARGUMENT', 'item type name is required');
  }
  const duplicate = lab.itemTypes.some(
    (candidate) =>
      candidate.labId === labId &&
      candidate.id !== exceptId &&
      candidate.name === name &&
      candidate.archivedAt === undefined,
  );
  if (duplicate) {
    throw new FakeRpcError(
      'ALREADY_EXISTS',
      'execute sqlite item_type statement: UNIQUE constraint failed: item_types.lab_id, item_types.name',
    );
  }
}

/**
 * `validate_cfd_shape` + `reject_indexed_phi` + the
 * `cfd_lab_scope_type_key_unique` index.
 *
 * The PHI refusal carries the *service's* wording, not the storage layer's:
 * `ItemTypeServiceImpl::reject_indexed_phi` runs first, so
 * `detail::validate_cfd_shape`'s "PHI fields may not be indexed (see L10.3)" is
 * unreachable through this route. A client that classified on the storage
 * wording alone would miss every real refusal.
 */
function requireCfdShape(lab: DemoLab, wire: Partial<CustomFieldDefinition>, exceptId = ''): void {
  if (wire.scopeKind === undefined || wire.scopeKind === ScopeKind.UNSPECIFIED) {
    throw new FakeRpcError('INVALID_ARGUMENT', 'scope_kind is required');
  }
  if (wire.dataType === undefined || wire.dataType === FieldDataType.UNSPECIFIED) {
    throw new FakeRpcError('INVALID_ARGUMENT', 'data_type is required');
  }
  if ((wire.key ?? '') === '') {
    throw new FakeRpcError('INVALID_ARGUMENT', 'custom field key is required');
  }
  if ((wire.label ?? '') === '') {
    throw new FakeRpcError('INVALID_ARGUMENT', 'custom field label is required');
  }
  if (wire.isPhi === true && wire.indexed === true) {
    throw new FakeRpcError(
      'INVALID_ARGUMENT',
      'a PHI custom field may not be indexed (is_phi and indexed are mutually exclusive)',
    );
  }
  if (
    wire.itemTypeId !== undefined &&
    !lab.itemTypes.some((candidate) => candidate.id === wire.itemTypeId)
  ) {
    throw new FakeRpcError(
      'FAILED_PRECONDITION',
      'execute sqlite custom_field_definition statement: FOREIGN KEY constraint failed',
    );
  }
  const duplicate = lab.customFieldDefs.some(
    (candidate) =>
      candidate.id !== exceptId &&
      candidate.labId === wire.labId &&
      candidate.scopeKind === wire.scopeKind &&
      (candidate.itemTypeId ?? '') === (wire.itemTypeId ?? '') &&
      candidate.key === wire.key &&
      candidate.archivedAt === undefined,
  );
  if (duplicate) {
    throw new FakeRpcError(
      'ALREADY_EXISTS',
      'execute sqlite custom_field_definition statement: UNIQUE constraint failed: ' +
        'custom_field_definitions.lab_id, custom_field_definitions.scope_kind, custom_field_definitions.key',
    );
  }
}

/** A fresh id for a created row: the server mints a UUID, the fake a stable one. */
function nextFakeId(prefix: string, taken: readonly { id: string }[]): string {
  const ids = new Set(taken.map((row) => row.id));
  let index = ids.size + 1;
  while (ids.has(`${prefix}${String(index)}`)) {
    index += 1;
  }
  return `${prefix}${String(index)}`;
}

/**
 * The routes that answer with real demo data. Every other route in `routes.ts`
 * still gets a handler and can still be made to fail, but replies with the
 * response message's default values. A feature task that needs real data for
 * another route adds a resolver here — cheaper than every test stubbing its
 * own, and it keeps the fake one place.
 */
const resolvers: Partial<Record<RpcName, Resolver>> = {
  // ---- AuthService: the browser session (G2.1, gateway G0.1) ----
  //
  // `Login` answers a token in the body for `/auth/login` (scripts, the CLI)
  // and no token for `/auth/browser/login`, where the gateway moves it into an
  // `HttpOnly` cookie — see `RestGateway.cc`'s `success_response` overload. Both
  // set the fake's pending-MFA state, because both are the same RPC.
  'auth/login': (lab, message) => passwordLogin(lab, message, { withToken: true }),
  'auth/browser/login': (lab, message) => passwordLogin(lab, message, { withToken: false }),

  // `SubmitMfa` holds a token but not a completed second factor (#62), so it
  // answers the only question it can: is a code still outstanding, and is this
  // the right one? A code with no half-finished login behind it is refused the
  // way `verify_totp` refuses a session it cannot find.
  'auth/submit-mfa': (lab, message) => verifyTotp(lab, message),
  'auth/browser/submit-mfa': (lab, message) => verifyTotp(lab, message),

  // `Logout` is token-only for the same #62 reason: an abandoned login must be
  // able to give the credential up. It therefore also clears the pending state.
  'auth/logout': (lab) => {
    lab.auth.pendingMfaUserId = null;
    return {};
  },
  'auth/browser/logout': (lab) => {
    lab.auth.pendingMfaUserId = null;
    return {};
  },

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
  // lab-scoped definition is not returned for an item-type query — and it does
  // no ancestor resolution either, which is why the client walks the chain
  // itself. The server ignores `page` here, so this route returns everything
  // (`samplePage`'s doc comment lists the routes that really are paged).
  'custom-field-def/list': (lab, message) => {
    const { labId, itemTypeId } = fields(message) as { labId: string; itemTypeId?: string };
    requireId(labId, 'lab');
    return {
      cfds: lab.customFieldDefs.filter((cfd) => {
        if (cfd.labId !== labId) return false;
        if (cfd.archivedAt !== undefined) return false;
        if (itemTypeId !== undefined && cfd.itemTypeId !== itemTypeId) return false;
        return true;
      }),
    };
  },

  'item-type/get': (lab, message) => {
    const { itemTypeId } = fields(message) as { itemTypeId: string };
    requireId(itemTypeId, 'item type');
    const found = lab.itemTypes.find((candidate) => candidate.id === itemTypeId);
    if (found === undefined || found.archivedAt !== undefined) {
      throw new FakeRpcError('NOT_FOUND', 'item type not found');
    }
    return { itemType: found };
  },

  // ---- ItemTypeService writes (G3.9) ----
  //
  // Until G3.9 these six routes had no resolver, so a write answered with the
  // response message's *default* values: a test that posted a parent cycle
  // "succeeded". That is the worst kind of fake — the test that depends on it
  // passes — and it would have made the acceptance criterion "a cycle rejected
  // by the server" untestable while looking tested. The rules `freezerd`
  // enforces are mirrored here, message for message:
  //
  //  - `ItemTypeRepositories::check_no_cycle` on insert and update;
  //  - the `item_types_lab_name_unique` partial index on `(lab_id, name)`;
  //  - `ItemTypeServiceImpl::reject_indexed_phi`, ahead of the storage-layer
  //    `validate_cfd_shape` (which carries a second, differently worded
  //    refusal the service's own check makes unreachable);
  //  - `cfd_lab_scope_type_key_unique` on `(lab_id, scope_kind, item_type, key)`.
  //
  // What is deliberately *not* mirrored: lab PHI mode. `CreateCustomFieldDefinition`
  // does not consult `Lab.is_phi_enabled`; it is `SampleServiceImpl` that
  // refuses PHI *values* in a lab whose mode is off. The screen refuses the
  // definition earlier, and the fake must not pretend the server does.
  'item-type/create': (lab, message) => {
    const { labId, parentId, name } = fields(message) as {
      labId: string;
      parentId?: string;
      name: string;
    };
    requireId(labId, 'lab');
    requireItemTypeName(lab, labId, name);
    const created = create(ItemTypeSchema, {
      id: nextFakeId('it-created-', lab.itemTypes),
      labId,
      parentId,
      name,
      createdAt: seedTimestamp(),
    });
    requireItemTypeParent(lab, created.parentId);
    requireNoItemTypeCycle(lab, created.id, created.parentId);
    lab.itemTypes.push(created);
    return { itemType: created };
  },

  'item-type/update': (lab, message) => {
    const wire = (fields(message).itemType ?? {}) as Partial<ItemType>;
    const labId = wire.labId ?? '';
    const itemTypeId = wire.id ?? '';
    requireId(labId, 'lab');
    requireId(itemTypeId, 'item type');
    const found = lab.itemTypes.find((candidate) => candidate.id === itemTypeId);
    // The service answers NOT_FOUND when the id is unknown *or* belongs to
    // another lab, so the two cannot be told apart from outside.
    if (found?.labId !== labId) {
      throw new FakeRpcError('NOT_FOUND', 'item type not found');
    }
    requireItemTypeName(lab, labId, wire.name ?? '', itemTypeId);
    requireItemTypeParent(lab, wire.parentId);
    requireNoItemTypeCycle(lab, itemTypeId, wire.parentId);
    found.name = wire.name ?? '';
    found.parentId = wire.parentId;
    return { itemType: found };
  },

  'custom-field-def/create': (lab, message) => {
    const wire = (fields(message).cfd ?? {}) as Partial<CustomFieldDefinition>;
    requireId(wire.labId ?? '', 'lab');
    requireCfdShape(lab, wire);
    const created = create(CustomFieldDefinitionSchema, {
      id: nextFakeId('cfd-created-', lab.customFieldDefs),
      labId: wire.labId ?? '',
      scopeKind: wire.scopeKind ?? ScopeKind.UNSPECIFIED,
      itemTypeId: wire.itemTypeId,
      key: wire.key ?? '',
      label: wire.label ?? '',
      dataType: wire.dataType ?? FieldDataType.UNSPECIFIED,
      required: wire.required ?? false,
      validationJson:
        wire.validationJson === undefined || wire.validationJson === ''
          ? '{}'
          : wire.validationJson,
      indexed: wire.indexed ?? false,
      isPhi: wire.isPhi ?? false,
      createdAt: seedTimestamp(),
    });
    lab.customFieldDefs.push(created);
    return { cfd: created };
  },

  'custom-field-def/update': (lab, message) => {
    const wire = (fields(message).cfd ?? {}) as Partial<CustomFieldDefinition>;
    const labId = wire.labId ?? '';
    const cfdId = wire.id ?? '';
    requireId(labId, 'lab');
    requireId(cfdId, 'custom field');
    const found = lab.customFieldDefs.find((candidate) => candidate.id === cfdId);
    if (found?.labId !== labId) {
      throw new FakeRpcError('NOT_FOUND', 'custom field definition not found');
    }
    requireCfdShape(lab, wire, cfdId);
    // Mutable fields only, as in `UpdateCustomFieldDefinition`: `lab_id` and the
    // timestamps are not caller-editable.
    found.scopeKind = wire.scopeKind ?? found.scopeKind;
    found.itemTypeId = wire.itemTypeId;
    found.key = wire.key ?? '';
    found.label = wire.label ?? '';
    found.dataType = wire.dataType ?? found.dataType;
    found.required = wire.required ?? false;
    found.validationJson =
      wire.validationJson === undefined || wire.validationJson === '' ? '{}' : wire.validationJson;
    found.indexed = wire.indexed ?? false;
    found.isPhi = wire.isPhi ?? false;
    return { cfd: found };
  },

  // `BoxServiceImpl::ListContainerTypes` ignores `page` too.
  'container-type/list': (lab, message) => {
    const { labId } = fields(message) as { labId: string };
    requireId(labId, 'lab');
    return { containerTypes: lab.containerTypes.filter((candidate) => candidate.labId === labId) };
  },

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
    requireValidCustomFields(lab, init.itemTypeId ?? '', init.customFieldsJson ?? '{}');
    if (init.boxId !== undefined && init.positionLabel !== undefined) {
      if (positionHolder(lab, init.boxId, init.positionLabel) !== undefined) {
        // `samples_position_unique` (box_id, position_label): the SQLite backend
        // reports it as `execute sqlite sample statement: UNIQUE constraint
        // failed: …`, Postgres with its own wording — the code is the stable part.
        throw new FakeRpcError(
          'ALREADY_EXISTS',
          'execute sqlite sample statement: UNIQUE constraint failed: samples.box_id, samples.position_label',
        );
      }
      if (init.containerTypeId !== undefined) {
        requireAcceptedSizeClass(lab, init.boxId, init.positionLabel, init.containerTypeId);
      }
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
    requireValidCustomFields(lab, incoming.itemTypeId, incoming.customFieldsJson);
    if (incoming.boxId !== undefined && incoming.positionLabel !== undefined) {
      if (positionHolder(lab, incoming.boxId, incoming.positionLabel, incoming.id) !== undefined) {
        throw new FakeRpcError(
          'ALREADY_EXISTS',
          'execute sqlite sample statement: UNIQUE constraint failed: samples.box_id, samples.position_label',
        );
      }
      if (incoming.containerTypeId !== undefined) {
        requireAcceptedSizeClass(
          lab,
          incoming.boxId,
          incoming.positionLabel,
          incoming.containerTypeId,
        );
      }
    }
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
    if (destBoxId !== undefined && destPosition !== undefined) {
      if (positionHolder(lab, destBoxId, destPosition, sampleId) !== undefined) {
        throw new FakeRpcError(
          'ALREADY_EXISTS',
          'execute sqlite sample statement: UNIQUE constraint failed: samples.box_id, samples.position_label',
        );
      }
      if (found.containerTypeId !== undefined) {
        requireAcceptedSizeClass(lab, destBoxId, destPosition, found.containerTypeId);
      }
    }
    if (destBoxId !== undefined) found.boxId = destBoxId;
    if (destPosition !== undefined) found.positionLabel = destPosition;
    return { sample: found };
  },

  // The status machine of `storage::apply_checkout()`: CheckedOut requires
  // Active; CheckedIn requires CheckedOut and subtracts `volume_used`
  // (auto-depleting at zero); Discard requires Active|CheckedOut and consumes
  // whatever is left.
  //
  // The volume pair is **both or neither**, mirroring
  // `SampleServiceImpl::CheckoutSample` (#100): a `core::Volume` has no
  // unitless state, so a lone `volume_used` is INVALID_ARGUMENT rather than a
  // value the server quietly drops. Before this the fake applied the volume
  // regardless of the unit, which is exactly how the web check-in could lose a
  // typed volume against real `freezerd` with every test still green.
  //
  // An illegal transition answers **FAILED_PRECONDITION**, which is this fake's
  // contract from G1.2 (`fakeApi.test.ts`, `hooks/samples.test.tsx`) even though
  // the C++ `ConstraintViolation` maps to INVALID_ARGUMENT in
  // `GrpcErrorTranslation.h`. Changing it is a G1.2 decision, not a G3.3 one, so
  // it is reported rather than edited here.
  'sample/checkout': (lab, message) => {
    const { sampleId, action, volumeUsed, volumeUnit, reason } = fields(message) as {
      sampleId: string;
      action: CheckoutAction;
      volumeUsed?: number;
      volumeUnit?: string;
      reason?: string;
    };
    const found = lab.samples.find((candidate) => candidate.id === sampleId);
    if (found === undefined) throw new FakeRpcError('NOT_FOUND', 'no such sample');

    // Field validation runs before the transition, as it does in the handler, so
    // a malformed pair never changes the sample's state.
    if ((volumeUsed === undefined) !== (volumeUnit === undefined)) {
      throw new FakeRpcError(
        'INVALID_ARGUMENT',
        'volume_used and volume_unit must both be set or both empty',
      );
    }
    if (volumeUnit !== undefined && !isVolumeUnit(volumeUnit)) {
      throw new FakeRpcError('INVALID_ARGUMENT', `volume_unit: unknown unit: '${volumeUnit}'`);
    }
    // A consumption cannot be negative: the server refuses one with
    // `volume_used: must not be negative` instead of letting the subtraction
    // invert and *add* stock (#112), and the fake mirrors that so the client and
    // the server cannot disagree about it. The sign is read from the raw number,
    // as the handler reads it, because `Math.trunc` below would otherwise turn
    // -0.5 into a well-formed 0.
    if (volumeUsed !== undefined && volumeUsed < 0) {
      throw new FakeRpcError('INVALID_ARGUMENT', 'volume_used: must not be negative');
    }

    if (found.status === SampleStatus.TOMBSTONED || found.status === SampleStatus.DESTROYED) {
      throw new FakeRpcError('FAILED_PRECONDITION', 'sample is not in a checkout-eligible state');
    }

    // Signed change in the sample's own unit, exactly as `apply_checkout` signs
    // it; `undefined` when the transition moved no volume.
    let volumeDelta: number | undefined;

    switch (action) {
      // Before G3.3 the fake ignored `action` entirely and only ever checked the
      // sample out, so an omitted action must keep meaning "check out" — the
      // G1.2 tests that call `sample/checkout` with no action pin that.
      case CheckoutAction.UNSPECIFIED:
      case CheckoutAction.CHECKOUT:
        if (found.status !== SampleStatus.ACTIVE) {
          throw new FakeRpcError('FAILED_PRECONDITION', 'only an active sample can be checked out');
        }
        found.status = SampleStatus.CHECKED_OUT;
        break;
      case CheckoutAction.CHECKIN: {
        if (found.status !== SampleStatus.CHECKED_OUT) {
          throw new FakeRpcError(
            'FAILED_PRECONDITION',
            'only a checked-out sample can be checked in',
          );
        }
        found.status = SampleStatus.ACTIVE;
        // `volume_used` is only subtracted when the sample tracks a volume, as
        // in `apply_checkout`. The pair rule above means the two request fields
        // are present or absent together.
        if (
          volumeUsed !== undefined &&
          volumeUnit !== undefined &&
          found.volumeValue !== undefined &&
          found.volumeUnit !== undefined
        ) {
          const used = convertVolume(Math.trunc(volumeUsed), volumeUnit, found.volumeUnit);
          const previous = found.volumeValue;
          const remaining = Math.max(0, previous - used);
          volumeDelta = remaining - previous; // negative = consumed
          found.volumeValue = remaining;
          if (remaining === 0) found.status = SampleStatus.DEPLETED;
        }
        break;
      }
      case CheckoutAction.DISCARD:
        if (found.volumeValue !== undefined) {
          volumeDelta = -found.volumeValue;
          found.volumeValue = 0;
        }
        found.status = SampleStatus.DESTROYED;
        break;
    }
    found.lastModifiedAt = seedTimestamp();
    // `reason` and the volume delta belong to the chain-of-custody event, not
    // the sample row (`storage::apply_checkout`), so both are appended the same
    // way the server appends them — which also gives the history section
    // something to render.
    lab.checkoutEvents.push({
      sampleId,
      action,
      ...(volumeDelta === undefined ? {} : { volumeDelta, volumeUnit: found.volumeUnit }),
      ...(reason === undefined ? {} : { reason }),
    });
    lab.auditEvents.push(
      create(AuditEventSchema, {
        id: `audit-${sampleId}-${String(lab.auditEvents.length + 1)}`,
        labId: found.labId,
        at: seedTimestamp(),
        actorUserId: 'user-1',
        entityKind: 'sample',
        entityId: sampleId,
        action: `sample.${CHECKOUT_AUDIT_ACTION[action] ?? 'checkout'}`,
        requestId: reason ?? '',
      }),
    );
    return { sample: found };
  },

  // ---- AuditService: the sample's history (G3.3) ----
  'audit/list': (lab, message) => {
    const {
      labId,
      entityKind,
      entityId,
      page: pageRequest,
    } = fields(message) as {
      labId?: string;
      entityKind?: string;
      entityId?: string;
      page?: JsonValue;
    };
    const matching = lab.auditEvents.filter((candidate) => {
      if (labId !== undefined && candidate.labId !== labId) return false;
      if (entityKind !== undefined && candidate.entityKind !== entityKind) return false;
      if (entityId !== undefined && candidate.entityId !== entityId) return false;
      return true;
    });
    const { slice, token } = samplePage(matching, pageRequest);
    return { events: slice, page: { nextPageToken: token } };
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
        // Every RPC that needs a completed second factor — `AuthMiddleware`'s
        // `CredentialRule::TokenAndMfa` — refuses a pending-MFA session with
        // `UNAUTHENTICATED` and the `mfa_required: ` prefix. The routes below are
        // the #62 exception: finishing the login, giving the credential up and
        // logging in again are exactly what a half-finished session may still
        // call. Without this, a test could log in with an MFA account, never
        // submit a code, and still read lab data that production refuses.
        if (lab.auth.pendingMfaUserId !== null && !PENDING_MFA_EXEMPT_ROUTES.has(rpc as RpcName)) {
          return errorResponse(
            'UNAUTHENTICATED',
            'mfa_required: MFA required before this operation',
          );
        }

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
