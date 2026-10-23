// SPDX-License-Identifier: AGPL-3.0-or-later
import { beforeEach, describe, expect, it, vi } from 'vitest';

const mocks = vi.hoisted(() => ({
  call: vi.fn(),
  announce: vi.fn(),
}));

vi.mock('./client', () => ({ call: mocks.call }));
vi.mock('../app/sessionChange', () => ({ announceSessionChange: mocks.announce }));

import { signIn, signOutOfBrowserSession, submitMfaCode } from './auth';

describe('browser auth session notifications', () => {
  beforeEach(() => {
    mocks.call.mockReset();
    mocks.announce.mockReset();
  });

  it('announces a successful password login while MFA is still pending', async () => {
    mocks.call.mockResolvedValue({ mfaRequired: true });

    await expect(signIn('person@example.test', 'password')).resolves.toEqual({ mfaRequired: true });

    expect(mocks.announce).toHaveBeenCalledOnce();
  });

  it('announces successful MFA completion and logout', async () => {
    mocks.call.mockResolvedValue({});

    await submitMfaCode('123456');
    await signOutOfBrowserSession();

    expect(mocks.announce).toHaveBeenCalledTimes(2);
  });

  it('does not announce a rejected auth request', async () => {
    mocks.call.mockRejectedValue(new Error('refused'));

    await expect(signIn('person@example.test', 'wrong')).rejects.toThrow('refused');
    await expect(submitMfaCode('000000')).rejects.toThrow('refused');
    await expect(signOutOfBrowserSession()).rejects.toThrow('refused');

    expect(mocks.announce).not.toHaveBeenCalled();
  });
});
