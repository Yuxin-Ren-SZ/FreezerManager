// SPDX-License-Identifier: AGPL-3.0-or-later
import { act, renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it } from 'vitest';
import { ApiError } from '../../api/errors';
import { layoutKeys } from '../../api/hooks';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { createWrapper } from '../../test/render';
import { server } from '../../test/server';
import { useLabLayout } from './useLabLayout';

/**
 * `useLabLayout` (TODO.md G3.1) — the one hook G3.2–G3.5 and G3.8 build on.
 *
 * Three things are contractual and are what these tests pin:
 *
 *  1. **Every query key includes `lab_id`** (G1.2's convention, G-arch 7). A
 *     missing one is cross-lab cache contamination, which is a data-leak class
 *     of bug, not a rendering bug.
 *  2. **One request per resource per mount.** The tree needs all four lists;
 *     a hook that refetches per consumer multiplies the load on `freezerd`.
 *  3. **A failure anywhere in the four is visible as an error**, never as a
 *     silently partial tree — "the lab has no boxes" and "the box list did not
 *     load" must not look the same (the lesson `doc/dev/web.md` records for
 *     `fakeApi`'s default answers).
 */

let lab: DemoLab;

/** Paths the hook's four queries hit, in the order they arrived. */
let requested: string[] = [];

const LAYOUT_PATHS = [
  '/api/v1/freezer/list',
  '/api/v1/storage-container/list',
  '/api/v1/box-type/list',
  '/api/v1/box/list',
] as const;

const requestsPerPath = () =>
  LAYOUT_PATHS.map((path) => requested.filter((candidate) => candidate === path).length);

server.events.on('request:start', ({ request }) => {
  requested.push(new URL(request.url).pathname);
});

beforeEach(() => {
  requested = [];
  lab = createDemoLab();
  server.use(...fakeApi({ lab }));
});

describe('layoutKeys', () => {
  it('includes lab_id in every key, so two labs can never share a cache entry', () => {
    const keys = [
      layoutKeys.all('lab-1'),
      layoutKeys.freezers('lab-1'),
      layoutKeys.storageContainers('lab-1'),
      layoutKeys.boxTypes('lab-1'),
      layoutKeys.boxes('lab-1'),
    ];

    for (const key of keys) {
      expect(key).toContain('lab-1');
    }
    expect(layoutKeys.boxes('lab-1')).not.toEqual(layoutKeys.boxes('lab-2'));
  });
});

describe('useLabLayout', () => {
  it('loads the four lists once and derives the tree from them', async () => {
    const { result } = renderHook(() => useLabLayout('lab-demo'), { wrapper: createWrapper() });

    await waitFor(() => {
      expect(result.current.isPending).toBe(false);
    });

    expect(requestsPerPath()).toEqual([1, 1, 1, 1]);
    expect(result.current.tree.map((node) => node.label)).toEqual(['Freezer A', 'Freezer B']);
    expect(result.current.tree[0]?.boxCount).toBe(2);
    expect(result.current.isError).toBe(false);
    expect(result.current.error).toBeNull();
  });

  it('shares one cache entry between consumers instead of refetching per screen', async () => {
    const { result } = renderHook(
      () => [useLabLayout('lab-demo'), useLabLayout('lab-demo')] as const,
      { wrapper: createWrapper() },
    );

    await waitFor(() => {
      expect(result.current[0].isPending).toBe(false);
    });

    expect(requestsPerPath()).toEqual([1, 1, 1, 1]);
    expect(result.current[0].tree).toEqual(result.current[1].tree);
  });

  it('exposes the raw lists as well, because the box view and the admin screen reuse them', async () => {
    const { result } = renderHook(() => useLabLayout('lab-demo'), { wrapper: createWrapper() });

    await waitFor(() => {
      expect(result.current.isPending).toBe(false);
    });

    // The archived rows are in the data — the list RPCs have no
    // `include_archived` field — and it is the tree that hides them.
    expect(result.current.boxes.map((box) => box.id)).toContain('box-old');
    expect(result.current.tree.flatMap((node) => node.id)).not.toContain('box-old');
    expect(result.current.storageContainers.length).toBeGreaterThan(0);
    expect(result.current.boxTypes.length).toBeGreaterThan(0);
  });

  it('resolves a location path from the same loaded data', async () => {
    const { result } = renderHook(() => useLabLayout('lab-demo'), { wrapper: createWrapper() });

    await waitFor(() => {
      expect(result.current.isPending).toBe(false);
    });

    const path = result.current.locationPath('box-3', 'A1');
    expect(path.segments.map((segment) => segment.label)).toEqual([
      'Freezer B',
      'Rack 2',
      'Shelf 1',
      'Box C',
      'A1',
    ]);
    // The helper is safe to call before the data arrives, which a screen does
    // on its first render.
    expect(result.current.locationPath('', '')).toEqual({
      placed: false,
      partial: false,
      segments: [],
    });
  });

  it('reports a failure of the second resource as an error, not as a partial tree', async () => {
    // "Partway through loading" is the point: the freezer list answers, the
    // container list does not.
    server.use(...fakeApi({ fail: { 'storage-container/list': 'INTERNAL' } }));

    const { result } = renderHook(() => useLabLayout('lab-demo'), { wrapper: createWrapper() });

    await waitFor(() => {
      expect(result.current.isError).toBe(true);
    });

    expect(result.current.error).toBeInstanceOf(ApiError);
    expect((result.current.error as ApiError).code).toBe('INTERNAL');
    expect(result.current.isPending).toBe(false);

    // The hook hands back whatever loaded — the freezers are there, with no
    // containers under them — and says so with `isError`. Rendering the tree is
    // therefore the caller's decision, and the screen renders the error state
    // instead; `LayoutTreeScreen.test.tsx` asserts that, because ten empty
    // drawers would look exactly like a lab that has none.
    expect(result.current.tree.map((node) => node.id)).toEqual(['fz-front', 'fz-back']);
    expect(result.current.tree.every((node) => node.children.length === 0)).toBe(true);
    expect(result.current.storageContainers).toEqual([]);
  });

  it('surfaces PERMISSION_DENIED, the branch a Member hits on freezer/list today', async () => {
    server.use(...fakeApi({ fail: { 'freezer/list': 'PERMISSION_DENIED' } }));

    const { result } = renderHook(() => useLabLayout('lab-demo'), { wrapper: createWrapper() });

    await waitFor(() => {
      expect(result.current.isError).toBe(true);
    });

    expect((result.current.error as ApiError).code).toBe('PERMISSION_DENIED');
  });

  it('retries all four resources, so a screen can recover without a reload', async () => {
    server.use(...fakeApi({ fail: { 'freezer/list': 'UNAVAILABLE' } }));
    const { result } = renderHook(() => useLabLayout('lab-demo'), { wrapper: createWrapper() });

    await waitFor(() => {
      expect(result.current.isError).toBe(true);
    });

    server.use(...fakeApi({ lab }));
    await act(async () => {
      await result.current.refetch();
    });

    await waitFor(() => {
      expect(result.current.isError).toBe(false);
    });
    expect(result.current.tree.map((node) => node.label)).toEqual(['Freezer A', 'Freezer B']);
  });

  it('does not call the server at all until it has a lab id', () => {
    const { result } = renderHook(() => useLabLayout(''), { wrapper: createWrapper() });

    expect(result.current.isPending).toBe(true);
    expect(requested).toEqual([]);
  });
});
