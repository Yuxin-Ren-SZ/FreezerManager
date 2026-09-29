// SPDX-License-Identifier: AGPL-3.0-or-later
import type {
  Box,
  BoxType,
  ContainerKind,
  Freezer,
  StorageContainer,
} from '../../gen/fmgr/v1/box_pb';

/**
 * The lab's storage layout as a value: the tree the G3.1 screen renders and the
 * location path later screens resolve (TODO.md G3.1, G3.2–G3.5, G3.8).
 *
 * Deliberately pure and React-free: G3.4 (box view) and G3.5 (lookup) need the
 * same answers without mounting a tree, and the two guards below are the kind
 * of thing that has to be unit-testable on its own.
 *
 * **Where the guards come from.** `src/qt/LocationPathResolver.cc` is the
 * reference implementation: it walks the storage-container parent chain from
 * the box upwards, and a chain that never reaches a root (orphan) or that
 * revisits a node (cycle) must produce a *partial* path rather than hanging or
 * inventing a location. That file shipped the cycle guard only after `2afb49b`
 * fixed a real recursion bug, so the same walk here carries the same two
 * guards — a `Set` of visited ids plus a "parent not in the loaded set" check —
 * instead of a variant invented on the spot.
 *
 * **Archived rows arrive in the data.** The BoxService list RPCs have no
 * `include_archived` field, so an archived freezer, container, box type or box
 * is always in the response and the *client* is what hides it. The two
 * functions disagree on purpose: the tree is a browsable inventory, so an
 * archived node (and its subtree) is hidden; the path answers "where is this
 * sample", so it resolves through an archived container exactly as the Qt
 * resolver does, which has no archived filter at all.
 */

/** The four BoxService lists, as the queries return them. */
export interface LabLayoutData {
  readonly freezers: readonly Freezer[];
  readonly storageContainers: readonly StorageContainer[];
  readonly boxTypes: readonly BoxType[];
  readonly boxes: readonly Box[];
}

export type LayoutNodeKind = 'freezer' | 'container' | 'box';

/** One node of the layout tree. */
export interface LayoutNode {
  readonly id: string;
  readonly kind: LayoutNodeKind;
  /** A container's human label when it has one, else its name — as in Qt. */
  readonly label: string;
  /** The container kind, so the UI can prefix it with a translated label. */
  readonly containerKind: ContainerKind | null;
  /** Visible boxes in this subtree; 1 for a box node. */
  readonly boxCount: number;
  /** Positions the box's type declares; `null` on non-box nodes and when the
   * type is not in the loaded set (archived, or belonging to another lab). */
  readonly positionCount: number | null;
  readonly children: readonly LayoutNode[];
}

export type LocationSegmentKind = 'freezer' | 'container' | 'box' | 'position';

/** One rung of a location path, ordered outermost first. */
export interface LocationPathSegment {
  readonly kind: LocationSegmentKind;
  readonly label: string;
  /** Set on container segments only (Qt's `PathSegment::detail`). */
  readonly containerKind: ContainerKind | null;
}

/** What `resolveLocationPath` returns. Mirrors Qt's `Result`. */
export interface LocationPath {
  /** A box was named. `false` for an unplaced sample or an unknown box. */
  readonly placed: boolean;
  /** The chain broke (orphan or cycle): the path is best effort. */
  readonly partial: boolean;
  readonly segments: readonly LocationPathSegment[];
}

const isArchived = (row: { readonly archivedAt?: unknown }): boolean =>
  row.archivedAt !== undefined;

/** Qt's `containerLabel`: the human label if set, else the name. */
function containerLabel(container: StorageContainer): string {
  return container.label === '' ? container.name : container.label;
}

function byNameThenId(a: { name: string; id: string }, b: { name: string; id: string }): number {
  return a.name.localeCompare(b.name) || a.id.localeCompare(b.id);
}

function byOrdering(
  a: { orderingIndex: number; name: string; id: string },
  b: { orderingIndex: number; name: string; id: string },
): number {
  return a.orderingIndex - b.orderingIndex || byNameThenId(a, b);
}

/** Boxes are ordered by the label the user sees, then by id for stability. */
function byLabel(a: Box, b: Box): number {
  return a.label.localeCompare(b.label) || a.id.localeCompare(b.id);
}

/**
 * The forest the layout screen renders: one node per visible freezer, its root
 * container below it, and so on down to the boxes.
 *
 * A freezer's children start at the container its `layout_root_id` names —
 * the same pairing the Qt resolver uses to name a freezer. A container that no
 * freezer names, or whose parent is not in the loaded set, is not reachable
 * and is therefore not rendered; that is a data problem for the admin screen
 * (G3.8) to show, not something to paper over with an invented root.
 */
export function buildLayoutTree(data: LabLayoutData): readonly LayoutNode[] {
  const containers = data.storageContainers.filter((container) => !isArchived(container));
  const containersById = new Map(containers.map((container) => [container.id, container]));
  const boxTypesById = new Map(
    data.boxTypes.filter((boxType) => !isArchived(boxType)).map((boxType) => [boxType.id, boxType]),
  );

  const boxesByContainer = new Map<string, Box[]>();
  for (const box of data.boxes) {
    if (isArchived(box)) continue;
    const list = boxesByContainer.get(box.storageContainerId);
    if (list === undefined) {
      boxesByContainer.set(box.storageContainerId, [box]);
    } else {
      list.push(box);
    }
  }

  // Shared by the whole forest, not per subtree: a container that a cycle (or
  // two freezers claiming the same root) would reach twice is rendered once,
  // and the walk always terminates.
  const rendered = new Set<string>();

  const boxNode = (box: Box): LayoutNode => ({
    id: box.id,
    kind: 'box',
    label: box.label,
    containerKind: null,
    boxCount: 1,
    positionCount: boxTypesById.get(box.boxTypeId)?.positions.length ?? null,
    children: [],
  });

  const containerNode = (container: StorageContainer): LayoutNode => {
    rendered.add(container.id);
    const boxes = [...(boxesByContainer.get(container.id) ?? [])].sort(byLabel);
    const childNodes = [
      ...containers
        .filter((child) => child.parentId === container.id && !rendered.has(child.id))
        .sort(byOrdering)
        .map((child) => containerNode(child)),
      ...boxes.map((box) => boxNode(box)),
    ];

    return {
      id: container.id,
      kind: 'container',
      label: containerLabel(container),
      containerKind: container.kind,
      // A box node counts itself, so the subtree total is the sum of the
      // children and must not add the direct boxes a second time.
      boxCount: childNodes.reduce((sum, node) => sum + node.boxCount, 0),
      positionCount: null,
      children: childNodes,
    };
  };

  return data.freezers
    .filter((freezer) => !isArchived(freezer))
    .sort(byNameThenId)
    .map((freezer) => {
      const root =
        freezer.layoutRootId === '' ? undefined : containersById.get(freezer.layoutRootId);
      const children = root === undefined || rendered.has(root.id) ? [] : [containerNode(root)];

      return {
        id: freezer.id,
        kind: 'freezer' as const,
        label: freezer.name,
        containerKind: null,
        boxCount: children.reduce((sum, node) => sum + node.boxCount, 0),
        positionCount: null,
        children,
      };
    });
}

/**
 * The full human-readable location of a placement: freezer → … → box →
 * position, outermost first.
 *
 * `boxId === ''` is an unplaced sample, which is a normal state and not a
 * failure — Qt returns `ok && !placed` for it and so does this. An id that is
 * not in the loaded boxes is `partial` with no segments: the caller still gets
 * a value to render instead of an exception from a render function.
 */
export function resolveLocationPath(
  data: LabLayoutData,
  boxId: string,
  position = '',
): LocationPath {
  if (boxId === '') {
    return { placed: false, partial: false, segments: [] };
  }

  const box = data.boxes.find((candidate) => candidate.id === boxId);
  if (box === undefined) {
    return { placed: false, partial: true, segments: [] };
  }

  const containersById = new Map(
    data.storageContainers.map((container) => [container.id, container]),
  );

  // Walk the parent chain from the box's container up to a parentless root,
  // collecting box-side first. `visited` is the cycle guard and the
  // `containersById` miss is the orphan guard; both end the walk, which is the
  // whole point — the loop must terminate on data the server allowed.
  const chain: StorageContainer[] = [];
  const visited = new Set<string>();
  let rootId = '';
  let partial = false;
  let cursor = box.storageContainerId;

  while (cursor !== '') {
    if (visited.has(cursor)) {
      partial = true;
      break;
    }
    visited.add(cursor);

    const container = containersById.get(cursor);
    if (container === undefined) {
      partial = true;
      break;
    }
    chain.push(container);

    const parentId = container.parentId ?? '';
    if (parentId === '') {
      rootId = container.id;
      break;
    }
    cursor = parentId;
  }

  const segments: LocationPathSegment[] = [];

  // Name the enclosing freezer by matching its layout_root_id to the chain's
  // root, exactly as Qt does: no root reached (or no freezer claiming it) means
  // no freezer segment, which is what makes `partial` visible to the user.
  if (rootId !== '') {
    const home = data.freezers.find((freezer) => freezer.layoutRootId === rootId);
    if (home !== undefined) {
      segments.push({ kind: 'freezer', label: home.name, containerKind: null });
    }
  }

  for (const container of [...chain].reverse()) {
    segments.push({
      kind: 'container',
      label: containerLabel(container),
      containerKind: container.kind,
    });
  }

  segments.push({ kind: 'box', label: box.label, containerKind: null });
  if (position !== '') {
    segments.push({ kind: 'position', label: position, containerKind: null });
  }

  return { placed: true, partial, segments };
}
