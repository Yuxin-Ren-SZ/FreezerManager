# ADR 0002: A freezer scene API for the 3D layout view

- Status: Proposed. It needs lead/owner review before the implementation slices are filed.
- Date: 2026-10-27.
- Design issue: [#215](https://github.com/Yuxin-Ren-SZ/FreezerManager/issues/215).
- Demo: [`doc/design/layout-3d/cryocube-f740.html`](../design/layout-3d/cryocube-f740.html).
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

All changes are additive in `fmgr.v1`. They are written in proto syntax here;
field numbers continue the existing messages.

### 1. A freezer model catalogue

```proto
message FreezerModel {
  string id = 1;
  string lab_id = 2;                     // "" = built-in entry, seeded by migration
  string manufacturer = 3;               // "Eppendorf"
  string model_code = 4;                 // "F740hi"
  string display_name = 5;               // "CryoCube F740hi"
  FreezerForm form = 6;                  // UPRIGHT | CHEST
  Dimensions exterior_mm = 7;            // 1099 x 980 x 1973
  Dimensions interior_mm = 8;            //  865 x 622 x 1390
  HingeSide hinge = 9;                   // as seen from the front
  bool inner_doors = 10;                 // one insulated inner door per compartment
  repeated CompartmentSpec compartments = 11;   // bottom -> top
  string render_key = 12;                // hand-built renderer, "" = generic cabinet
  fmgr.v1.common.Timestamp created_at = 13;
  optional fmgr.v1.common.Timestamp archived_at = 14;
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
```

`Freezer` gains `optional string freezer_model_id = 10;`, which `CreateFreezer` and `UpdateFreezer` also accept.

The seeded **CryoCube F740hi** entry:
- 3 compartments, made by 2 shelves plus the floor, about 457 mm each.
- 3 inner doors, hinged on the right as seen from the front, the same side as the outer door.
- 6 rack slots per compartment, each about 139 x 560 x 430 mm.
- 18 side-access racks of 8 tiers x 4 deep, which is 576 two-inch boxes.

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
  - `grid` replaces `capacity_hint_json`. That field stays readable and is marked deprecated.
- `Box` gains `optional Placement placement = 10;`.
- The Create/Update requests for both accept the new fields.

Validation lives in `src/core` and stays pure:
- A placement must fall inside the parent's grid. For a container directly under the layout root, the grid comes from the model's compartments and their `rack_slots`.
- Two non-archived siblings can't share a placement.
- A placement isn't allowed when the parent has no grid.

Unplaced containers and boxes stay legal, so no existing lab needs a backfill. The 3D view lists them under "Unplaced".

### 3. One read RPC for the scene

```proto
rpc GetFreezerScene(GetFreezerSceneRequest) returns (GetFreezerSceneResponse);
message GetFreezerSceneRequest { string freezer_id = 1; }
message GetFreezerSceneResponse {
  Freezer freezer = 1;
  optional FreezerModel model = 2;       // absent -> generic cabinet
  repeated SceneNode nodes = 3;          // non-archived, under layout_root_id, parents first
}
enum SceneNodeKind { SCENE_NODE_KIND_UNSPECIFIED = 0; SCENE_NODE_KIND_CONTAINER = 1; SCENE_NODE_KIND_BOX = 2; }
message SceneNode {
  string id = 1;
  optional string parent_id = 2;
  SceneNodeKind kind = 3;
  ContainerKind container_kind = 4;      // containers only
  string label = 5;
  optional Placement placement = 6;
  optional GridSpec grid = 7;
  int32 occupied_positions = 8;          // boxes: counts only
  int32 total_positions = 9;
}
```

- **Authorization.** This is an entity-id read, so it follows `GetFreezer`, which is registered with `rpc::RpcGate::token_and_mfa()` in `src/server/BoxServiceImpl.cc`.
  - The handler asks the gate for that rule, then checks `sample.read` manually on the freezer's lab.
  - The registration carries a comment saying so (AGENTS.md §5, #119).
  - `RpcRegistryHoldsExactlyTheServedRpcs` covers the new entry.
- **No PHI.** Nodes carry only labels, geometry and position counts. No sample field reaches the scene.
- **Audit.** `GetFreezerScene` and `ListFreezerModels` are read-only. `CreateFreezerModel` and `ArchiveFreezerModel` require `freezer.configure`, and each writes its audit row in the same transaction.
- **REST:**
  - `GET /api/v1/freezers/{freezer_id}/scene`
  - `GET /api/v1/freezer-models`
  - `POST /api/v1/labs/{lab_id}/freezer-models`
- **Freshness.** The client refetches the scene on the existing sample-watch events. A scene stream is out of scope.

### 4. Storage

There is one migration, with the same version in both backends (`lock:migration`):

- New tables `freezer_models` and `freezer_model_compartments`, with typed columns and no JSON.
- `freezers.freezer_model_id`.
- `placement_level/column/depth` and `grid_levels/columns/depths` on `storage_containers`.
- `placement_level/column/depth` on `boxes`.
- A partial unique index on (parent, placement) where `archived_at IS NULL`.
- The seed row for the CryoCube F740hi.

The backends must pass `tests/backend_conformance/`.

### 5. Web rendering

- The renderer is a zero-dependency SVG isometric engine in the Layout route's lazy chunk, in the style of the demo.
  - It needs no `lock:deps`, and it leaves the 250 KiB initial-bundle budget alone.
- `render_key` picks a hand-built model, such as `cryocube-f740`. Otherwise the client builds a generic cabinet from `FreezerModel` dimensions and compartments.
- The tree stays on the page as the accessible text alternative.
- Interaction path:
  1. Open the door.
  2. Pick a compartment (its inner door opens).
  3. Pull a rack out.
  4. Click a box to open `/labs/:labId/boxes/:boxId` (G3.4).

## Alternatives considered

- **Keep `capacity_hint_json` and the four list calls.** This needs no proto change, but the geometry stays untyped and every client re-implements the parent walk. Rejected.
- **Geometry inside `Freezer` itself.** Every F740 in every lab would repeat the same dimensions, and correcting them would mean editing each row. Rejected in favour of a shared catalogue.
- **WebGL / three.js.** It gives real lighting, but adds about 150 KiB gzipped, needs `lock:deps` and a GPU in kiosk browsers. Deferred. The scene API is renderer-neutral, so the decision can be revisited.

## Consequences

- Four implementation slices:
  1. Proto, migration and core validation (`lock:proto` + `lock:migration`).
  2. Server RPCs, registry, audit and REST.
  3. Web 3D view.
  4. Admin placement editing, extending G3.8.
- The Qt client can consume the same scene for its planned QML 3D pane.
- `capacity_hint_json` becomes deprecated. Removing it needs `fmgr.v2`.
