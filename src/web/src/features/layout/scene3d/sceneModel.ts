// SPDX-License-Identifier: AGPL-3.0-or-later
import {
  SceneNodeKind,
  type FreezerModel,
  type GetFreezerSceneResponse,
  type SceneNode,
} from '../../../gen/fmgr/v1/box_pb';

/**
 * `freezer/scene` → what the 3D view draws (ADR 0003 §3, §5). Pure and
 * React-free, like `layoutModel.ts`.
 *
 * The server sends the live layout under the freezer's root, parents first.
 * This turns it into:
 *
 * - **racks**: the containers placed directly in the cabinet, each at its
 *   compartment (`level`) and slot (`column`), when the model has that slot;
 * - **cells**: what sits in a rack's grid — a box, or a drawer with boxes of
 *   its own (the TSX's drawer racks);
 * - **loose** boxes: in a rack but without a cell, still reachable from it;
 * - **unplaced**: everything else directly in the cabinet. Nothing is dropped
 *   silently, so the 3D view never under-reports a freezer.
 *
 * Occupancy is the server's count per box; no sample data reaches the scene.
 */

export interface GridSize {
  readonly levels: number;
  readonly columns: number;
  readonly depths: number;
}

export interface BoxSummary {
  readonly id: string;
  readonly label: string;
  readonly occupied: number;
  readonly total: number;
}

export interface RackCell {
  readonly id: string;
  readonly kind: 'box' | 'container';
  readonly label: string;
  readonly level: number;
  readonly column: number;
  readonly depth: number;
  /** The box itself, or every box in the drawer. */
  readonly boxes: readonly BoxSummary[];
  readonly occupied: number;
  readonly total: number;
}

export interface RackModel {
  readonly id: string;
  readonly label: string;
  readonly level: number;
  readonly column: number;
  readonly grid: GridSize | null;
  readonly cells: readonly RackCell[];
  readonly loose: readonly BoxSummary[];
  readonly boxCount: number;
  readonly occupied: number;
  readonly total: number;
}

export interface UnplacedItem {
  readonly id: string;
  readonly kind: 'box' | 'container';
  readonly label: string;
  /** Boxes in this subtree; 1 for a box. */
  readonly boxes: number;
}

export interface RenderModel {
  readonly freezerId: string;
  readonly freezerName: string;
  readonly model: FreezerModel | null;
  readonly racks: readonly RackModel[];
  readonly unplaced: readonly UnplacedItem[];
}

export type FillLevel = 'empty' | 'partial' | 'full';

export function fillLevel(occupied: number, total: number): FillLevel {
  if (occupied <= 0 || total <= 0) {
    return 'empty';
  }
  return occupied >= total ? 'full' : 'partial';
}

const isBox = (node: SceneNode) => node.kind === SceneNodeKind.BOX;

const boxSummary = (node: SceneNode): BoxSummary => ({
  id: node.id,
  label: node.label,
  occupied: node.occupiedPositions,
  total: node.totalPositions,
});

const sum = (boxes: readonly BoxSummary[], pick: (box: BoxSummary) => number) =>
  boxes.reduce((total, box) => total + pick(box), 0);

/** True when the model has slot `column` in compartment `level`. */
function cabinetHasSlot(model: FreezerModel | null, level: number, column: number, depth: number) {
  if (model === null || depth !== 0 || column < 0) {
    return false;
  }
  const compartment = model.compartments.find((candidate) => candidate.level === level);
  return compartment !== undefined && column < compartment.rackSlots;
}

/** Row-major order through a grid: bottom tier first, front to back, left to right. */
const byCell = (left: RackCell, right: RackCell) =>
  left.level - right.level || left.depth - right.depth || left.column - right.column;

export function buildRenderModel(scene: GetFreezerSceneResponse): RenderModel {
  const freezer = scene.freezer;
  const model = scene.model ?? null;
  const rootId = freezer?.layoutRootId ?? scene.nodes.at(0)?.id ?? '';

  const children = new Map<string, SceneNode[]>();
  for (const node of scene.nodes) {
    if (node.parentId === undefined) continue;
    const list = children.get(node.parentId);
    if (list === undefined) {
      children.set(node.parentId, [node]);
    } else {
      list.push(node);
    }
  }
  const childrenOf = (id: string) => children.get(id) ?? [];

  // Every box at any depth below a node; the visited set guards against a cycle
  // the server should never send.
  const boxesUnder = (id: string, visited = new Set<string>()): BoxSummary[] => {
    if (visited.has(id)) return [];
    visited.add(id);
    return childrenOf(id).flatMap((child) =>
      isBox(child) ? [boxSummary(child)] : boxesUnder(child.id, visited),
    );
  };

  const racks: RackModel[] = [];
  const unplaced: UnplacedItem[] = [];

  for (const node of childrenOf(rootId)) {
    const placement = node.placement;
    if (
      isBox(node) ||
      placement === undefined ||
      !cabinetHasSlot(model, placement.level, placement.column, placement.depth)
    ) {
      unplaced.push({
        id: node.id,
        kind: isBox(node) ? 'box' : 'container',
        label: node.label,
        boxes: isBox(node) ? 1 : boxesUnder(node.id).length,
      });
      continue;
    }

    const cells: RackCell[] = [];
    const loose: BoxSummary[] = [];
    for (const child of childrenOf(node.id)) {
      const cell = child.placement;
      if (cell === undefined) {
        loose.push(...(isBox(child) ? [boxSummary(child)] : boxesUnder(child.id)));
        continue;
      }
      const boxes = isBox(child) ? [boxSummary(child)] : boxesUnder(child.id);
      cells.push({
        id: child.id,
        kind: isBox(child) ? 'box' : 'container',
        label: child.label,
        level: cell.level,
        column: cell.column,
        depth: cell.depth,
        boxes,
        occupied: sum(boxes, (box) => box.occupied),
        total: sum(boxes, (box) => box.total),
      });
    }
    cells.sort(byCell);

    const everyBox = [...cells.flatMap((cell) => cell.boxes), ...loose];
    racks.push({
      id: node.id,
      label: node.label,
      level: placement.level,
      column: placement.column,
      grid:
        node.grid === undefined
          ? null
          : { levels: node.grid.levels, columns: node.grid.columns, depths: node.grid.depths },
      cells,
      loose,
      boxCount: everyBox.length,
      occupied: sum(everyBox, (box) => box.occupied),
      total: sum(everyBox, (box) => box.total),
    });
  }

  return {
    freezerId: freezer?.id ?? '',
    freezerName: freezer?.name ?? '',
    model,
    racks,
    unplaced,
  };
}

/** The rack in a slot, if one is placed there. */
export function rackAt(render: RenderModel, level: number, column: number): RackModel | undefined {
  return render.racks.find((rack) => rack.level === level && rack.column === column);
}
