# ADR 0003: A freezer scene API for the 3D layout view

- Status: Accepted design (2026-10-27); implementation is on the integration branch `feat/215-layout-3d`.
- Date: 2026-10-27.
- Design issue: [#215](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/215).
- Decision: the owner chose the code-defined built-in catalogue (§1) and the flat, token-styled renderer (§5)
  during the lead's review. The lead accepted the rest after the corrections listed under "Revisions".
- Demos: [`doc/design/layout-3d/`](../design/layout-3d/README.md), the CryoCube F740 and the TSX40086A.
- ADR requirement: [TODO.md Section N, N4](../../TODO.md#section-n--documentation-completeness-m0--ongoing).
  The change crosses proto, storage, core, server, REST and web.

## Context

The Layout page (`/labs/:labId/layout`, G3.1) draws each freezer as a
collapsible tree. The owner wants each freezer drawn as a 3D object that opens
to show its shelves, racks and boxes. PRD §9 already plans a "freezer 3D layout"
pane for the Qt client.

The current `BoxService` model can't describe a physical layout:

| Gap | Where |
|---|---|
| `Freezer.model` is free text with no geometry | `proto/fmgr/v1/box.proto`, `message Freezer` |
| A container's position is only `ordering_index`, plus a free-form `capacity_hint_json` | `message StorageContainer` |
| A box knows its container but not its tier or depth in it | `message Box` |
| The client needs four list calls and an orphan/cycle-guarded walk to build the tree | `src/web/src/features/layout/layoutModel.ts` |

## Decision

All changes are additive in `fmgr.v1` (`box.proto`). Field numbers continue the existing messages.

### 1. A built-in freezer model catalogue, defined in code

The catalogue is constant data in `src/core/freezer_models.h`: pure, with no I/O, and versioned
together with the renderers that draw each entry. It is not stored in the database.

```proto
message FreezerModel {
  string id = 1;                         // stable key, e.g. "eppendorf-cryocube-f740hi"
  string manufacturer = 2;               // "Eppendorf"
  string model_code = 3;                 // "F740hi"
  string display_name = 4;               // "CryoCube F740hi"
  FreezerForm form = 5;                  // UPRIGHT | CHEST
  Dimensions exterior_mm = 6;            // 1099 x 980 x 1973
  Dimensions interior_mm = 7;            //  865 x 622 x 1390
  HingeSide hinge = 8;                   // as seen from the front
  repeated InnerDoorSpec inner_doors = 9;       // bottom -> top; empty = no inner doors
  repeated CompartmentSpec compartments = 10;   // bottom -> top, level 0 first
  string render_key = 11;                // hand-built renderer, "" = generic cabinet
}
message Dimensions { int32 width_mm = 1; int32 depth_mm = 2; int32 height_mm = 3; }
enum FreezerForm { FREEZER_FORM_UNSPECIFIED = 0; FREEZER_FORM_UPRIGHT = 1; FREEZER_FORM_CHEST = 2; }
enum HingeSide { HINGE_SIDE_UNSPECIFIED = 0; HINGE_SIDE_LEFT = 1; HINGE_SIDE_RIGHT = 2; }
message CompartmentSpec {
  int32 level = 1;                       // 0 = bottom
  int32 floor_mm = 2;                    // above the interior floor
  int32 height_mm = 3;
  int32 rack_slots = 4;                  // side-by-side rack positions, left -> right
  Dimensions rack_slot_mm = 5;
}
// One insulated inner door and the compartment levels it closes off. A door may span several
// levels (the TSX40086A has 2 doors over 4 levels), so this is not a flag per compartment.
message InnerDoorSpec { int32 first_level = 1; int32 last_level = 2; }
```

`Freezer` gains `string freezer_model_id = 10;`, where `""` means no model. `CreateFreezerRequest` gains
`string freezer_model_id = 7;`, and `UpdateFreezer` takes it through the `Freezer` message.

- The server refuses an id that isn't in the catalogue with `INVALID_ARGUMENT`.
- An id that the catalogue later drops still reads back. The client draws a generic cabinet for it.

The first two entries:

| | CryoCube F740hi | TSX Universal TSX40086A |
|---|---|---|
| `id` / `render_key` | `eppendorf-cryocube-f740hi` / `cryocube-f740` | `thermo-tsx40086a` / `tsx-40086` |
| Outside (W x D x H mm) | 1099 x 980 x 1973 | 719 x 955 x 1981 |
| Inside | 865 x 622 x 1390 | 588 x 719 x 1301 |
| Hinge | right | right |
| Compartments | 3, about 457 mm each, 6 rack slots each | 4, about 325 mm each, 4 rack slots each |
| Inner doors | 3, one per compartment | 2, covering levels 0-1 and 2-3 |
| Typical rack grid | 8 tiers x 1 x 4 deep (576 two-inch boxes) | 5 drawers x 1 x 5 deep (400 two-inch boxes) |

Core validation keeps every entry honest, and a unit test runs it over the whole catalogue:
- Levels run 0..n-1 without gaps.
- Compartments are in bounds inside `interior_mm`.
- Inner-door ranges are in bounds and don't overlap.
- Every dimension is positive.

### 2. Typed placement

```proto
message Placement {
  int32 level = 1;    // vertical, 0 = bottom (compartment in a freezer, tier in a rack)
  int32 column = 2;   // left -> right as seen from the front
  int32 depth = 3;    // front -> back, 0 = front
}
message GridSpec { int32 levels = 1; int32 columns = 2; int32 depths = 3; }
```

- `StorageContainer` gains `optional Placement placement = 11; optional GridSpec grid = 12;`.
  - `grid` replaces `capacity_hint_json`. That field stays readable and is marked `deprecated`.
- `Box` gains `optional Placement placement = 10;`.
- `CreateStorageContainerRequest` gains `optional Placement placement = 7; optional GridSpec grid = 8;`.
- `CreateBoxRequest` gains `optional Placement placement = 7;`.
- The Update requests already carry the whole message.

**The cabinet grid.** The layout root container (`Freezer.layout_root_id`) is the cabinet. Its direct
children take `Placement{level = compartment, column = rack slot, depth = 0}`. The bound for each
placement comes from that compartment's `rack_slots`, not from a rectangular grid. A root container
never stores its own `grid`; the server derives it from the freezer's model.

Validation lives in `src/core/placement.h` and stays pure:
- A placement must fall inside the parent's grid. That is the container's `grid`, or the cabinet grid
  when the parent is a layout root whose freezer has a model.
- A placement isn't allowed when the parent has no grid.
- Every grid dimension is at least 1 and at most 64.
- Two non-archived siblings can't share a placement. A partial unique index enforces this as well
  (§4), so a race ends in `ALREADY_EXISTS`, not a double booking.
- A change can't strand a placed child:
  - a grid can't shrink below an existing child's placement;
  - a freezer's model can't change while a placed child would fall outside the new cabinet grid.
  Both are refused with `FAILED_PRECONDITION`.

Unplaced containers and boxes stay legal, so no existing lab needs a backfill. The 3D view lists them under "Unplaced".

### 3. Read RPCs

```proto
rpc GetFreezerScene(GetFreezerSceneRequest) returns (GetFreezerSceneResponse);
rpc ListFreezerModels(ListFreezerModelsRequest) returns (ListFreezerModelsResponse);

message GetFreezerSceneRequest { string freezer_id = 1; }
message GetFreezerSceneResponse {
  Freezer freezer = 1;
  optional FreezerModel model = 2;       // absent -> generic cabinet
  repeated SceneNode nodes = 3;          // non-archived, under layout_root_id, parents first
}
enum SceneNodeKind { SCENE_NODE_KIND_UNSPECIFIED = 0; SCENE_NODE_KIND_CONTAINER = 1; SCENE_NODE_KIND_BOX = 2; }
message SceneNode {
  string id = 1;
  optional string parent_id = 2;         // absent only on the layout root
  SceneNodeKind kind = 3;
  ContainerKind container_kind = 4;      // containers only
  string label = 5;                      // container label, else name; box label
  optional Placement placement = 6;
  optional GridSpec grid = 7;
  int32 occupied_positions = 8;          // boxes: counts only
  int32 total_positions = 9;
}
message ListFreezerModelsRequest { string lab_id = 1; }
message ListFreezerModelsResponse { repeated FreezerModel models = 1; }
```

- **`GetFreezerScene` authorization.** It is an entity-id read, so it is registered with
  `rpc::RpcGate::token_and_mfa()`, as `GetFreezer` is in `src/server/BoxServiceImpl.cc`.
  - The handler asks the gate for that rule with `authenticate(…, TokenAndMfa)`.
  - It then checks `sample.read` with `has_for_lab` on the freezer's lab. That is the layout-read
    permission (#54); `GetFreezer` itself checks `freezer.configure`.
  - The registration comment states the rule (AGENTS.md §5, #119).
  - An archived freezer, or one in another lab, returns `NOT_FOUND` / `PERMISSION_DENIED`, as
    `GetFreezer` does.
- **`ListFreezerModels`** carries `lab_id` only so the RBAC gate has a lab to check. It is registered
  with `P::SampleRead` and its handler calls `authorize(…, SampleRead, lab_id)`. The response is the
  whole catalogue.
- **The scene walk** starts at `layout_root_id`.
  - It runs parents first, with the cycle and orphan guards of `layoutModel.ts` and
    `src/qt/LocationPathResolver.cc`.
  - Nodes are emitted in `ordering_index`, then name, then id order (containers) and label, then id
    order (boxes).
- **Occupancy** is counted on the server from the non-archived samples whose `box_id` is in the
  scene's boxes, through the typed DSL (`in`).
  - `total_positions` is the size of the box type's position list.
  - A backend count method can replace this if profiling asks for one.
- **No PHI.** Nodes carry only labels, geometry and position counts. No sample field reaches the
  scene, and no `phi.read` is noted.
- **Audit.** Both RPCs are read-only. The placement, grid and model writes ride on the existing
  `Create/Update{Freezer,StorageContainer,Box}` mutations, which already write their audit row in the
  same transaction; the audited entity snapshot now includes the new fields.
- **REST**, in the gateway's `POST /api/v1/<entity>/<verb>` convention (`src/rest/RestGateway.cc`):
  - `POST /api/v1/freezer/scene`
  - `POST /api/v1/freezer-model/list`
- **Freshness.** The web client invalidates the scene query on the existing sample-watch events. A
  scene stream is out of scope.

### 4. Storage

There is one migration, with the same version in both backends (`lock:migration` at merge to `main`):

- `freezers.freezer_model_id TEXT NOT NULL DEFAULT ''`.
- On `storage_containers`: nullable `placement_level`, `placement_column`, `placement_depth`,
  `grid_levels`, `grid_columns` and `grid_depths`.
  - A CHECK constraint makes each triple all-null or all-set, with placement values ≥ 0 and grid
    values in 1..64.
- The same nullable `placement_*` triple, with the same CHECK, on `boxes`.
- Partial unique indexes WHERE `archived_at IS NULL AND placement_level IS NOT NULL`:
  - on `storage_containers (parent_id, placement_level, placement_column, placement_depth)`;
  - on `boxes (storage_container_id, placement_level, placement_column, placement_depth)`.

No table is added, and no RLS policy changes: the new columns sit on lab-scoped tables that already
have their policies. The backends must pass `tests/backend_conformance/`.

On the integration branch the migration is version 15. If `main` takes 15 first (#179), it is
renumbered before the track merges.

### 5. Web rendering

- The renderer is a zero-dependency SVG isometric engine in the Layout route's lazy chunk.
  - It needs no `lock:deps`, and it leaves the 250 KiB initial-bundle budget alone.
- **Style:** flat shading from the app's design tokens (`src/web/src/ui/tokens.css`), with light and
  dark themes. The demos' inked look, see-through view and temperature readout stay in the demos.
- `render_key` picks a hand-built model (`cryocube-f740`, `tsx-40086`). Otherwise the client builds a
  generic cabinet from the model's dimensions and compartments, or from a default cabinet when the
  freezer has no model.
- The tree stays on the page as the accessible text alternative, and it is the default below 600 px.
- Interaction path:
  1. Open the door.
  2. Pick a compartment (its inner door opens).
  3. Pull a rack out.
  4. Click a box to open `/labs/:labId/boxes/:boxId` (G3.4).
- HTML controls drive the same steps, so the view works from the keyboard.

## Revisions from the proposal

The proposal, as reviewed on `docs/215-freezer-3d-layout`, was revised as follows:

| Proposal | Accepted | Why |
|---|---|---|
| Catalogue in `freezer_models` / `freezer_model_compartments` tables, seeded with `lab_id = ""` | Constant data in `src/core` | Built-in rows would need RLS exceptions on lab-scoped tables, plus three tables and repos per backend. Lab-custom models have no consumer yet. |
| `CreateFreezerModel`, `ArchiveFreezerModel` | Deferred | Nothing to create without lab-custom models. A later ADR adds them with their own table. |
| `optional string freezer_model_id` | `string freezer_model_id`, `""` = none | This matches `layout_root_id` and the other id fields in `box.proto`. |
| `GET /api/v1/freezers/{id}/scene` and similar | `POST /api/v1/freezer/scene`, `POST /api/v1/freezer-model/list` | The gateway maps `POST /api/v1/<entity>/<verb>` 1:1 onto gRPC. |
| `ListFreezerModels` with no request fields | `lab_id` field, registered `sample.read` | A `Permission` registration needs a lab to check (#78). |
| Only the F740 seeded | F740 and TSX40086A | Both demos become real views. |
| Numbered ADR 0002 | ADR 0003 | ADR 0002 is the MFA design (#208). |

## Alternatives considered

- **Keep `capacity_hint_json` and the four list calls.** This needs no proto change, but the geometry stays untyped and every client re-implements the parent walk. Rejected.
- **Geometry inside `Freezer` itself.** Every F740 in every lab would repeat the same dimensions, and correcting them would mean editing each row. Rejected in favour of a shared catalogue.
- **Catalogue in the database.** See "Revisions". It stays the route for lab-custom models.
- **WebGL / three.js.** It gives real lighting, but adds about 150 KiB gzipped, needs `lock:deps` and a GPU in kiosk browsers. Deferred. The scene API is renderer-neutral, so the decision can be revisited.

## Consequences

- Implementation slices, all merged into `feat/215-layout-3d` first:
  1. **3D-1:** proto, core catalogue and placement validation, migration and storage.
  2. **3D-2:** `GetFreezerScene`, `ListFreezerModels`, placement writes on the existing mutations, and REST.
  3. **3D-3:** the web 3D view.
  4. **3D-4:** admin placement editing, after G3.8.
- The Qt client can consume the same scene for its planned QML 3D pane.
- `capacity_hint_json` becomes deprecated. Removing it needs `fmgr.v2`.
- Adding a freezer model is a code change: a catalogue entry, a validation test and, optionally, a
  hand-built renderer.
