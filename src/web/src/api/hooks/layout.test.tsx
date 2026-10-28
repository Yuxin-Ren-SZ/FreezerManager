// SPDX-License-Identifier: AGPL-3.0-or-later
import { renderHook, waitFor } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { createDemoLab, fakeApi } from '../../test/fakeApi';
import { createWrapper } from '../../test/render';
import { server } from '../../test/server';
import { useFreezerModels, useFreezerScene } from './layout';

/**
 * The 3D layout reads (ADR 0003 §3): one call for a freezer's scene, one for
 * the model catalogue. Both are lab-scoped keys, so a lab switch never shows
 * another lab's freezer.
 */
describe('useFreezerScene', () => {
  it("loads a freezer's scene", async () => {
    server.use(...fakeApi({ lab: createDemoLab() }));
    const { result } = renderHook(() => useFreezerScene('lab-demo', 'fz-front'), {
      wrapper: createWrapper(),
    });
    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });
    expect(result.current.data?.freezer?.id).toBe('fz-front');
    expect(result.current.data?.nodes.length).toBeGreaterThan(0);
  });

  it('does not ask before a freezer is chosen', () => {
    server.use(...fakeApi({ lab: createDemoLab() }));
    const { result } = renderHook(() => useFreezerScene('lab-demo', ''), {
      wrapper: createWrapper(),
    });
    expect(result.current.fetchStatus).toBe('idle');
  });

  it('surfaces a refusal as an error, not an empty freezer', async () => {
    server.use(
      ...fakeApi({ lab: createDemoLab(), fail: { 'freezer/scene': 'PERMISSION_DENIED' } }),
    );
    const { result } = renderHook(() => useFreezerScene('lab-demo', 'fz-front'), {
      wrapper: createWrapper(),
    });
    await waitFor(() => {
      expect(result.current.isError).toBe(true);
    });
  });
});

describe('useFreezerModels', () => {
  it('lists the built-in catalogue', async () => {
    server.use(...fakeApi({ lab: createDemoLab() }));
    const { result } = renderHook(() => useFreezerModels('lab-demo'), {
      wrapper: createWrapper(),
    });
    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });
    expect(result.current.data?.models.map((model) => model.renderKey)).toEqual([
      'cryocube-f740',
      'tsx-40086',
    ]);
  });
});
