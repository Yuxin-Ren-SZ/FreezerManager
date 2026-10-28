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
 * Layers (painted low to high, depth-sorted within each):
 *
 * | Layer | What |
 * |---|---|
 * | 0 | the interior liner and shelves — always behind what is inside |
 * | 1 | racks standing in their slots |
 * | 2 | closed inner doors |
 * | 3 | the outer shell, drawn from outside only, and the base |
 * | 4 | everything outside the shell: the door, an open inner door, a pulled rack |
 *
 * The outer shell carries `only` faces, so its inner faces never cover the
 * contents; the liner draws the inside surfaces instead. Nothing behind a
 * closed door is emitted at all.
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

/** How far a fully pulled rack travels towards the viewer. */
function pullDistance(cabinet: CabinetSpec): number {
  return cabinet.interior.y0 + cabinet.interior.d * 0.75;
}

function shell(cabinet: CabinetSpec): Block[] {
  const { exterior: e, plinth, doorThickness: dt, interior: i } = cabinet;
  const blocks: Block[] = [
    // The carcass behind the door, seen from outside.
    boxBlock('shell', [0, dt, plinth], [e.w, e.d, e.h], 'body', {
      layer: 3,
      only: ['left', 'right', 'top', 'back'],
    }),
    // The face frame round the opening, visible when the door is open.
    boxBlock('frame-left', [0, dt, plinth], [i.x0, dt + 20, e.h], 'body', {
      layer: 3,
      only: ['front'],
    }),
    boxBlock('frame-right', [i.x0 + i.w, dt, plinth], [e.w, dt + 20, e.h], 'body', {
      layer: 3,
      only: ['front'],
    }),
    boxBlock('frame-top', [i.x0, dt, i.z0 + i.h], [i.x0 + i.w, dt + 20, e.h], 'body', {
      layer: 3,
      only: ['front'],
    }),
    boxBlock('frame-sill', [i.x0, dt, plinth], [i.x0 + i.w, dt + 20, i.z0], 'body', {
      layer: 3,
      only: ['front'],
    }),
    // The machine base: slightly set back, with a grille or a hinged door.
    boxBlock('base', [10, dt + 10, 0], [e.w - 10, e.d - 10, plinth], 'plinth', {
      layer: 3,
      only: ['front', 'left', 'right'],
    }),
  ];
  const panel =
    cabinet.base === 'grille'
      ? { x0: e.w * 0.2, x1: e.w * 0.8, z0: plinth * 0.2, z1: plinth * 0.7 }
      : { x0: 40, x1: e.w - 40, z0: plinth * 0.12, z1: plinth * 0.88 };
  blocks.push(
    boxBlock('base-front', [panel.x0, dt + 4, panel.z0], [panel.x1, dt + 10, panel.z1], 'grille', {
      layer: 3,
    }),
  );
  return blocks;
}

function liner(cabinet: CabinetSpec): Block[] {
  const { x0, y0, z0, w, d, h } = cabinet.interior;
  const blocks: Block[] = [
    box('liner-back', { x0, y0: y0 + d - LINER, z0, w, d: LINER, h }, 'interior', { layer: 0 }),
    box('liner-floor', { x0, y0, z0: z0 - LINER, w, d, h: LINER }, 'interior', { layer: 0 }),
    box('liner-roof', { x0, y0, z0: z0 + h, w, d, h: LINER }, 'interior', { layer: 0 }),
    box('liner-left', { x0: x0 - LINER, y0, z0, w: LINER, d, h }, 'interior', { layer: 0 }),
    box('liner-right', { x0: x0 + w, y0, z0, w: LINER, d, h }, 'interior', { layer: 0 }),
  ];
  // A shelf under every compartment but the bottom one.
  for (const compartment of cabinet.compartments) {
    if (compartment.level === 0) continue;
    blocks.push(
      box(
        `shelf-${String(compartment.level)}`,
        { x0, y0, z0: compartment.z0 - 12, w, d, h: 12 },
        'shelf',
        { layer: 0 },
      ),
    );
  }
  return blocks;
}

function door(cabinet: CabinetSpec, open: number): Block[] {
  const { exterior: e, plinth, doorThickness: dt, panel } = cabinet;
  const right = cabinet.hinge === 'right';
  const pivot: Vec3 = [right ? e.w : 0, 0, plinth];
  const angle = (right ? 1 : -1) * DOOR_SWING * open;
  const height = e.h - plinth;
  const across = right ? -e.w : e.w;
  const target: BlockTarget = { kind: 'door' };
  // Door fittings, in the door's own unturned frame: x runs across the door
  // from its left edge (`left`), so every fitting has a positive width.
  const left = right ? -e.w : 0;
  const part = (id: string, offset: Vec3, size: Vec3, material: Material): Block =>
    rotatedBlock(id, pivot, [size[0], 0, 0], [0, size[1], 0], [0, 0, size[2]], angle, material, {
      layer: 4,
      target,
      originOffset: offset,
    });
  // The handle sits by the free edge; the panel high on the handle side.
  const handleX = right ? left + 30 : left + e.w - 66;
  const panelX = right ? left + 70 : left + e.w - 70 - panel.width;
  return [
    rotatedBlock('door', pivot, [across, 0, 0], [0, dt, 0], [0, 0, height], angle, 'door', {
      layer: 4,
      target,
    }),
    part('door-gasket', [left + 8, dt, 20], [e.w - 16, 6, height - 40], 'gasket'),
    part('door-handle', [handleX, -45, height * 0.42], [36, 45, 320], 'handle'),
    part(
      'door-panel',
      [panelX, -6, height * (1 - panel.fromTop) - panel.height],
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
    if (open <= 0) {
      return [
        box(
          `inner-${String(index)}`,
          { x0: i.x0, y0: i.y0 - 14, z0, w: i.w, d: 10, h },
          'innerDoor',
          { layer: 2, target },
        ),
      ];
    }
    const pivot: Vec3 = [right ? i.x0 + i.w : i.x0, i.y0 - 14, z0];
    return [
      rotatedBlock(
        `inner-${String(index)}`,
        pivot,
        [right ? -i.w : i.w, 0, 0],
        [0, 10, 0],
        [0, 0, h],
        (right ? 1 : -1) * DOOR_SWING * open,
        'innerDoor',
        { layer: 4, target },
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

function rackBlocks(cabinet: CabinetSpec, rack: RackModel, slot: Extent, pull: number): Block[] {
  const target: BlockTarget = { kind: 'rack', level: rack.level, column: rack.column };
  if (pull <= 0) {
    return [box(`rack-${rack.id}`, slot, 'rack', { layer: 1, target })];
  }
  const moved: Extent = { ...slot, y0: slot.y0 - pullDistance(cabinet) * pull };
  const plate = 8;
  const blocks: Block[] = [
    // A pulled rack is drawn open: floor, back and one side plate, so its
    // boxes show.
    box(`rack-${rack.id}-floor`, { ...moved, h: plate }, 'rackActive', { layer: 4, target }),
    box(
      `rack-${rack.id}-back`,
      { ...moved, y0: moved.y0 + moved.d - plate, d: plate },
      'rackActive',
      { layer: 4, target },
    ),
    box(
      `rack-${rack.id}-side`,
      { ...moved, x0: cabinet.hinge === 'right' ? moved.x0 : moved.x0 + moved.w - plate, w: plate },
      'rackActive',
      { layer: 4, target },
    ),
  ];
  const grid = rack.grid ?? { levels: 1, columns: 1, depths: 1 };
  const cellW = moved.w / grid.columns;
  const cellD = (moved.d - plate) / grid.depths;
  const cellH = (moved.h - plate) / grid.levels;
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
      box(
        `cell-${cell.id}`,
        {
          x0: moved.x0 + cell.column * cellW + cellW * inset,
          y0: moved.y0 + cell.depth * cellD + cellD * inset,
          z0: moved.z0 + plate + cell.level * cellH + 2,
          w: cellW * (1 - 2 * inset),
          d: cellD * (1 - 2 * inset),
          h: cellH * 0.82,
        },
        material,
        { layer: 4, target: cellTarget },
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
  const { exterior: e } = cabinet;
  const swing = e.w * 1.05;
  const x0 = cabinet.hinge === 'left' ? -swing * 0.25 : -20;
  const x1 = cabinet.hinge === 'right' ? e.w + swing * 0.25 : e.w + 20;
  const y0 = -Math.max(swing, pullDistance(cabinet) + 40);
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
