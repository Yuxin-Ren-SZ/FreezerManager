// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import {
  ContainerKind,
  GetFreezerSceneResponseSchema,
  SceneNodeKind,
  type GetFreezerSceneResponse,
} from '../../../gen/fmgr/v1/box_pb';
import { F740_MODEL, TSX_MODEL } from '../../../test/freezerModels';
import { buildRenderModel, fillLevel } from './sceneModel';

/**
 * `freezer/scene` → what the 3D view draws (ADR 0003 §3, §5): racks in their
 * cabinet slots, the boxes in each rack's grid, occupancy as counts, and an
 * honest "Unplaced" list for everything without a cell.
 */

interface NodeInit {
  id: string;
  parentId?: string;
  box?: boolean;
  label: string;
  placement?: [number, number, number];
  grid?: [number, number, number];
  occupied?: number;
  total?: number;
}

function scene(model: typeof F740_MODEL | undefined, nodes: NodeInit[]): GetFreezerSceneResponse {
  return create(GetFreezerSceneResponseSchema, {
    freezer: {
      id: 'fz-1',
      name: 'Minus 80 A',
      layoutRootId: 'root',
      freezerModelId: model?.id ?? '',
    },
    model,
    nodes: [
      {
        id: 'root',
        kind: SceneNodeKind.CONTAINER,
        containerKind: ContainerKind.GENERIC,
        label: 'Root',
      },
      ...nodes.map((node) => ({
        id: node.id,
        parentId: node.parentId ?? 'root',
        kind: node.box === true ? SceneNodeKind.BOX : SceneNodeKind.CONTAINER,
        containerKind: node.box === true ? ContainerKind.UNSPECIFIED : ContainerKind.RACK,
        label: node.label,
        placement:
          node.placement === undefined
            ? undefined
            : { level: node.placement[0], column: node.placement[1], depth: node.placement[2] },
        grid:
          node.grid === undefined
            ? undefined
            : { levels: node.grid[0], columns: node.grid[1], depths: node.grid[2] },
        occupiedPositions: node.occupied ?? 0,
        totalPositions: node.total ?? 0,
      })),
    ],
  });
}

describe('buildRenderModel', () => {
  it('puts a placed rack in its compartment slot with its boxes in the rack grid', () => {
    const model = buildRenderModel(
      scene(F740_MODEL, [
        { id: 'rack-4', label: 'Rack 4', placement: [1, 3, 0], grid: [8, 1, 4] },
        {
          id: 'b-top',
          parentId: 'rack-4',
          box: true,
          label: 'Top back',
          placement: [7, 0, 3],
          occupied: 81,
          total: 81,
        },
        {
          id: 'b-low',
          parentId: 'rack-4',
          box: true,
          label: 'Low front',
          placement: [0, 0, 0],
          occupied: 10,
          total: 81,
        },
      ]),
    );

    expect(model.freezerName).toBe('Minus 80 A');
    expect(model.model?.id).toBe(F740_MODEL.id);
    expect(model.racks).toHaveLength(1);
    const [rack] = model.racks;
    expect(rack).toMatchObject({ id: 'rack-4', label: 'Rack 4', level: 1, column: 3 });
    expect(rack.grid).toEqual({ levels: 8, columns: 1, depths: 4 });
    expect(rack.cells.map((cell) => cell.id)).toEqual(['b-low', 'b-top']);
    expect(rack.cells[1]).toMatchObject({ level: 7, column: 0, depth: 3, kind: 'box' });
    expect(rack.occupied).toBe(91);
    expect(rack.total).toBe(162);
    expect(rack.boxCount).toBe(2);
    expect(model.unplaced).toEqual([]);
  });

  it('keeps a box without a cell in its rack, listed as loose', () => {
    const model = buildRenderModel(
      scene(F740_MODEL, [
        { id: 'rack-1', label: 'Rack 1', placement: [0, 0, 0], grid: [8, 1, 4] },
        { id: 'b-loose', parentId: 'rack-1', box: true, label: 'Loose', occupied: 1, total: 81 },
      ]),
    );
    expect(model.racks[0].cells).toEqual([]);
    expect(model.racks[0].loose.map((box) => box.id)).toEqual(['b-loose']);
    expect(model.racks[0].boxCount).toBe(1);
  });

  it('collects the boxes of a drawer held in a drawer rack', () => {
    const model = buildRenderModel(
      scene(TSX_MODEL, [
        { id: 'rack-l4r2', label: 'L4 R2', placement: [3, 1, 0], grid: [5, 1, 1] },
        {
          id: 'drawer-2',
          parentId: 'rack-l4r2',
          label: 'Drawer 2',
          placement: [1, 0, 0],
          grid: [1, 1, 5],
        },
        {
          id: 'b1',
          parentId: 'drawer-2',
          box: true,
          label: 'D2 B1',
          placement: [0, 0, 0],
          occupied: 3,
          total: 81,
        },
        {
          id: 'b2',
          parentId: 'drawer-2',
          box: true,
          label: 'D2 B2',
          placement: [0, 0, 1],
          occupied: 0,
          total: 81,
        },
      ]),
    );
    const [rack] = model.racks;
    expect(rack.cells).toHaveLength(1);
    expect(rack.cells[0]).toMatchObject({ id: 'drawer-2', kind: 'container', level: 1 });
    expect(rack.cells[0].boxes.map((box) => box.id)).toEqual(['b1', 'b2']);
    expect(rack.cells[0].occupied).toBe(3);
    expect(rack.boxCount).toBe(2);
  });

  it('lists everything under a freezer without a model as unplaced', () => {
    const model = buildRenderModel(
      scene(undefined, [
        { id: 'rack-a', label: 'Rack A' },
        { id: 'b-1', parentId: 'rack-a', box: true, label: 'Box 1', occupied: 2, total: 9 },
      ]),
    );
    expect(model.model).toBeNull();
    expect(model.racks).toEqual([]);
    expect(model.unplaced).toEqual([
      { id: 'rack-a', kind: 'container', label: 'Rack A', boxes: 1 },
    ]);
  });

  it('lists a rack whose slot the model does not have as unplaced rather than drawing it', () => {
    const model = buildRenderModel(
      scene(TSX_MODEL, [{ id: 'rack-x', label: 'Rack X', placement: [0, 5, 0] }]),
    );
    expect(model.racks).toEqual([]);
    expect(model.unplaced.map((item) => item.id)).toEqual(['rack-x']);
  });

  it('lists a box standing directly in the cabinet as unplaced', () => {
    const model = buildRenderModel(
      scene(F740_MODEL, [{ id: 'b-floor', box: true, label: 'On the floor' }]),
    );
    expect(model.unplaced).toEqual([
      { id: 'b-floor', kind: 'box', label: 'On the floor', boxes: 1 },
    ]);
  });

  it('survives an empty response', () => {
    const model = buildRenderModel(create(GetFreezerSceneResponseSchema, {}));
    expect(model.racks).toEqual([]);
    expect(model.unplaced).toEqual([]);
  });
});

describe('fillLevel', () => {
  it('names a box empty, partly full or full from its counts', () => {
    expect(fillLevel(0, 81)).toBe('empty');
    expect(fillLevel(40, 81)).toBe('partial');
    expect(fillLevel(81, 81)).toBe('full');
    expect(fillLevel(0, 0)).toBe('empty');
  });
});
