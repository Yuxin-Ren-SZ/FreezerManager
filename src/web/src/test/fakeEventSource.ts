// SPDX-License-Identifier: AGPL-3.0-or-later
import type { EventSourceLike } from '../api/sse';

/**
 * A scriptable `EventSource` double (TODO.md G1.2). jsdom implements no
 * `EventSource` at all, so `subscribeSse` takes a factory and every test that
 * needs a live feed supplies one of these.
 *
 * The three "server" methods mirror the three things a real stream can do:
 * `open()`, `message()` for a data frame and `serverError()` for the gateway's
 * `event: error` frame. `transportError()` is the fourth: the connection
 * dropping, which arrives as a plain `Event` with no `data` — the distinction
 * `src/api/sse.ts` relies on.
 */
export class FakeEventSource implements EventSourceLike {
  static instances: FakeEventSource[] = [];

  /** Every instance created since the last `reset()`. */
  static all(): FakeEventSource[] {
    return FakeEventSource.instances;
  }

  /** The most recently created instance, i.e. the one now in use. */
  static current(): FakeEventSource {
    const source = FakeEventSource.instances.at(-1);
    if (source === undefined) {
      throw new Error('no FakeEventSource has been created');
    }
    return source;
  }

  static reset(): void {
    FakeEventSource.instances = [];
  }

  readonly url: string;
  closed = false;
  private readonly listeners = new Map<string, Set<(event: Event) => void>>();

  constructor(url: string) {
    this.url = url;
    FakeEventSource.instances.push(this);
  }

  addEventListener(type: string, listener: (event: Event) => void): void {
    const set = this.listeners.get(type) ?? new Set<(event: Event) => void>();
    set.add(listener);
    this.listeners.set(type, set);
  }

  removeEventListener(type: string, listener: (event: Event) => void): void {
    this.listeners.get(type)?.delete(listener);
  }

  close(): void {
    this.closed = true;
  }

  /** The connection came up (fires again after every reconnect). */
  open(): void {
    this.dispatch('open', new Event('open'));
  }

  /** A data frame. `lastEventId` is the gateway's `id:` line. */
  message(payload: unknown, lastEventId = ''): void {
    const data = typeof payload === 'string' ? payload : JSON.stringify(payload);
    this.dispatch('message', new MessageEvent('message', { data, lastEventId }));
  }

  /** The gateway's `event: error` frame, carrying a `{code, message}` body. */
  serverError(payload: { code: string; message: string } | string): void {
    const data = typeof payload === 'string' ? payload : JSON.stringify(payload);
    this.dispatch('error', new MessageEvent('error', { data }));
  }

  /** The connection dropped: a plain `Event`, no `data`. */
  transportError(): void {
    this.dispatch('error', new Event('error'));
  }

  private dispatch(type: string, event: Event): void {
    for (const listener of [...(this.listeners.get(type) ?? [])]) {
      listener(event);
    }
  }
}

/** The factory `subscribeSse` takes, wired to the shared instance list. */
export function fakeEventSource(url: string): EventSourceLike {
  return new FakeEventSource(url);
}
