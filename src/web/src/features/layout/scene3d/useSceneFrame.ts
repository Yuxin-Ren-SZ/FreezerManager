// SPDX-License-Identifier: AGPL-3.0-or-later
import { useEffect, useRef, useState } from 'react';
import type { SceneFrame } from './geometry';
import type { RackRef, SceneState } from './interaction';

/**
 * The drawn frame, moving towards what `SceneState` asks for.
 *
 * Doors swing and racks slide over a few hundred milliseconds. Parts close
 * before others open: a rack goes back before its inner door shuts, and a
 * different inner door only starts opening once the first one has shut, so the
 * drawing never shows two doors through each other.
 *
 * With `prefers-reduced-motion`, or where `matchMedia`/`requestAnimationFrame`
 * do not exist (tests, old kiosks), the frame jumps straight to the target.
 */

const SPEED = 1 / 320; // of the full travel, per millisecond

function prefersReducedMotion(): boolean {
  if (typeof globalThis.matchMedia !== 'function') {
    return true;
  }
  return globalThis.matchMedia('(prefers-reduced-motion: reduce)').matches;
}

const sameRack = (left: RackRef | null, right: RackRef | null) =>
  left?.level === right?.level && left?.column === right?.column;

/** One step of the tween: the frame moved `delta` of the way towards `state`. */
export function stepFrame(frame: SceneFrame, state: SceneState, delta: number): SceneFrame {
  const toward = (value: number, goal: number) =>
    value < goal ? Math.min(goal, value + delta) : Math.max(goal, value - delta);

  // The rack goes back first if it is not the one that should be out.
  const rackChanges = frame.pulled !== null && !sameRack(frame.pulled, state.pulledRack);
  if (rackChanges && frame.pull > 0) {
    return { ...frame, pull: toward(frame.pull, 0) };
  }
  const pulled = rackChanges ? null : frame.pulled;

  // Then the inner door, if a different one (or none) should be open.
  const innerChanges = frame.innerDoor !== null && frame.innerDoor !== state.innerDoor;
  if (innerChanges && frame.inner > 0) {
    return { ...frame, pulled, pull: 0, inner: toward(frame.inner, 0) };
  }
  const innerDoor = innerChanges ? null : frame.innerDoor;

  // Closing the outer door waits until everything inside is shut.
  const door = toward(frame.door, state.doorOpen ? 1 : 0);
  if (!state.doorOpen) {
    return { door, innerDoor, inner: 0, pulled, pull: 0 };
  }
  if (door < 1) {
    return { ...frame, door, innerDoor, pulled };
  }

  const nextInnerDoor = innerDoor ?? state.innerDoor;
  const inner = nextInnerDoor === null ? 0 : toward(frame.inner, 1);
  if (state.pulledRack !== null && inner < 1 && nextInnerDoor !== null) {
    return { door, innerDoor: nextInnerDoor, inner, pulled, pull: 0 };
  }
  const nextPulled = pulled ?? state.pulledRack;
  return {
    door,
    innerDoor: nextInnerDoor,
    inner,
    pulled: nextPulled,
    pull: nextPulled === null ? 0 : toward(frame.pull, 1),
  };
}

/** The frame `state` ends at, with nothing in motion. */
export function settledFrame(state: SceneState): SceneFrame {
  return {
    door: state.doorOpen ? 1 : 0,
    innerDoor: state.doorOpen ? state.innerDoor : null,
    inner: state.doorOpen && state.innerDoor !== null ? 1 : 0,
    pulled: state.doorOpen ? state.pulledRack : null,
    pull: state.doorOpen && state.pulledRack !== null ? 1 : 0,
  };
}

const same = (left: SceneFrame, right: SceneFrame) =>
  left.door === right.door &&
  left.inner === right.inner &&
  left.innerDoor === right.innerDoor &&
  left.pull === right.pull &&
  sameRack(left.pulled, right.pulled);

export function useSceneFrame(state: SceneState): SceneFrame {
  // Asked once: a change of the preference mid-visit takes effect next visit.
  const [animate] = useState(
    () => !prefersReducedMotion() && typeof globalThis.requestAnimationFrame === 'function',
  );
  const [frame, setFrame] = useState<SceneFrame>(() => settledFrame(state));
  const frameRef = useRef(frame);

  useEffect(() => {
    if (!animate) return undefined;
    const target = settledFrame(state);
    let handle = 0;
    let last = performance.now();
    const tick = (now: number) => {
      const next = stepFrame(frameRef.current, state, (now - last) * SPEED);
      last = now;
      frameRef.current = next;
      setFrame(next);
      if (!same(next, target)) {
        handle = requestAnimationFrame(tick);
      }
    };
    handle = requestAnimationFrame(tick);
    return () => {
      cancelAnimationFrame(handle);
    };
  }, [animate, state]);

  return animate ? frame : settledFrame(state);
}
