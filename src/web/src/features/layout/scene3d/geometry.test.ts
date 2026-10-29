// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import {
  ContainerKind,
  GetFreezerSceneResponseSchema,
  SceneNodeKind,
} from '../../../gen/fmgr/v1/box_pb';
import { F740_MODEL, TSX_MODEL } from '../../../test/freezerModels';
import { cabinetFor, slotBounds } from './cabinet';
import { buildBlocks, CLOSED_FRAME, sceneEnvelope, type SceneFrame } from './geometry';
import { blockFaces, paint, type Block, type Vec3 } from './iso';
import { buildRenderModel } from './sceneModel';

/**
 * Cabinet + scene + how far each part is open → the blocks the engine paints.
 * What is clickable, what is hidden behind a closed door, and where a pulled
 * rack's boxes end up.
 */

const f740Scene = buildRenderModel(
  create(GetFreezerSceneResponseSchema, {
    freezer: { id: 'fz', name: 'F740', layoutRootId: 'root', freezerModelId: F740_MODEL.id },
    model: F740_MODEL,
    nodes: [
      {
        id: 'root',
        kind: SceneNodeKind.CONTAINER,
        containerKind: ContainerKind.GENERIC,
        label: 'Root',
      },
      {
        id: 'rack-4',
        parentId: 'root',
        kind: SceneNodeKind.CONTAINER,
        containerKind: ContainerKind.RACK,
        label: 'Rack 4',
        placement: { level: 1, column: 3, depth: 0 },
        grid: { levels: 8, columns: 1, depths: 4 },
      },
      {
        id: 'box-full',
        parentId: 'rack-4',
        kind: SceneNodeKind.BOX,
        label: 'Full box',
        placement: { level: 0, column: 0, depth: 0 },
        occupiedPositions: 81,
        totalPositions: 81,
      },
      {
        id: 'box-empty',
        parentId: 'rack-4',
        kind: SceneNodeKind.BOX,
        label: 'Empty box',
        placement: { level: 7, column: 0, depth: 3 },
        occupiedPositions: 0,
        totalPositions: 81,
      },
    ],
  }),
);
const f740 = cabinetFor(F740_MODEL);

const targets = (blocks: readonly Block[], kind: string) =>
  blocks.filter((block) => block.target?.kind === kind);

const frame = (overrides: Partial<SceneFrame>): SceneFrame => ({ ...CLOSED_FRAME, ...overrides });

describe('buildBlocks', () => {
  it('draws a closed freezer as its shell and a clickable door, nothing inside', () => {
    const blocks = buildBlocks(f740, f740Scene, CLOSED_FRAME);
    expect(targets(blocks, 'door')).not.toHaveLength(0);
    expect(targets(blocks, 'rack')).toHaveLength(0);
    expect(targets(blocks, 'box')).toHaveLength(0);
  });

  it('shows the inner doors, not the racks, once only the outer door is open', () => {
    const blocks = buildBlocks(f740, f740Scene, frame({ door: 1 }));
    const doors = new Set(
      targets(blocks, 'innerDoor').map((block) =>
        block.target?.kind === 'innerDoor' ? block.target.index : -1,
      ),
    );
    expect([...doors].sort()).toEqual([0, 1, 2]);
    expect(targets(blocks, 'rack')).toHaveLength(0);
  });

  it('shows the racks of the compartment whose inner door is open', () => {
    const blocks = buildBlocks(f740, f740Scene, frame({ door: 1, innerDoor: 1, inner: 1 }));
    const racks = targets(blocks, 'rack');
    expect(racks.length).toBeGreaterThan(0);
    expect(racks.every((block) => block.target?.kind === 'rack' && block.target.level === 1)).toBe(
      true,
    );
  });

  it('slides a pulled rack out of the cabinet and shows its boxes, coloured by fill', () => {
    const inside = buildBlocks(f740, f740Scene, frame({ door: 1, innerDoor: 1, inner: 1 }));
    const pulled = buildBlocks(
      f740,
      f740Scene,
      frame({ door: 1, innerDoor: 1, inner: 1, pulled: { level: 1, column: 3 }, pull: 1 }),
    );
    const rackFront = (blocks: readonly Block[]) =>
      Math.min(...targets(blocks, 'rack').map((block) => block.origin[1]));
    expect(rackFront(pulled)).toBeLessThan(rackFront(inside));

    const boxes = targets(pulled, 'box');
    expect(boxes.map((block) => block.target)).toEqual(
      expect.arrayContaining([
        { kind: 'box', id: 'box-full' },
        { kind: 'box', id: 'box-empty' },
      ]),
    );
    const material = (id: string) =>
      boxes.find((block) => block.target?.kind === 'box' && block.target.id === id)?.material;
    expect(material('box-full')).toBe('boxFull');
    expect(material('box-empty')).toBe('boxEmpty');
  });

  it('hangs the door on the hinge side: an open right-hinged door swings out to the right', () => {
    const open = buildBlocks(f740, f740Scene, frame({ door: 1 }));
    const door = targets(open, 'door').find((block) => block.material === 'door');
    expect(door).toBeDefined();
    const faces = door === undefined ? [] : blockFaces(door, { yaw: 0, pitch: 20 });
    const xs = faces.flatMap((face) => face.points.map((point) => point.x));
    expect(Math.max(...xs)).toBeGreaterThan(f740.exterior.w);
  });

  it('draws a TSX inner door over both of its levels', () => {
    const tsx = cabinetFor(TSX_MODEL);
    const empty = buildRenderModel(create(GetFreezerSceneResponseSchema, { model: TSX_MODEL }));
    const blocks = buildBlocks(tsx, empty, frame({ door: 1 }));
    const lower = targets(blocks, 'innerDoor').find(
      (block) => block.target?.kind === 'innerDoor' && block.target.index === 0,
    );
    expect(lower).toBeDefined();
    const height = lower === undefined ? 0 : lower.w[2];
    const twoLevels = tsx.compartments[1].z0 + tsx.compartments[1].h - tsx.compartments[0].z0;
    expect(height).toBeGreaterThan(twoLevels * 0.9);
  });
});

/**
 * Where the parts stand, so that a paint order exists at all: parts must not
 * pass through each other, and the layers must match what is inside the shell
 * and what is in front of it.
 */
describe('buildBlocks geometry', () => {
  const tsx = cabinetFor(TSX_MODEL);
  const tsxScene = buildRenderModel(
    create(GetFreezerSceneResponseSchema, {
      freezer: { id: 'fz', name: 'TSX', layoutRootId: 'root', freezerModelId: TSX_MODEL.id },
      model: TSX_MODEL,
      nodes: [
        {
          id: 'root',
          kind: SceneNodeKind.CONTAINER,
          containerKind: ContainerKind.GENERIC,
          label: 'Root',
        },
        {
          id: 'rack-t',
          parentId: 'root',
          kind: SceneNodeKind.CONTAINER,
          containerKind: ContainerKind.RACK,
          label: 'Rack',
          placement: { level: 2, column: 3, depth: 0 },
          grid: { levels: 5, columns: 1, depths: 3 },
        },
        {
          id: 'box-t',
          parentId: 'rack-t',
          kind: SceneNodeKind.BOX,
          label: 'Box',
          placement: { level: 0, column: 0, depth: 0 },
          occupiedPositions: 1,
          totalPositions: 81,
        },
      ],
    }),
  );
  const cases = [
    { name: 'F740', cabinet: f740, scene: f740Scene, level: 1, column: 3, innerDoor: 1 },
    { name: 'TSX', cabinet: tsx, scene: tsxScene, level: 2, column: 3, innerDoor: 1 },
  ];

  const corners = (block: Block): Vec3[] => {
    const out: Vec3[] = [];
    for (const a of [0, 1])
      for (const b of [0, 1])
        for (const c of [0, 1])
          out.push(
            [0, 1, 2].map(
              (axis) =>
                block.origin[axis] + a * block.u[axis] + b * block.v[axis] + c * block.w[axis],
            ) as unknown as Vec3,
          );
    return out;
  };
  const along = (axis: 0 | 1 | 2, block: Block) => corners(block).map((corner) => corner[axis]);
  const dot = (a: Vec3, b: Vec3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const isAxisAligned = (block: Block) =>
    block.u[1] === 0 && block.u[2] === 0 && block.v[0] === 0 && block.v[2] === 0;
  const byId = (blocks: readonly Block[], id: string) => {
    const block = blocks.find((candidate) => candidate.id === id);
    if (block === undefined) throw new Error(`no block ${id}`);
    return block;
  };

  it.each(cases)('stands the open $name door clear of the opening, beside its wall', (c) => {
    const blocks = buildBlocks(c.cabinet, c.scene, frame({ door: 1 }));
    for (const block of targets(blocks, 'door')) {
      expect(Math.min(...along(0, block))).toBeGreaterThanOrEqual(
        c.cabinet.interior.x0 + c.cabinet.interior.w,
      );
    }
  });

  it.each(cases)(
    'swings the open $name inner door clear of the racks and of the open door',
    (c) => {
      const blocks = buildBlocks(
        c.cabinet,
        c.scene,
        frame({ door: 1, innerDoor: c.innerDoor, inner: 1 }),
      );
      const inner = byId(blocks, `inner-${String(c.innerDoor)}`);
      // The rack paths: every slot of every level the inner door covers.
      const range = c.cabinet.innerDoors[c.innerDoor];
      const slots = c.cabinet.compartments
        .filter((level) => range.firstLevel <= level.level && level.level <= range.lastLevel)
        .flatMap((level) =>
          Array.from({ length: level.slots }, (_, column) =>
            slotBounds(c.cabinet, level.level, column),
          ),
        )
        .filter((slot) => slot !== null);
      expect(slots.length).toBeGreaterThan(0);
      const rackEdge = Math.max(...slots.map((slot) => slot.x0 + slot.w));
      expect(Math.min(...along(0, inner))).toBeGreaterThanOrEqual(rackEdge);

      // Every corner of the inner door is on the cabinet side of the open
      // door's gasket, the door's innermost surface.
      const gasket = byId(blocks, 'door-gasket');
      const size = Math.hypot(...gasket.v);
      const inward: Vec3 = [gasket.v[0] / size, gasket.v[1] / size, gasket.v[2] / size];
      const surface: Vec3 = [
        gasket.origin[0] + gasket.v[0],
        gasket.origin[1] + gasket.v[1],
        gasket.origin[2] + gasket.v[2],
      ];
      for (const corner of corners(inner)) {
        const offset: Vec3 = [
          corner[0] - surface[0],
          corner[1] - surface[1],
          corner[2] - surface[2],
        ];
        expect(dot(offset, inward)).toBeGreaterThan(0);
      }
    },
  );

  it.each(cases)(
    'splits a $name rack being pulled at the face frame: under the shell inside, over it outside',
    (c) => {
      const blocks = buildBlocks(
        c.cabinet,
        c.scene,
        frame({
          door: 1,
          innerDoor: c.innerDoor,
          inner: 1,
          pulled: { level: c.level, column: c.column },
          pull: 0.5,
        }),
      );
      const shellLayer = byId(blocks, 'shell').layer;
      const face = c.cabinet.doorThickness;
      const moving = blocks.filter(
        (block) =>
          block.material === 'rackActive' ||
          block.target?.kind === 'box' ||
          block.id.startsWith('cell-'),
      );
      const inside = moving.filter((block) => block.layer < shellLayer);
      const outside = moving.filter((block) => block.layer > shellLayer);
      expect(inside.length).toBeGreaterThan(0);
      expect(outside.length).toBeGreaterThan(0);
      expect(inside.length + outside.length).toBe(moving.length);
      for (const block of inside) expect(Math.min(...along(1, block))).toBeGreaterThanOrEqual(face);
      for (const block of outside) expect(Math.max(...along(1, block))).toBeLessThanOrEqual(face);
    },
  );

  it.each(cases)('pulls a $name rack all the way out of the shell', (c) => {
    const blocks = buildBlocks(
      c.cabinet,
      c.scene,
      frame({
        door: 1,
        innerDoor: c.innerDoor,
        inner: 1,
        pulled: { level: c.level, column: c.column },
        pull: 1,
      }),
    );
    const plates = blocks.filter((block) => block.material === 'rackActive');
    expect(plates.length).toBeGreaterThan(0);
    for (const block of plates) {
      expect(Math.max(...along(1, block))).toBeLessThanOrEqual(c.cabinet.doorThickness);
    }
  });

  it("puts a pulled rack's side plate on the hinge side, away from the home view", () => {
    const blocks = buildBlocks(
      f740,
      f740Scene,
      frame({ door: 1, innerDoor: 1, inner: 1, pulled: { level: 1, column: 3 }, pull: 1 }),
    );
    const plates = blocks.filter((block) => block.material === 'rackActive');
    const side = byId(blocks, 'rack-rack-4-side:out');
    const centre = (block: Block) => block.origin[0] + block.u[0] / 2;
    const rackCentre =
      (Math.min(...plates.flatMap((block) => along(0, block))) +
        Math.max(...plates.flatMap((block) => along(0, block)))) /
      2;
    expect(f740.hinge).toBe('right');
    expect(centre(side)).toBeGreaterThan(rackCentre);
  });

  it.each(cases)(
    'keeps the axis-aligned parts of an open $name from passing through each other',
    (c) => {
      const blocks = buildBlocks(
        c.cabinet,
        c.scene,
        frame({
          door: 1,
          innerDoor: c.innerDoor,
          inner: 1,
          pulled: { level: c.level, column: c.column },
          pull: 1,
        }),
      );
      const shellLayer = byId(blocks, 'shell').layer;
      const solid = blocks.filter((block) => block.layer !== shellLayer && isAxisAligned(block));
      const range = (block: Block, axis: 0 | 1 | 2) => {
        const values = along(axis, block);
        return [Math.min(...values), Math.max(...values)];
      };
      const clashes: string[] = [];
      for (let i = 0; i < solid.length; i += 1) {
        for (let j = i + 1; j < solid.length; j += 1) {
          const overlap = ([0, 1, 2] as const).every((axis) => {
            const [a0, a1] = range(solid[i], axis);
            const [b0, b1] = range(solid[j], axis);
            return Math.min(a1, b1) - Math.max(a0, b0) > 0.5;
          });
          if (overlap) clashes.push(`${solid[i].id} × ${solid[j].id}`);
        }
      }
      expect(clashes).toEqual([]);
    },
  );
});

describe('painting a full TSX', () => {
  // Every slot filled, the upper inner door (levels 2 and 3) open: two levels
  // of racks with a shelf between them, all in one layer.
  const tsx = cabinetFor(TSX_MODEL);
  const full = buildRenderModel(
    create(GetFreezerSceneResponseSchema, {
      freezer: { id: 'fz', name: 'TSX', layoutRootId: 'root', freezerModelId: TSX_MODEL.id },
      model: TSX_MODEL,
      nodes: [
        {
          id: 'root',
          kind: SceneNodeKind.CONTAINER,
          containerKind: ContainerKind.GENERIC,
          label: 'Root',
        },
        ...[0, 1, 2, 3].flatMap((level) =>
          [0, 1, 2, 3].map((column) => ({
            id: `r${String(level)}${String(column)}`,
            parentId: 'root',
            kind: SceneNodeKind.CONTAINER,
            containerKind: ContainerKind.RACK,
            label: 'Rack',
            placement: { level, column, depth: 0 },
          })),
        ),
      ],
    }),
  );

  it.each([
    { yaw: -28, pitch: 24 },
    { yaw: -60, pitch: 50 },
    { yaw: 15, pitch: 10 },
  ])(
    'paints each rack between the shelf under it and the shelf over it (yaw $yaw, pitch $pitch)',
    (camera) => {
      const order = paint(
        buildBlocks(tsx, full, frame({ door: 1, innerDoor: 1, inner: 1 })),
        camera,
      )
        .map((face) => face.blockId)
        .filter((id, index, ids) => ids.indexOf(id) === index);
      const at = (id: string) => order.indexOf(id);
      for (const level of [2, 3]) {
        for (const column of [0, 1, 2, 3]) {
          const rack = at(`rack-r${String(level)}${String(column)}`);
          expect(rack).toBeGreaterThan(at(`shelf-${String(level)}`));
          if (level < 3) expect(rack).toBeLessThan(at(`shelf-${String(level + 1)}`));
        }
      }
    },
  );
});

describe('sceneEnvelope', () => {
  it('covers the open door and a pulled rack, so the view does not jump while they move', () => {
    const corners = sceneEnvelope(f740);
    const xs = corners.map((corner) => corner[0]);
    const ys = corners.map((corner) => corner[1]);
    expect(Math.max(...xs)).toBeGreaterThan(f740.exterior.w);
    expect(Math.min(...ys)).toBeLessThan(-f740.interior.d * 0.5);
  });
});
