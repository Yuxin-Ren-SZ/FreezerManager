// SPDX-License-Identifier: AGPL-3.0-or-later
import { slotBounds, type CabinetSpec, type Extent } from './cabinet';
import {
  boxBlock,
  rotatedBlock,
  type Block,
  type BlockTarget,
  type Material,
  type Vec3,
} from './iso';
import { fillLevel, rackAt, type RackModel, type RenderModel } from './sceneModel';

/**
 * Cabinet + scene + how far each moving part is open → the blocks to paint.
 *
 * Layers (painted low to high, in occlusion order within each; see `paint`):
 *
 * | Layer | What |
 * |---|---|
 * | `INSIDE` | everything behind the face frame: liner, shelves, racks in their slots, closed inner doors, the part of a moving rack still in |
 * | `SHELL` | the outer shell, drawn from outside only, its face frame and the base |
 * | `FRONT` | everything in front of the face frame: the door, an open inner door, the part of a pulled rack that is out |
 *
 * The layers are exact, not a hint. The outer shell is a box round everything
 * inside, so each of its outside faces covers whatever is behind it; and
 * nothing in front of the face frame (y < `doorThickness`) can be hidden by the
 * shell, because the camera never looks from behind the freezer. A rack on its
 * way out is cut at the face frame so each part lands in its own layer.
 *
 * Within a layer the engine orders blocks by the planes that separate them,
 * so parts must not pass through each other: the doors swing on hinges at
 * their inner edge, clear of the opening, and a pulled rack's plates and boxes
 * fit together without overlapping. Nothing behind a closed door is emitted
 * at all.
 */

export interface SceneFrame {
  /** 0 = shut, 1 = fully open. */
  readonly door: number;
  readonly innerDoor: number | null;
  /** How far `innerDoor` is open, 0–1. */
  readonly inner: number;
  readonly pulled: { readonly level: number; readonly column: number } | null;
  /** How far `pulled` is out, 0–1. */
  readonly pull: number;
}

export const CLOSED_FRAME: SceneFrame = {
  door: 0,
  innerDoor: null,
  inner: 0,
  pulled: null,
  pull: 0,
};

const DOOR_SWING = 100;
const LINER = 6;
/** Thickness of an inner door, and how far in front of the interior it sits. */
const INNER_DOOR = 10;
const INNER_GAP = 4;
/** Thickness of a pulled rack's floor, back and side plates. */
const PLATE = 8;

export const INSIDE = 0;
export const SHELL = 1;
export const FRONT = 2;

const box = (
  id: string,
  extent: Extent,
  material: Material,
  options: Parameters<typeof boxBlock>[4] = {},
) =>
  boxBlock(
    id,
    [extent.x0, extent.y0, extent.z0],
    [extent.x0 + extent.w, extent.y0 + extent.d, extent.z0 + extent.h],
    material,
    options,
  );

/** How far a fully pulled rack travels towards the viewer: until it clears the face frame. */
function pullDistance(cabinet: CabinetSpec, slot: Extent): number {
  return slot.y0 + slot.d - cabinet.doorThickness;
}

function shell(cabinet: CabinetSpec): Block[] {
  const { exterior: e, plinth, doorThickness: dt, interior: i } = cabinet;
  const blocks: Block[] = [
    // The carcass behind the door, seen from outside.
    boxBlock('shell', [0, dt, plinth], [e.w, e.d, e.h], 'body', {
      layer: SHELL,
      only: ['left', 'right', 'top', 'back'],
    }),
    // The face frame round the opening, visible when the door is open.
    boxBlock('frame-left', [0, dt, plinth], [i.x0, dt + 20, e.h], 'body', {
      layer: SHELL,
      only: ['front'],
    }),
    boxBlock('frame-right', [i.x0 + i.w, dt, plinth], [e.w, dt + 20, e.h], 'body', {
      layer: SHELL,
      only: ['front'],
    }),
    boxBlock('frame-top', [i.x0, dt, i.z0 + i.h], [i.x0 + i.w, dt + 20, e.h], 'body', {
      layer: SHELL,
      only: ['front'],
    }),
    boxBlock('frame-sill', [i.x0, dt, plinth], [i.x0 + i.w, dt + 20, i.z0], 'body', {
      layer: SHELL,
      only: ['front'],
    }),
    // The machine base: slightly set back, with a grille or a hinged door.
    boxBlock('base', [10, dt + 10, 0], [e.w - 10, e.d - 10, plinth], 'plinth', {
      layer: SHELL,
      only: ['front', 'left', 'right'],
    }),
  ];
  const panel =
    cabinet.base === 'grille'
      ? { x0: e.w * 0.2, x1: e.w * 0.8, z0: plinth * 0.2, z1: plinth * 0.7 }
      : { x0: 40, x1: e.w - 40, z0: plinth * 0.12, z1: plinth * 0.88 };
  blocks.push(
    boxBlock('base-front', [panel.x0, dt + 4, panel.z0], [panel.x1, dt + 10, panel.z1], 'grille', {
      layer: SHELL,
    }),
  );
  return blocks;
}

function liner(cabinet: CabinetSpec): Block[] {
  const { x0, y0, z0, w, d, h } = cabinet.interior;
  const blocks: Block[] = [
    box('liner-back', { x0, y0: y0 + d - LINER, z0, w, d: LINER, h }, 'interior', {
      layer: INSIDE,
    }),
    box('liner-floor', { x0, y0, z0: z0 - LINER, w, d, h: LINER }, 'interior', { layer: INSIDE }),
    box('liner-roof', { x0, y0, z0: z0 + h, w, d, h: LINER }, 'interior', { layer: INSIDE }),
    box('liner-left', { x0: x0 - LINER, y0, z0, w: LINER, d, h }, 'interior', { layer: INSIDE }),
    box('liner-right', { x0: x0 + w, y0, z0, w: LINER, d, h }, 'interior', { layer: INSIDE }),
  ];
  // A shelf under every compartment but the bottom one.
  for (const compartment of cabinet.compartments) {
    if (compartment.level === 0) continue;
    blocks.push(
      box(
        `shelf-${String(compartment.level)}`,
        { x0, y0, z0: compartment.z0 - 12, w, d: d - LINER, h: 12 },
        'shelf',
        { layer: INSIDE },
      ),
    );
  }
  return blocks;
}

function door(cabinet: CabinetSpec, open: number): Block[] {
  const { exterior: e, plinth, doorThickness: dt, panel } = cabinet;
  const right = cabinet.hinge === 'right';
  // The hinge is on the door's inner edge, so the open door stands beside the
  // cabinet's wall instead of across the opening. Offsets below are from the
  // hinge: the door's outer face is at y = -dt, its inner face at y = 0.
  const pivot: Vec3 = [right ? e.w : 0, dt, plinth];
  const angle = (right ? 1 : -1) * DOOR_SWING * open;
  const height = e.h - plinth;
  const across = right ? -e.w : e.w;
  const target: BlockTarget = { kind: 'door' };
  // Door fittings, in the door's own unturned frame: x runs across the door
  // from its left edge (`left`), so every fitting has a positive width.
  const left = right ? -e.w : 0;
  const part = (id: string, offset: Vec3, size: Vec3, material: Material): Block =>
    rotatedBlock(id, pivot, [size[0], 0, 0], [0, size[1], 0], [0, 0, size[2]], angle, material, {
      layer: FRONT,
      target,
      originOffset: offset,
    });
  // The handle sits by the free edge; the panel high on the handle side.
  const handleX = right ? left + 30 : left + e.w - 66;
  const panelX = right ? left + 70 : left + e.w - 70 - panel.width;
  return [
    rotatedBlock('door', pivot, [across, 0, 0], [0, dt, 0], [0, 0, height], angle, 'door', {
      layer: FRONT,
      target,
      originOffset: [0, -dt, 0],
    }),
    part('door-gasket', [left + 8, 0, 20], [e.w - 16, 6, height - 40], 'gasket'),
    part('door-handle', [handleX, -dt - 45, height * 0.42], [36, 45, 320], 'handle'),
    part(
      'door-panel',
      [panelX, -dt - 6, height * (1 - panel.fromTop) - panel.height],
      [panel.width, 6, panel.height],
      'panel',
    ),
  ];
}

function innerDoors(cabinet: CabinetSpec, frame: SceneFrame): Block[] {
  const { interior: i } = cabinet;
  const right = cabinet.hinge === 'right';
  return cabinet.innerDoors.flatMap((range, index) => {
    const first = cabinet.compartments.find(
      (compartment) => compartment.level === range.firstLevel,
    );
    const last = cabinet.compartments.find((compartment) => compartment.level === range.lastLevel);
    if (first === undefined || last === undefined) return [];
    const z0 = first.z0;
    const h = last.z0 + last.h - z0;
    const target: BlockTarget = { kind: 'innerDoor', index };
    const open = frame.innerDoor === index ? frame.inner : 0;
    const front = i.y0 - INNER_GAP - INNER_DOOR;
    if (open <= 0) {
      return [
        box(
          `inner-${String(index)}`,
          { x0: i.x0, y0: front, z0, w: i.w, d: INNER_DOOR, h },
          'innerDoor',
          { layer: INSIDE, target },
        ),
      ];
    }
    // Hinged on its inner edge like the outer door, so the open door's
    // thickness turns away from the opening and the rack paths.
    const pivot: Vec3 = [right ? i.x0 + i.w : i.x0, front + INNER_DOOR, z0];
    return [
      rotatedBlock(
        `inner-${String(index)}`,
        pivot,
        [right ? -i.w : i.w, 0, 0],
        [0, INNER_DOOR, 0],
        [0, 0, h],
        (right ? 1 : -1) * DOOR_SWING * open,
        'innerDoor',
        { layer: FRONT, target, originOffset: [0, -INNER_DOOR, 0] },
      ),
    ];
  });
}

/** True when nothing closed stands between the viewer and compartment `level`. */
function compartmentVisible(cabinet: CabinetSpec, frame: SceneFrame, level: number): boolean {
  if (frame.door <= 0) return false;
  const index = cabinet.innerDoors.findIndex(
    (range) => range.firstLevel <= level && level <= range.lastLevel,
  );
  return index === -1 || (frame.innerDoor === index && frame.inner > 0);
}

/**
 * An extent cut at the face frame (y = `doorThickness`): the part behind it
 * belongs under the shell, the part in front over it. Either may be missing.
 */
function cutAtFaceFrame(
  cabinet: CabinetSpec,
  id: string,
  extent: Extent,
  material: Material,
  target: BlockTarget | undefined,
): Block[] {
  const face = cabinet.doorThickness;
  const blocks: Block[] = [];
  if (extent.y0 < face) {
    const d = Math.min(extent.d, face - extent.y0);
    blocks.push(box(`${id}:out`, { ...extent, d }, material, { layer: FRONT, target }));
  }
  if (extent.y0 + extent.d > face) {
    const y0 = Math.max(extent.y0, face);
    blocks.push(
      box(`${id}:in`, { ...extent, y0, d: extent.y0 + extent.d - y0 }, material, {
        layer: INSIDE,
        target,
      }),
    );
  }
  return blocks;
}

function rackBlocks(cabinet: CabinetSpec, rack: RackModel, slot: Extent, pull: number): Block[] {
  const target: BlockTarget = { kind: 'rack', level: rack.level, column: rack.column };
  if (pull <= 0) {
    return [box(`rack-${rack.id}`, slot, 'rack', { layer: INSIDE, target })];
  }
  const moved: Extent = { ...slot, y0: slot.y0 - pullDistance(cabinet, slot) * pull };
  // A pulled rack is drawn open: floor, back and one side plate, so its boxes
  // show. The side plate is on the hinge side, away from the home camera. The
  // back and side stand on the floor, and the side stops at the back, so no
  // two plates overlap.
  const sideOnLeft = cabinet.hinge === 'left';
  const above = { z0: moved.z0 + PLATE, h: moved.h - PLATE };
  const plates: [string, Extent][] = [
    ['floor', { ...moved, h: PLATE }],
    ['back', { ...moved, ...above, y0: moved.y0 + moved.d - PLATE, d: PLATE }],
    [
      'side',
      {
        ...moved,
        ...above,
        x0: sideOnLeft ? moved.x0 : moved.x0 + moved.w - PLATE,
        w: PLATE,
        d: moved.d - PLATE,
      },
    ],
  ];
  const blocks: Block[] = plates.flatMap(([name, extent]) =>
    cutAtFaceFrame(cabinet, `rack-${rack.id}-${name}`, extent, 'rackActive', target),
  );

  // The cells fill the space the plates leave.
  const grid = rack.grid ?? { levels: 1, columns: 1, depths: 1 };
  const space: Extent = {
    x0: sideOnLeft ? moved.x0 + PLATE : moved.x0,
    y0: moved.y0,
    z0: above.z0,
    w: moved.w - PLATE,
    d: moved.d - PLATE,
    h: above.h,
  };
  const cellW = space.w / grid.columns;
  const cellD = space.d / grid.depths;
  const cellH = space.h / grid.levels;
  for (const cell of rack.cells) {
    if (cell.level >= grid.levels || cell.column >= grid.columns || cell.depth >= grid.depths) {
      continue;
    }
    const fill = fillLevel(cell.occupied, cell.total);
    const material: Material =
      cell.kind === 'container'
        ? 'drawer'
        : fill === 'full'
          ? 'boxFull'
          : fill === 'partial'
            ? 'boxPartial'
            : 'boxEmpty';
    const inset = 0.08;
    const cellTarget: BlockTarget | undefined =
      cell.kind === 'box' && cell.boxes.length === 1
        ? { kind: 'box', id: cell.boxes[0].id }
        : target;
    blocks.push(
      ...cutAtFaceFrame(
        cabinet,
        `cell-${cell.id}`,
        {
          x0: space.x0 + cell.column * cellW + cellW * inset,
          y0: space.y0 + cell.depth * cellD + cellD * inset,
          z0: space.z0 + cell.level * cellH + 2,
          w: cellW * (1 - 2 * inset),
          d: cellD * (1 - 2 * inset),
          h: cellH * 0.82,
        },
        material,
        cellTarget,
      ),
    );
  }
  return blocks;
}

export function buildBlocks(cabinet: CabinetSpec, scene: RenderModel, frame: SceneFrame): Block[] {
  const blocks: Block[] = [...shell(cabinet), ...door(cabinet, frame.door)];
  if (frame.door <= 0) {
    return blocks;
  }
  blocks.push(...liner(cabinet), ...innerDoors(cabinet, frame));
  for (const compartment of cabinet.compartments) {
    if (!compartmentVisible(cabinet, frame, compartment.level)) continue;
    for (let column = 0; column < compartment.slots; column += 1) {
      const rack = rackAt(scene, compartment.level, column);
      const slot = slotBounds(cabinet, compartment.level, column);
      if (rack === undefined || slot === null) continue;
      const pulled =
        frame.pulled !== null &&
        frame.pulled.level === compartment.level &&
        frame.pulled.column === column;
      blocks.push(...rackBlocks(cabinet, rack, slot, pulled ? frame.pull : 0));
    }
  }
  return blocks;
}

/**
 * Every point the drawing can reach — the shell, the door's swing, a fully
 * pulled rack — so the SVG view box can be fixed per cabinet and camera and
 * does not jump while parts move.
 */
export function sceneEnvelope(cabinet: CabinetSpec): Vec3[] {
  const { exterior: e, interior: i, doorThickness: dt } = cabinet;
  const swing = e.w * 1.05;
  // The open door stands beside the hinge-side wall: its swing past 90°, its
  // thickness and the handle.
  const beside = swing * 0.2 + dt + 60;
  const x0 = cabinet.hinge === 'left' ? -beside : -20;
  const x1 = cabinet.hinge === 'right' ? e.w + beside : e.w + 20;
  // No slot is deeper than the interior, so no rack pulls out further.
  const y0 = -Math.max(swing, i.y0 + i.d - dt + 40);
  const corners: Vec3[] = [];
  for (const x of [x0, x1]) {
    for (const y of [y0, e.d]) {
      for (const z of [0, e.h]) {
        corners.push([x, y, z]);
      }
    }
  }
  return corners;
}
