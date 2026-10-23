// SPDX-License-Identifier: AGPL-3.0-or-later

const CHANNEL_NAME = 'fmgr-session-change-v1';
const STORAGE_KEY = 'fmgr-session-change-v1';
const SIGNAL = 'session-changed';

type SessionChangeListener = () => void;

let sessionChangeVersion = 0;
let channel: BroadcastChannel | null = null;
const listeners = new Set<SessionChangeListener>();
let storageListenerInstalled = false;
const seenSignals = new Set<string>();

function receiveSessionChange(signalId: string): void {
  if (seenSignals.has(signalId)) return;
  seenSignals.add(signalId);
  if (seenSignals.size > 128) {
    const oldest = seenSignals.values().next().value;
    if (oldest !== undefined) seenSignals.delete(oldest);
  }
  sessionChangeVersion += 1;
  for (const listener of listeners) listener();
}

function openChannel(): BroadcastChannel | null {
  if (channel !== null) return channel;
  if (typeof BroadcastChannel === 'undefined') return null;
  try {
    channel = new BroadcastChannel(CHANNEL_NAME);
    channel.onmessage = (event: MessageEvent<unknown>) => {
      const message = event.data as { type?: unknown; id?: unknown } | null;
      if (message?.type === SIGNAL && typeof message.id === 'string') {
        receiveSessionChange(message.id);
      }
    };
    return channel;
  } catch {
    return null;
  }
}

function onStorage(event: StorageEvent): void {
  if (event.key === STORAGE_KEY && event.newValue !== null) {
    receiveSessionChange(event.newValue);
  }
}

/** A local epoch lets the transport ignore a 401 from a request sent pre-change. */
export function getSessionChangeVersion(): number {
  return sessionChangeVersion;
}

/**
 * Notify other same-origin tabs that the shared HttpOnly cookie may have
 * changed. The message is a fixed, non-sensitive signal with no identity or
 * credential data. BroadcastChannel is preferred; storage events are the
 * fallback for browsers or contexts where it cannot be opened.
 */
export function announceSessionChange(): void {
  sessionChangeVersion += 1;
  const signalId = `${String(Date.now())}-${String(Math.random())}`;
  const activeChannel = openChannel();
  if (activeChannel !== null) {
    try {
      activeChannel.postMessage({ type: SIGNAL, id: signalId });
      return;
    } catch {
      // Storage events below are the fallback when posting is disabled.
    }
  }

  try {
    window.localStorage.setItem(STORAGE_KEY, signalId);
  } catch {
    // A blocked storage area means this tab cannot notify peers. The local
    // auth flow still proceeds; no credential is persisted as a workaround.
  }
}

/** Listen for a peer tab's fixed session-change signal. */
export function subscribeToSessionChanges(listener: SessionChangeListener): () => void {
  listeners.add(listener);
  openChannel();
  if (!storageListenerInstalled) {
    window.addEventListener('storage', onStorage);
    storageListenerInstalled = true;
  }
  return () => {
    listeners.delete(listener);
    if (listeners.size === 0) {
      channel?.close();
      channel = null;
      if (storageListenerInstalled) {
        window.removeEventListener('storage', onStorage);
        storageListenerInstalled = false;
      }
    }
  };
}
