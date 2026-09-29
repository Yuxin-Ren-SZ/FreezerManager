// SPDX-License-Identifier: AGPL-3.0-or-later
import { renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it } from 'vitest';
import { ApiError } from '../errors';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { createWrapper } from '../../test/render';
import { server } from '../../test/server';
import { useCustomFieldDefinitions } from './labs';

/**
 * The custom-field definitions behind the G3.2 column chooser.
 *
 * `custom-field-def/list` needs `custom_field.define`, which a read-only member
 * does not hold — so this hook has to be *switchable* and its failure has to
 * arrive as an ordinary `ApiError` the screen can degrade on, not as an
 * exception or a retry loop.
 */

let lab: DemoLab;

beforeEach(() => {
  lab = createDemoLab();
  server.use(...fakeApi({ lab }));
});

describe('useCustomFieldDefinitions', () => {
  it('loads the lab definitions', async () => {
    const { result } = renderHook(() => useCustomFieldDefinitions('lab-demo'), {
      wrapper: createWrapper(),
    });

    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });

    expect(result.current.data?.cfds.map((cfd) => cfd.key)).toEqual([
      'concentration',
      'freeze_thaw_count',
      'storage_note',
    ]);
  });

  it('asks for one item type when given one', async () => {
    const { result } = renderHook(
      () => useCustomFieldDefinitions('lab-demo', { itemTypeId: 'it-plasma' }),
      { wrapper: createWrapper() },
    );

    await waitFor(() => {
      expect(result.current.isSuccess).toBe(true);
    });

    // A lab-scoped definition is not part of an item-type query: the server
    // compares `item_type_id` for equality.
    expect(result.current.data?.cfds.map((cfd) => cfd.key)).toEqual(['freeze_thaw_count']);
  });

  it('stays idle when disabled, so a member without the permission never asks', () => {
    const { result } = renderHook(
      () => useCustomFieldDefinitions('lab-demo', { enabled: false }),
      { wrapper: createWrapper() },
    );

    expect(result.current.fetchStatus).toBe('idle');
    expect(result.current.data).toBeUndefined();
  });

  it('surfaces a refusal as an ApiError, so the screen can show the base columns', async () => {
    server.use(...fakeApi({ fail: { 'custom-field-def/list': 'PERMISSION_DENIED' } }));

    const { result } = renderHook(() => useCustomFieldDefinitions('lab-demo'), {
      wrapper: createWrapper(),
    });

    await waitFor(() => {
      expect(result.current.isError).toBe(true);
    });
    expect(result.current.error).toBeInstanceOf(ApiError);
    expect((result.current.error as ApiError).code).toBe('PERMISSION_DENIED');
  });
});
