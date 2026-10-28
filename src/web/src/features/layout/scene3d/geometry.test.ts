// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import {
  ContainerKind,
  GetFreezerSceneResponseSchema,
  SceneNodeKind,
} from '../../../gen/fmgr/v1/box_pb';
import { F740_MODEL, TSX_MODEL } from '../../../test/freezerModels';
import { cabinetFor } from './cabinet';
import { buildBlocks, CLOSED_FRAME, sceneEnvelope, type SceneFrame } from './geometry';
import { blockFaces, type Block } from './iso';
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

describe('sceneEnvelope', () => {
  it('covers the open door and a pulled rack, so the view does not jump while they move', () => {
    const corners = sceneEnvelope(f740);
    const xs = corners.map((corner) => corner[0]);
    const ys = corners.map((corner) => corner[1]);
    expect(Math.max(...xs)).toBeGreaterThan(f740.exterior.w);
    expect(Math.min(...ys)).toBeLessThan(-f740.interior.d * 0.5);
  });
});
