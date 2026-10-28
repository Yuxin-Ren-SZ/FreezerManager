// SPDX-License-Identifier: AGPL-3.0-or-later
import { clampCamera, DEFAULT_CAMERA, type Camera } from './iso';

/**
 * What the user has opened in the 3D view, as a pure reducer (ADR 0003 §5).
 *
 * The rules follow the design demos (`doc/design/layout-3d/`):
 *
 * - reaching in opens whatever is in the way — an inner door opens the outer
 *   door first, and pulling a rack opens both;
 * - one inner door is open at a time, and opening another one pushes back a
 *   rack it does not cover;
 * - closing is the reverse: the rack goes back, then the inner door, then the
 *   outer door.
 *
 * Inner doors are ranges of levels, not one per compartment: the TSX40086A has
 * two doors over four levels (`InnerDoorSpec`).
 *
 * The state is the *target*; the view animates towards it.
 */

export interface InnerDoorRange {
  readonly firstLevel: number;
  readonly lastLevel: number;
}

export interface RackRef {
  readonly level: number;
  readonly column: number;
}

export interface SceneState {
  readonly doorOpen: boolean;
  /** Index into the model's inner doors, or `null` when all are shut. */
  readonly innerDoor: number | null;
  readonly pulledRack: RackRef | null;
  readonly camera: Camera;
}

export type SceneAction =
  | { readonly type: 'openDoor' }
  | { readonly type: 'closeDoor' }
  | { readonly type: 'openInnerDoor'; readonly index: number }
  | { readonly type: 'closeInnerDoor' }
  | { readonly type: 'pullRack'; readonly level: number; readonly column: number }
  | { readonly type: 'pushRack' }
  | { readonly type: 'rotate'; readonly dYaw: number; readonly dPitch: number }
  | { readonly type: 'resetView' };

export const initialSceneState: SceneState = {
  doorOpen: false,
  innerDoor: null,
  pulledRack: null,
  camera: DEFAULT_CAMERA,
};

/**
 * The view a freezer opens with: a little from above, from the side away from
 * the hinge, so the open door stands beside the opening instead of across it.
 */
export function homeCamera(hinge: 'left' | 'right'): Camera {
  return {
    ...DEFAULT_CAMERA,
    yaw: hinge === 'right' ? -Math.abs(DEFAULT_CAMERA.yaw) : Math.abs(DEFAULT_CAMERA.yaw),
  };
}

/** The inner door whose range covers `level`, or `null` when none does. */
export function innerDoorIndexFor(doors: readonly InnerDoorRange[], level: number): number | null {
  const index = doors.findIndex((door) => door.firstLevel <= level && level <= door.lastLevel);
  return index === -1 ? null : index;
}

const sameRack = (left: RackRef | null, right: RackRef) =>
  left !== null && left.level === right.level && left.column === right.column;

export function reduceScene(
  doors: readonly InnerDoorRange[],
  state: SceneState,
  action: SceneAction,
  home: Camera = DEFAULT_CAMERA,
): SceneState {
  switch (action.type) {
    case 'openDoor':
      return { ...state, doorOpen: true };

    case 'closeDoor':
      return { ...state, doorOpen: false, innerDoor: null, pulledRack: null };

    case 'openInnerDoor': {
      const door = doors.at(action.index);
      if (door === undefined) {
        return state;
      }
      const rackStaysOut =
        state.pulledRack !== null &&
        door.firstLevel <= state.pulledRack.level &&
        state.pulledRack.level <= door.lastLevel;
      return {
        ...state,
        doorOpen: true,
        innerDoor: action.index,
        pulledRack: rackStaysOut ? state.pulledRack : null,
      };
    }

    case 'closeInnerDoor':
      return {
        ...state,
        innerDoor: null,
        pulledRack: doors.length === 0 ? state.pulledRack : null,
      };

    case 'pullRack': {
      const rack = { level: action.level, column: action.column };
      if (sameRack(state.pulledRack, rack)) {
        return { ...state, pulledRack: null };
      }
      return {
        ...state,
        doorOpen: true,
        innerDoor: innerDoorIndexFor(doors, action.level),
        pulledRack: rack,
      };
    }

    case 'pushRack':
      return { ...state, pulledRack: null };

    case 'rotate':
      return {
        ...state,
        camera: clampCamera({
          yaw: state.camera.yaw + action.dYaw,
          pitch: state.camera.pitch + action.dPitch,
        }),
      };

    case 'resetView':
      return { ...state, camera: home };
  }
}
