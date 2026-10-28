// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import { CLOSED_FRAME, type SceneFrame } from './geometry';
import { initialSceneState, type SceneState } from './interaction';
import { settledFrame, stepFrame } from './useSceneFrame';

/**
 * The tween's ordering: parts close before others open, so two doors or a
 * rack and a shut door are never drawn through each other.
 */

const state = (overrides: Partial<SceneState>): SceneState => ({
  ...initialSceneState,
  ...overrides,
});

/** Run the tween to rest, recording every frame. */
function settle(from: SceneFrame, to: SceneState): SceneFrame[] {
  const frames = [from];
  for (let step = 0; step < 200; step += 1) {
    const next = stepFrame(frames[frames.length - 1], to, 0.25);
    frames.push(next);
    if (JSON.stringify(next) === JSON.stringify(settledFrame(to))) break;
  }
  return frames;
}

describe('stepFrame', () => {
  it('opens the door before the inner door and pulls the rack last', () => {
    const target = state({ doorOpen: true, innerDoor: 1, pulledRack: { level: 1, column: 2 } });
    const frames = settle(CLOSED_FRAME, target);
    expect(frames.at(-1)).toEqual(settledFrame(target));
    for (const frame of frames) {
      if (frame.inner > 0) expect(frame.door).toBe(1);
      if (frame.pull > 0) expect(frame.inner).toBe(1);
    }
  });

  it('pushes the rack back before the outer door closes', () => {
    const open = state({ doorOpen: true, innerDoor: 0, pulledRack: { level: 0, column: 0 } });
    const frames = settle(settledFrame(open), state({ doorOpen: false }));
    expect(frames.at(-1)).toEqual(CLOSED_FRAME);
    for (const frame of frames) {
      if (frame.door < 1) expect(frame.pull).toBe(0);
    }
  });

  it('shuts one inner door before opening the next', () => {
    const first = state({ doorOpen: true, innerDoor: 0 });
    const frames = settle(settledFrame(first), state({ doorOpen: true, innerDoor: 2 }));
    expect(frames.at(-1)?.innerDoor).toBe(2);
    const switched = frames.findIndex((frame) => frame.innerDoor === 2);
    expect(frames[switched - 1].inner).toBe(0);
  });
});
