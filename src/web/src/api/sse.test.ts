// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { SampleSchema } from '../gen/fmgr/v1/sample_pb';
import { FakeEventSource, fakeEventSource } from '../test/fakeEventSource';
import { ApiError } from './errors';
import { reconnectDelayMs, subscribeSse, type SseFrame } from './sse';

/**
 * `src/api/sse.ts` (TODO.md G1.2). jsdom has no `EventSource` at all, so the
 * module takes a factory — which is also what makes the reconnect and cleanup
 * branches testable without a server. The double is shared with the rest of the
 * suite (`src/test/fakeEventSource.ts`), so a component test drives the same
 * object these unit tests do.
 */

const factory = fakeEventSource;

/** The fake the module is currently talking to. */
const current = (): FakeEventSource => FakeEventSource.current();

beforeEach(() => {
  FakeEventSource.reset();
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
});

describe('reconnectDelayMs', () => {
  it('doubles the delay per attempt and stops at the cap', () => {
    expect(reconnectDelayMs(0, 1_000, 30_000)).toBe(1_000);
    expect(reconnectDelayMs(1, 1_000, 30_000)).toBe(2_000);
    expect(reconnectDelayMs(2, 1_000, 30_000)).toBe(4_000);
    expect(reconnectDelayMs(5, 1_000, 30_000)).toBe(30_000);
    expect(reconnectDelayMs(50, 1_000, 30_000)).toBe(30_000);
  });
});

describe('subscribeSse', () => {
  it('subscribes to the route path with the query parameters it was given', () => {
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      params: { lab_id: 'lab-1', box_id: 'box-9' },
      onFrame: vi.fn(),
      eventSourceFactory: factory,
    });

    expect(current().url).toBe('/api/v1/sample/watch?lab_id=lab-1&box_id=box-9');
  });

  it('rejects an unknown query parameter instead of sending it', () => {
    expect(() =>
      subscribeSse('sample/watch', {
        schema: SampleSchema,
        // @ts-expect-error — the whole point: this is not a filter this route takes.
        params: { labd_id: 'lab-1' },
        onFrame: vi.fn(),
        eventSourceFactory: factory,
      }),
    ).toThrow(/unknown parameter/i);
  });

  // G0.1 removed the gateway's `?access_token=` fallback, because a token in a
  // URL ends up in proxy and access logs. The credential is now the HttpOnly
  // session cookie the browser attaches to a same-origin `EventSource`, so no
  // URL this module builds may carry one — on the first connect or on any
  // reconnect, which rebuilds the URL from the cursor.
  it('never puts a credential in the stream URL', () => {
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      params: { lab_id: 'lab-1' },
      onFrame: vi.fn(),
      onError: vi.fn(),
      eventSourceFactory: factory,
    });
    current().message(JSON.stringify({ id: 's-1', lab_id: 'lab-1' }), '1758931200000000');
    current().transportError();
    vi.advanceTimersByTime(1_000);

    expect(FakeEventSource.all()).toHaveLength(2);
    for (const source of FakeEventSource.all()) {
      expect(source.url).not.toContain('access_token');
      expect(source.url).not.toContain('token');
      expect(source.url).not.toContain('Bearer');
      expect(source.url).not.toContain('authorization');
    }
    expect(current().url).toBe('/api/v1/sample/watch?lab_id=lab-1&since=1758931200000000');
  });

  it('parses a frame into the generated message type and reports its cursor', () => {
    const frames: SseFrame<unknown>[] = [];
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: (frame) => frames.push(frame),
      eventSourceFactory: factory,
    });

    current().message(
      JSON.stringify({
        id: 's-1',
        lab_id: 'lab-1',
        name: 'Serum A',
        status: 'SAMPLE_STATUS_ACTIVE',
      }),
      '1758931200000000',
    );

    expect(frames).toHaveLength(1);
    expect(frames[0]?.id).toBe('1758931200000000');
    expect(frames[0]?.event).toBe('message');
    expect((frames[0]?.data as { name: string }).name).toBe('Serum A');
  });

  it('surfaces an `event: error` frame as an ApiError carrying the server code', () => {
    const onError = vi.fn();
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: vi.fn(),
      onError,
      eventSourceFactory: factory,
    });

    current().serverError(JSON.stringify({ code: 'PERMISSION_DENIED', message: 'no sample.read' }));

    expect(onError).toHaveBeenCalledTimes(1);
    const error = onError.mock.calls[0]?.[0] as ApiError;
    expect(error).toBeInstanceOf(ApiError);
    expect(error.code).toBe('PERMISSION_DENIED');
    expect(error.message).toBe('no sample.read');
  });

  it('does not reconnect after a PERMISSION_DENIED error frame', () => {
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: vi.fn(),
      onError: vi.fn(),
      eventSourceFactory: factory,
    });

    current().serverError(JSON.stringify({ code: 'PERMISSION_DENIED', message: 'nope' }));
    vi.advanceTimersByTime(60_000);

    expect(FakeEventSource.all()).toHaveLength(1);
    expect(current().closed).toBe(true);
  });

  it('reconnects after a transient error frame', () => {
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: vi.fn(),
      onError: vi.fn(),
      eventSourceFactory: factory,
      reconnect: { baseDelayMs: 100, maxDelayMs: 1_000 },
    });

    current().serverError(JSON.stringify({ code: 'UNAVAILABLE', message: 'restarting' }));
    vi.advanceTimersByTime(99);
    expect(FakeEventSource.all()).toHaveLength(1);
    vi.advanceTimersByTime(1);

    expect(FakeEventSource.all()).toHaveLength(2);
  });

  it('reports a malformed frame as ApiError(INTERNAL) and keeps the stream open', () => {
    const onError = vi.fn();
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: vi.fn(),
      onError,
      eventSourceFactory: factory,
    });

    current().message('{not json');

    expect((onError.mock.calls[0]?.[0] as ApiError).code).toBe('INTERNAL');
    expect(FakeEventSource.all()).toHaveLength(1);
    expect(current().closed).toBe(false);
  });

  it('reconnects after a transport error, with capped exponential backoff', () => {
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: vi.fn(),
      onError: vi.fn(),
      eventSourceFactory: factory,
      reconnect: { baseDelayMs: 1_000, maxDelayMs: 3_000 },
    });

    const delays: number[] = [];
    for (let attempt = 0; attempt < 4; attempt += 1) {
      current().transportError();
      let elapsed = 0;
      while (FakeEventSource.all().length === attempt + 1 && elapsed < 10_000) {
        vi.advanceTimersByTime(100);
        elapsed += 100;
      }
      delays.push(elapsed);
    }

    expect(delays).toEqual([1_000, 2_000, 3_000, 3_000]);
  });

  it('reports the lost connection as ApiError(UNAVAILABLE)', () => {
    const onError = vi.fn();
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: vi.fn(),
      onError,
      eventSourceFactory: factory,
    });

    current().transportError();

    expect((onError.mock.calls[0]?.[0] as ApiError).code).toBe('UNAVAILABLE');
  });

  it('resumes from the last event id by sending it as ?since on reconnect', () => {
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      params: { lab_id: 'lab-1' },
      onFrame: vi.fn(),
      onError: vi.fn(),
      eventSourceFactory: factory,
      reconnect: { baseDelayMs: 10, maxDelayMs: 10 },
    });

    current().message(JSON.stringify({ id: 's-1', lab_id: 'lab-1' }), '4242');
    current().transportError();
    vi.advanceTimersByTime(10);

    expect(current().url).toBe('/api/v1/sample/watch?lab_id=lab-1&since=4242');
  });

  it('resets the backoff after a successful open', () => {
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: vi.fn(),
      onError: vi.fn(),
      eventSourceFactory: factory,
      reconnect: { baseDelayMs: 1_000, maxDelayMs: 30_000 },
    });

    current().transportError();
    vi.advanceTimersByTime(1_000);
    current().open();
    current().transportError();
    vi.advanceTimersByTime(1_000);

    expect(FakeEventSource.all()).toHaveLength(3);
  });

  it('calls onOpen when the stream connects', () => {
    const onOpen = vi.fn();
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: vi.fn(),
      onOpen,
      eventSourceFactory: factory,
    });

    current().open();

    expect(onOpen).toHaveBeenCalledTimes(1);
  });

  it('closes the stream and cancels a pending reconnect on unsubscribe', () => {
    const onFrame = vi.fn();
    const unsubscribe = subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame,
      onError: vi.fn(),
      eventSourceFactory: factory,
      reconnect: { baseDelayMs: 50, maxDelayMs: 50 },
    });

    current().transportError();
    unsubscribe();
    vi.advanceTimersByTime(10_000);

    expect(FakeEventSource.all()).toHaveLength(1);
    expect(current().closed).toBe(true);
    expect(onFrame).not.toHaveBeenCalled();
  });

  it('ignores frames that arrive after unsubscribe', () => {
    const onFrame = vi.fn();
    const source = (() => {
      const unsubscribe = subscribeSse('sample/watch', {
        schema: SampleSchema,
        onFrame,
        eventSourceFactory: factory,
      });
      const created = current();
      unsubscribe();
      return created;
    })();

    source.message(JSON.stringify({ id: 's-1' }));

    expect(onFrame).not.toHaveBeenCalled();
  });

  it('uses a generated message as the schema source of truth', () => {
    const frames: unknown[] = [];
    subscribeSse('sample/watch', {
      schema: SampleSchema,
      onFrame: (frame) => frames.push(frame.data),
      eventSourceFactory: factory,
    });

    current().message(
      JSON.stringify(create(SampleSchema, { id: 's-1', labId: 'lab-1', name: 'Serum A' })),
    );

    expect(frames[0]).toMatchObject({ id: 's-1', labId: 'lab-1', name: 'Serum A' });
  });
});
