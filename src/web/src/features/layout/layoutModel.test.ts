// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import {
  BoxSchema,
  BoxTypeSchema,
  ContainerKind,
  FreezerSchema,
  StorageContainerSchema,
  type Box,
  type BoxType,
  type Freezer,
  type StorageContainer,
} from '../../gen/fmgr/v1/box_pb';
import { createDemoLab, type DemoLab } from '../../test/fakeApi';
import {
  buildLayoutTree,
  resolveLocationPath,
  type LabLayoutData,
  type LayoutNode,
} from './layoutModel';

/**
 * The layout model (TODO.md G3.1) — the tree and the location path, without
 * React or the network.
 *
 * The two guards are the part that has to survive review: the Qt client's
 * `LocationPathResolver` walks a parent chain that comes from the server, and
 * a chain that is broken (orphan) or loops (cycle) must degrade to a *partial*
 * path rather than hanging the tab or inventing a location. `2afb49b` fixed
 * exactly that class of bug in the Qt file, so both cases are pinned here and
 * both are proven able to fail (see the PR).
 *
 * The other rules: archived rows are hidden by the *client* (the list RPCs
 * have no `include_archived` field, so the data always contains them), and the
 * counts are per subtree, not per level.
 */

/** What the server would return for one lab — the model never re-filters. */
function labData(lab: DemoLab, labId = 'lab-demo'): LabLayoutData {
  return {
    freezers: lab.freezers.filter((freezer) => freezer.labId === labId),
    storageContainers: lab.storageContainers.filter((container) => container.labId === labId),
    boxTypes: lab.boxTypes.filter((boxType) => boxType.labId === labId),
    boxes: lab.boxes.filter((box) => box.labId === labId),
  };
}

function flatten(nodes: readonly LayoutNode[]): LayoutNode[] {
  return nodes.flatMap((node) => [node, ...flatten(node.children)]);
}

function freezer(id: string, layoutRootId: string, overrides: Partial<Freezer> = {}): Freezer {
  return create(FreezerSchema, { id, labId: 'lab-demo', name: id, layoutRootId, ...overrides });
}

function container(
  id: string,
  parentId?: string,
  overrides: Partial<StorageContainer> = {},
): StorageContainer {
  return create(StorageContainerSchema, {
    id,
    labId: 'lab-demo',
    kind: ContainerKind.RACK,
    name: id,
    orderingIndex: 0,
    parentId,
    ...overrides,
  });
}

function box(id: string, storageContainerId: string, overrides: Partial<Box> = {}): Box {
  return create(BoxSchema, {
    id,
    labId: 'lab-demo',
    boxTypeId: 'bt-96',
    storageContainerId,
    label: id,
    ...overrides,
  });
}

/** One archived_at value, so "is archived" is the same instant everywhere. */
const ARCHIVED = { unixMicros: 1n };

function boxType(id: string, positions = 4): BoxType {
  return create(BoxTypeSchema, {
    id,
    labId: 'lab-demo',
    name: id,
    positions: Array.from({ length: positions }, (_, index) => ({
      label: `A${String(index + 1)}`,
      row: 1,
      col: index + 1,
    })),
  });
}

describe('buildLayoutTree', () => {
  it('nests containers under the freezer that names their root, and boxes under their container', () => {
    const tree = buildLayoutTree(labData(createDemoLab()));

    expect(tree.map((node) => ({ kind: node.kind, label: node.label }))).toEqual([
      { kind: 'freezer', label: 'Freezer A' },
      { kind: 'freezer', label: 'Freezer B' },
    ]);

    const [freezerA, freezerB] = tree;
    expect(freezerA?.children.map((node) => node.id)).toEqual(['ct-rack-1']);
    expect(freezerA?.children[0]?.children.map((node) => node.id)).toEqual([
      'ct-drawer-1',
      'ct-drawer-2',
    ]);
    expect(freezerA?.children[0]?.children[0]?.children.map((node) => node.label)).toEqual([
      'Box A',
      'Box B',
    ]);
    expect(freezerB?.children[0]?.children[0]?.children.map((node) => node.label)).toEqual([
      'Box C',
    ]);
  });

  it('prefers a container’s human label over its name, as the Qt resolver does', () => {
    const tree = buildLayoutTree(labData(createDemoLab()));
    const drawer = tree[0]?.children[0]?.children[0];

    // ct-drawer-1 is seeded with both: label 'Top drawer', name 'Drawer 1'.
    expect(drawer?.label).toBe('Top drawer');
  });

  it('counts the boxes of the whole subtree, not just the direct children', () => {
    const tree = buildLayoutTree(labData(createDemoLab()));
    const [freezerA, freezerB] = tree;
    const rack = freezerA?.children[0];
    const [drawerOne, drawerTwo] = rack?.children ?? [];

    expect(freezerA?.boxCount).toBe(2); // Box A + Box B; 'Old box' is archived
    expect(rack?.boxCount).toBe(2);
    expect(drawerOne?.boxCount).toBe(2);
    expect(drawerTwo?.boxCount).toBe(0);
    expect(freezerB?.boxCount).toBe(1);
  });

  it('reports the positions a box declares through its box type', () => {
    const tree = buildLayoutTree(labData(createDemoLab()));
    const boxA = tree[0]?.children[0]?.children[0]?.children[0];
    const boxC = tree[1]?.children[0]?.children[0]?.children[0];

    expect(boxA?.positionCount).toBe(96);
    expect(boxC?.positionCount).toBe(9);
  });

  it('leaves the position count unknown when the box type is not in the loaded set', () => {
    // An archived box type is filtered out by the tree, exactly like an
    // archived box: the box is still listed, its size just cannot be named.
    const data = labData(createDemoLab());
    const tree = buildLayoutTree({ ...data, boxTypes: [] });
    const boxA = tree[0]?.children[0]?.children[0]?.children[0];

    expect(boxA?.positionCount).toBeNull();
    expect(boxA?.label).toBe('Box A');
  });

  it('hides an archived freezer, container and box, and their subtrees', () => {
    const data: LabLayoutData = {
      freezers: [freezer('fz-live', 'ct-root'), freezer('fz-gone', 'ct-root', { archivedAt: ARCHIVED })],
      storageContainers: [
        container('ct-root'),
        container('ct-dead', 'ct-root', { archivedAt: ARCHIVED }),
        container('ct-under-dead', 'ct-dead'),
      ],
      boxTypes: [boxType('bt-96')],
      boxes: [
        box('box-live', 'ct-root'),
        box('box-dead', 'ct-root', { archivedAt: ARCHIVED }),
        box('box-orphaned-by-archive', 'ct-under-dead'),
      ],
    };

    // Only box-dead is archived outright. The archived *container* takes its
    // own subtree with it, because a live box under a dead parent cannot be
    // shown as if it were reachable.
    const tree = buildLayoutTree(data);
    const ids = flatten(tree).map((node) => node.id);

    expect(tree.map((node) => node.id)).toEqual(['fz-live']);
    expect(ids).toContain('ct-root');
    expect(ids).toContain('box-live');
    expect(ids).not.toContain('fz-gone');
    expect(ids).not.toContain('ct-dead');
    expect(ids).not.toContain('ct-under-dead');
    expect(ids).not.toContain('box-dead');
    expect(ids).not.toContain('box-orphaned-by-archive');
  });

  it('terminates on a container cycle and renders each node once', () => {
    const data: LabLayoutData = {
      freezers: [freezer('fz', 'ct-a')],
      // ct-a → ct-b → ct-a: possible whenever a re-parenting goes wrong.
      storageContainers: [container('ct-a', 'ct-b'), container('ct-b', 'ct-a')],
      boxTypes: [boxType('bt-96')],
      boxes: [box('box-1', 'ct-b')],
    };

    const tree = buildLayoutTree(data);
    const ids = flatten(tree).map((node) => node.id);

    expect(ids.filter((id) => id === 'ct-a')).toHaveLength(1);
    expect(ids.filter((id) => id === 'ct-b')).toHaveLength(1);
    expect(ids).toContain('box-1');
  });

  it('ignores a container whose parent is missing instead of inventing a root', () => {
    const data: LabLayoutData = {
      freezers: [freezer('fz', 'ct-root')],
      storageContainers: [container('ct-root'), container('ct-orphan', 'ct-missing')],
      boxTypes: [boxType('bt-96')],
      boxes: [box('box-in-orphan', 'ct-orphan')],
    };

    const ids = flatten(buildLayoutTree(data)).map((node) => node.id);

    expect(ids).toEqual(['fz', 'ct-root']);
  });

  it('ignores a root container no freezer names', () => {
    const data: LabLayoutData = {
      freezers: [freezer('fz', 'ct-root')],
      storageContainers: [container('ct-root'), container('ct-unattached')],
      boxTypes: [boxType('bt-96')],
      boxes: [box('box-in-root', 'ct-root'), box('box-in-unattached', 'ct-unattached')],
    };

    const ids = flatten(buildLayoutTree(data)).map((node) => node.id);

    expect(ids).toContain('box-in-root');
    expect(ids).not.toContain('ct-unattached');
    expect(ids).not.toContain('box-in-unattached');
  });

  it('renders a freezer whose layout root is missing as an empty node', () => {
    const data: LabLayoutData = {
      freezers: [freezer('fz', 'ct-nope')],
      storageContainers: [],
      boxTypes: [],
      boxes: [],
    };

    expect(buildLayoutTree(data)).toMatchObject([{ id: 'fz', boxCount: 0, children: [] }]);
  });

  it('orders containers by ordering_index and boxes by label, so the tree is stable', () => {
    const data: LabLayoutData = {
      freezers: [freezer('fz', 'ct-root')],
      storageContainers: [
        container('ct-root'),
        container('ct-second', 'ct-root', { orderingIndex: 2, name: 'second' }),
        container('ct-first', 'ct-root', { orderingIndex: 1, name: 'first' }),
      ],
      boxTypes: [boxType('bt-96')],
      boxes: [
        create(BoxSchema, {
          id: 'box-b',
          labId: 'lab-demo',
          boxTypeId: 'bt-96',
          storageContainerId: 'ct-root',
          label: 'b',
        }),
        create(BoxSchema, {
          id: 'box-a',
          labId: 'lab-demo',
          boxTypeId: 'bt-96',
          storageContainerId: 'ct-root',
          label: 'a',
        }),
      ],
    };

    const root = buildLayoutTree(data)[0]?.children[0];

    // Sub-containers first, in ordering_index order, then the boxes by label.
    expect(root?.children.map((node) => node.id)).toEqual([
      'ct-first',
      'ct-second',
      'box-a',
      'box-b',
    ]);
    expect(root?.children[2]?.label).toBe('a');
  });
});

describe('resolveLocationPath', () => {
  it('resolves freezer → container → box → position for a placed sample', () => {
    const path = resolveLocationPath(labData(createDemoLab()), 'box-1', 'A1');

    expect(path.placed).toBe(true);
    expect(path.partial).toBe(false);
    expect(path.segments.map((segment) => [segment.kind, segment.label])).toEqual([
      ['freezer', 'Freezer A'],
      ['container', 'Rack 1'],
      ['container', 'Top drawer'],
      ['box', 'Box A'],
      ['position', 'A1'],
    ]);
  });

  it('names the container kind on a container segment, so the UI can prefix it', () => {
    const path = resolveLocationPath(labData(createDemoLab()), 'box-1', 'A1');
    const kinds = path.segments.map((segment) => segment.containerKind);

    expect(kinds).toEqual([null, ContainerKind.RACK, ContainerKind.DRAWER, null, null]);
  });

  it('omits the position segment when the sample has no position label', () => {
    const path = resolveLocationPath(labData(createDemoLab()), 'box-1');

    expect(path.segments.at(-1)?.kind).toBe('box');
  });

  it('resolves an unplaced sample to an empty path, which is not a failure', () => {
    const path = resolveLocationPath(labData(createDemoLab()), '');

    expect(path).toEqual({ placed: false, partial: false, segments: [] });
  });

  it('reports an unknown box as partial instead of throwing', () => {
    const path = resolveLocationPath(labData(createDemoLab()), 'box-nope', 'A1');

    expect(path.placed).toBe(false);
    expect(path.partial).toBe(true);
    expect(path.segments).toEqual([]);
  });

  it('stops at an orphaned container, keeps what it found, and flags the path partial', () => {
    const data: LabLayoutData = {
      freezers: [freezer('fz', 'ct-root')],
      storageContainers: [container('ct-root'), container('ct-orphan', 'ct-missing')],
      boxTypes: [boxType('bt-96')],
      boxes: [box('box-1', 'ct-orphan')],
    };

    const path = resolveLocationPath(data, 'box-1', 'A1');

    // Best effort: no freezer (the chain never reached a root), but the
    // container the box sits in is known and worth showing.
    expect(path.partial).toBe(true);
    expect(path.placed).toBe(true);
    expect(path.segments.map((segment) => [segment.kind, segment.label])).toEqual([
      ['container', 'ct-orphan'],
      ['box', 'box-1'],
      ['position', 'A1'],
    ]);
  });

  it('stops on a cycle instead of looping forever, and does not repeat a container', () => {
    const data: LabLayoutData = {
      freezers: [freezer('fz', 'ct-a')],
      storageContainers: [container('ct-a', 'ct-b'), container('ct-b', 'ct-a')],
      boxTypes: [boxType('bt-96')],
      boxes: [box('box-1', 'ct-b')],
    };

    const path = resolveLocationPath(data, 'box-1', 'A1');
    const containers = path.segments.filter((segment) => segment.kind === 'container');

    expect(path.partial).toBe(true);
    expect(path.placed).toBe(true);
    expect(containers.map((segment) => segment.label)).toEqual(['ct-a', 'ct-b']);
    // Outermost first, and every node exactly once: a cycle must not grow the
    // segment list.
    expect(new Set(containers.map((segment) => segment.label)).size).toBe(containers.length);
  });

  it('still resolves through an archived container, because the sample really is there', () => {
    // The *tree* hides archived nodes (it is a browsable inventory); the path
    // answers "where is this sample", which the Qt resolver also answers
    // without an archived filter.
    const data: LabLayoutData = {
      freezers: [freezer('fz', 'ct-root')],
      storageContainers: [
        container('ct-root'),
        container('ct-dead', 'ct-root', { archivedAt: { unixMicros: 1n } }),
      ],
      boxTypes: [boxType('bt-96')],
      boxes: [box('box-1', 'ct-dead')],
    };

    const path = resolveLocationPath(data, 'box-1', 'A1');

    expect(path.partial).toBe(false);
    expect(path.segments.map((segment) => segment.label)).toEqual([
      'fz',
      'ct-root',
      'ct-dead',
      'box-1',
      'A1',
    ]);
  });

  it('names the last container but no freezer when the root container belongs to no freezer', () => {
    const data: LabLayoutData = {
      freezers: [],
      storageContainers: [container('ct-root')],
      boxTypes: [boxType('bt-96')],
      boxes: [box('box-1', 'ct-root')],
    };

    const path = resolveLocationPath(data, 'box-1', 'A1');

    expect(path.partial).toBe(false);
    expect(path.segments.map((segment) => segment.kind)).toEqual(['container', 'box', 'position']);
  });
});
