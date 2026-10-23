// SPDX-License-Identifier: AGPL-3.0-or-later
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const CHANNEL_NAME = 'fmgr-session-change-v1';
const STORAGE_KEY = 'fmgr-session-change-v1';
const SIGNAL = 'session-changed';

type MessageHandler = ((event: MessageEvent<unknown>) => void) | null;

function broadcastChannelClass(options: { throwOnPost?: boolean } = {}) {
  class TestBroadcastChannel {
    static instances: TestBroadcastChannel[] = [];
    static sentMessages: unknown[] = [];

    onmessage: MessageHandler = null;
    closed = false;

    constructor(readonly name: string) {
      TestBroadcastChannel.instances.push(this);
    }

    postMessage(data: unknown): void {
      if (options.throwOnPost) throw new Error('postMessage blocked');
      TestBroadcastChannel.sentMessages.push(data);
      for (const peer of TestBroadcastChannel.instances) {
        if (peer !== this && !peer.closed && peer.name === this.name) {
          peer.onmessage?.({ data } as MessageEvent<unknown>);
        }
      }
    }

    close(): void {
      this.closed = true;
    }
  }

  return TestBroadcastChannel;
}

async function loadFreshHelper(channelConstructor: unknown) {
  vi.resetModules();
  vi.stubGlobal('BroadcastChannel', channelConstructor);
  return import('./sessionChange');
}

function dispatchStorageSignal(id: string): void {
  window.dispatchEvent(new StorageEvent('storage', { key: STORAGE_KEY, newValue: id }));
}

beforeEach(() => {
  window.localStorage.clear();
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('session-change transport', () => {
  it('uses storage when a peer cannot open BroadcastChannel even if the sender can', async () => {
    const receiver = await loadFreshHelper(undefined);
    const onChange = vi.fn();
    receiver.subscribeToSessionChanges(onChange);

    const BroadcastChannel = broadcastChannelClass();
    const sender = await loadFreshHelper(BroadcastChannel);
    const originalSetItem = Storage.prototype.setItem.bind(window.localStorage);
    const setItem = vi.spyOn(Storage.prototype, 'setItem').mockImplementation((key, value) => {
      originalSetItem(key, value);
      dispatchStorageSignal(value);
    });

    sender.announceSessionChange();

    expect(setItem).toHaveBeenCalledOnce();
    expect(setItem.mock.calls[0]?.[0]).toBe(STORAGE_KEY);
    expect(onChange).toHaveBeenCalledOnce();
  });

  it('falls back to localStorage when BroadcastChannel is unavailable', async () => {
    const helper = await loadFreshHelper(undefined);
    expect(typeof BroadcastChannel).toBe('undefined');
    const setItem = vi.spyOn(Storage.prototype, 'setItem');

    expect(() => {
      helper.announceSessionChange();
    }).not.toThrow();

    expect(setItem).toHaveBeenCalledOnce();
    expect(setItem.mock.calls[0]?.[0]).toBe(STORAGE_KEY);
    expect(setItem.mock.calls[0]?.[1]).toMatch(/^\d+-\d+\.\d+$/);
  });

  it('falls back when channel construction throws', async () => {
    class BlockedBroadcastChannel extends Error {
      constructor() {
        super('channel blocked');
        throw this;
      }

      postMessage(): void {
        throw new Error('unreachable');
      }

      close(): void {
        throw new Error('unreachable');
      }
    }
    const helper = await loadFreshHelper(BlockedBroadcastChannel);
    const setItem = vi.spyOn(Storage.prototype, 'setItem');

    expect(() => {
      helper.announceSessionChange();
    }).not.toThrow();

    expect(setItem).toHaveBeenCalledOnce();
    expect(setItem.mock.calls[0]?.[0]).toBe(STORAGE_KEY);
  });

  it('falls back when postMessage throws', async () => {
    const BroadcastChannel = broadcastChannelClass({ throwOnPost: true });
    const helper = await loadFreshHelper(BroadcastChannel);
    const setItem = vi.spyOn(Storage.prototype, 'setItem');

    expect(() => {
      helper.announceSessionChange();
    }).not.toThrow();

    expect(setItem).toHaveBeenCalledOnce();
    expect(setItem.mock.calls[0]?.[0]).toBe(STORAGE_KEY);
  });

  it('does not echo a sender notification back to its own listener', async () => {
    const BroadcastChannel = broadcastChannelClass();
    const helper = await loadFreshHelper(BroadcastChannel);
    const onChange = vi.fn();
    helper.subscribeToSessionChanges(onChange);

    helper.announceSessionChange();

    expect(BroadcastChannel.instances[0]?.name).toBe(CHANNEL_NAME);
    expect(BroadcastChannel.sentMessages[0]).toMatchObject({ type: SIGNAL });
    expect(onChange).not.toHaveBeenCalled();
  });

  it('deduplicates one notification delivered through both transports', async () => {
    const BroadcastChannel = broadcastChannelClass();
    const receiver = await loadFreshHelper(BroadcastChannel);
    const onChange = vi.fn();
    receiver.subscribeToSessionChanges(onChange);
    const sender = await loadFreshHelper(BroadcastChannel);
    const originalSetItem = Storage.prototype.setItem.bind(window.localStorage);
    vi.spyOn(Storage.prototype, 'setItem').mockImplementation((key, value) => {
      originalSetItem(key, value);
      dispatchStorageSignal(value);
    });

    sender.announceSessionChange();

    expect(onChange).toHaveBeenCalledOnce();
  });

  it('still notifies through BroadcastChannel when localStorage writes fail', async () => {
    const BroadcastChannel = broadcastChannelClass();
    const receiver = await loadFreshHelper(BroadcastChannel);
    const onChange = vi.fn();
    receiver.subscribeToSessionChanges(onChange);
    const sender = await loadFreshHelper(BroadcastChannel);
    vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
      throw new Error('storage blocked');
    });

    expect(() => {
      sender.announceSessionChange();
    }).not.toThrow();
    expect(onChange).toHaveBeenCalledOnce();
  });

  it('removes its storage listener when the last subscriber leaves', async () => {
    const helper = await loadFreshHelper(undefined);
    const onChange = vi.fn();
    const remove = vi.spyOn(window, 'removeEventListener');
    const unsubscribe = helper.subscribeToSessionChanges(onChange);

    unsubscribe();
    dispatchStorageSignal('after-unsubscribe');

    expect(onChange).not.toHaveBeenCalled();
    expect(remove).toHaveBeenCalledWith('storage', expect.any(Function));
  });
});
