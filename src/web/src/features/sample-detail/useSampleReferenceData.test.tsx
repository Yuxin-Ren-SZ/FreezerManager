// SPDX-License-Identifier: AGPL-3.0-or-later
import { renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it } from 'vitest';
import { useCan, useSession } from '../../app/session';
import type { PermissionKey } from '../../app/permissions';
import { createDemoLab, fakeApi, type DemoLab } from '../../test/fakeApi';
import { createWrapper } from '../../test/render';
import { server } from '../../test/server';
import { currentUserWith } from '../../test/session';
import { useSampleReferenceData } from './useSampleReferenceData';

/**
 * The permission gate on the reference data a generated sample form is built
 * from (TODO.md G3.3, issue #76).
 *
 * The gate has to agree with what the **server** enforces on
 * `custom-field-def/list`. Since #69 that is `sample.read`, not
 * `custom_field.define` — so a Member, who holds the former and not the latter,
 * is served the definitions. The reverse mistake is just as bad: a caller
 * without `sample.read` must still get nothing, or the "fix" is "everyone sees
 * everything".
 *
 * The fake cannot catch a wrong gate on its own: `fakeApi` answers every route
 * it is given and models no permissions at all. These tests are therefore about
 * *whether the request is made*, read through the data that can only be there
 * if it was.
 */

const LAB_ID = 'lab-demo';

let lab: DemoLab;

beforeEach(() => {
  lab = createDemoLab();
  server.use(...fakeApi({ lab }));
});

/**
 * The hook, plus the session facts an assertion needs to be unambiguous.
 *
 * `useSession().status` matters because `SessionProvider` starts with a null
 * user: until the loader resolves, `can()` answers `false` for every permission
 * and a negative assertion would pass for the wrong reason.
 */
function renderReference(permissions: readonly PermissionKey[]) {
  return renderHook(
    () => ({
      reference: useSampleReferenceData(LAB_ID),
      session: useSession(),
      // Read through the same `useCan` the gate uses, so the test states the
      // user's permissions in the terms the gate is written in rather than
      // restating the fixture.
      canReadSamples: useCan('sample.read', LAB_ID),
      canDefineFields: useCan('custom_field.define', LAB_ID),
    }),
    { wrapper: createWrapper({ user: currentUserWith(permissions) }) },
  );
}

describe('useSampleReferenceData', () => {
  it('reads the definitions for a member who holds sample.read but not custom_field.define', async () => {
    const { result } = renderReference(['sample.read']);

    await waitFor(() => {
      expect(result.current.session.status).toBe('authenticated');
    });
    await waitFor(() => {
      expect(result.current.reference.isPending).toBe(false);
    });

    expect(result.current.canReadSamples).toBe(true);
    expect(result.current.canDefineFields).toBe(false);

    // The definitions are the point: a Member's generated form is empty without
    // them, which is the defect #76 exists to close.
    expect(result.current.reference.definitionsReadable).toBe(true);
    expect(result.current.reference.cfds.map((cfd) => cfd.key)).toEqual(
      expect.arrayContaining(['concentration', 'freeze_thaw_count', 'storage_note']),
    );
  });

  it('leaves the definitions unread for a caller who holds custom_field.define but not sample.read', async () => {
    const { result } = renderReference(['custom_field.define']);

    await waitFor(() => {
      expect(result.current.session.status).toBe('authenticated');
    });
    await waitFor(() => {
      expect(result.current.reference.isPending).toBe(false);
    });

    expect(result.current.canDefineFields).toBe(true);
    expect(result.current.canReadSamples).toBe(false);

    // The fake would have answered `custom-field-def/list` with the seeded
    // definitions had it been asked — it models no permissions — so an empty
    // list here means the request was never sent.
    expect(result.current.reference.definitionsReadable).toBe(false);
    expect(result.current.reference.cfds).toEqual([]);
  });
});
