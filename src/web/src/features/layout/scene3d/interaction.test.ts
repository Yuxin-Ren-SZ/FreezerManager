// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import {
  homeCamera,
  initialSceneState,
  innerDoorIndexFor,
  reduceScene,
  type SceneState,
} from './interaction';

/**
 * What a click on the freezer does (ADR 0003 §5): open the door, open a
 * compartment's inner door, pull a rack. The rules are the demo's: reaching in
 * opens whatever is in the way, and closing puts things back in reverse order.
 */

const F740_DOORS = [
  { firstLevel: 0, lastLevel: 0 },
  { firstLevel: 1, lastLevel: 1 },
  { firstLevel: 2, lastLevel: 2 },
];
// The TSX: two inner doors, each over two levels.
const TSX_DOORS = [
  { firstLevel: 0, lastLevel: 1 },
  { firstLevel: 2, lastLevel: 3 },
];

const run = (doors: typeof F740_DOORS, ...actions: Parameters<typeof reduceScene>[2][]) =>
  actions.reduce<SceneState>(
    (state, action) => reduceScene(doors, state, action),
    initialSceneState,
  );

describe('reduceScene', () => {
  it('starts closed, from the front', () => {
    expect(initialSceneState.doorOpen).toBe(false);
    expect(initialSceneState.innerDoor).toBeNull();
    expect(initialSceneState.pulledRack).toBeNull();
  });

  it('opens the outer door before an inner door', () => {
    const state = run(F740_DOORS, { type: 'openInnerDoor', index: 1 });
    expect(state.doorOpen).toBe(true);
    expect(state.innerDoor).toBe(1);
  });

  it('keeps one inner door open at a time', () => {
    const state = run(
      F740_DOORS,
      { type: 'openInnerDoor', index: 0 },
      { type: 'openInnerDoor', index: 2 },
    );
    expect(state.innerDoor).toBe(2);
  });

  it('opens the door and the inner door covering the level when a rack is pulled', () => {
    const state = run(TSX_DOORS, { type: 'pullRack', level: 3, column: 1 });
    expect(state.doorOpen).toBe(true);
    expect(state.innerDoor).toBe(1);
    expect(state.pulledRack).toEqual({ level: 3, column: 1 });
  });

  it('pulls a rack on a model without inner doors with the outer door alone', () => {
    const state = run([], { type: 'pullRack', level: 0, column: 0 });
    expect(state.doorOpen).toBe(true);
    expect(state.innerDoor).toBeNull();
  });

  it('pushes a rack back when its inner door closes', () => {
    const state = run(
      F740_DOORS,
      { type: 'pullRack', level: 1, column: 3 },
      { type: 'closeInnerDoor' },
    );
    expect(state.pulledRack).toBeNull();
    expect(state.doorOpen).toBe(true);
  });

  it('pushes a rack back when another inner door is opened', () => {
    const state = run(
      F740_DOORS,
      { type: 'pullRack', level: 1, column: 3 },
      { type: 'openInnerDoor', index: 0 },
    );
    expect(state.pulledRack).toBeNull();
  });

  it('keeps a rack out when the inner door opened covers its level too', () => {
    const state = run(
      TSX_DOORS,
      { type: 'pullRack', level: 2, column: 0 },
      { type: 'openInnerDoor', index: 1 },
    );
    expect(state.pulledRack).toEqual({ level: 2, column: 0 });
  });

  it('closes everything in reverse when the outer door closes', () => {
    const state = run(F740_DOORS, { type: 'pullRack', level: 2, column: 5 }, { type: 'closeDoor' });
    expect(state).toMatchObject({ doorOpen: false, innerDoor: null, pulledRack: null });
  });

  it('toggles a rack: pulling the pulled rack pushes it back', () => {
    const state = run(
      F740_DOORS,
      { type: 'pullRack', level: 0, column: 0 },
      { type: 'pullRack', level: 0, column: 0 },
    );
    expect(state.pulledRack).toBeNull();
    expect(state.innerDoor).toBe(0);
  });

  it('turns the camera within its limits and resets to the front view', () => {
    const turned = run(F740_DOORS, { type: 'rotate', dYaw: 500, dPitch: -500 });
    expect(turned.camera).toEqual({ yaw: 75, pitch: 5 });
    const reset = reduceScene(F740_DOORS, turned, { type: 'resetView' });
    expect(reset.camera).toEqual(initialSceneState.camera);
  });
});

describe('homeCamera', () => {
  it('looks from the side away from the hinge, so an open door does not hide the inside', () => {
    expect(homeCamera('right').yaw).toBeLessThan(0);
    expect(homeCamera('left').yaw).toBeGreaterThan(0);
  });

  it('is where Front view returns to', () => {
    const turned = reduceScene([], initialSceneState, { type: 'rotate', dYaw: 40, dPitch: 0 });
    expect(reduceScene([], turned, { type: 'resetView' }, homeCamera('right')).camera).toEqual(
      homeCamera('right'),
    );
  });
});

describe('innerDoorIndexFor', () => {
  it('finds the door whose range covers a level', () => {
    expect(innerDoorIndexFor(TSX_DOORS, 0)).toBe(0);
    expect(innerDoorIndexFor(TSX_DOORS, 1)).toBe(0);
    expect(innerDoorIndexFor(TSX_DOORS, 3)).toBe(1);
    expect(innerDoorIndexFor(TSX_DOORS, 4)).toBeNull();
    expect(innerDoorIndexFor([], 0)).toBeNull();
  });
});
