// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import {
  type Block,
  blockFaces,
  boxBlock,
  clampCamera,
  faceTone,
  faceVisible,
  paint,
  project,
  rotatedBlock,
} from './iso';

/**
 * The isometric engine (ADR 0003 §5): an orthographic camera that turns about
 * the vertical axis (yaw) and tilts down (pitch), flat three-band shading, and
 * a painter's sort. World axes: x left → right as seen from the front, y front
 * → back, z up, all in millimetres.
 */

const FRONT = { yaw: 0, pitch: 0 };
const RIGHT_FRONT_ABOVE = { yaw: 30, pitch: 30 };

describe('project', () => {
  it('maps a front view straight onto the screen, z up', () => {
    const point = project([100, 0, 50], FRONT);
    expect(point.x).toBeCloseTo(100);
    expect(point.y).toBeCloseTo(-50);
  });

  it('puts a point further back at a greater depth', () => {
    expect(project([0, 500, 0], RIGHT_FRONT_ABOVE).depth).toBeGreaterThan(
      project([0, 0, 0], RIGHT_FRONT_ABOVE).depth,
    );
  });

  it('raises points further back when looking down from above', () => {
    const near = project([0, 0, 0], { yaw: 0, pitch: 30 });
    const far = project([0, 500, 0], { yaw: 0, pitch: 30 });
    expect(far.y).toBeLessThan(near.y);
  });

  it('brings the right side towards the viewer when the camera turns right', () => {
    const left = project([0, 0, 0], { yaw: 45, pitch: 0 });
    const right = project([1000, 0, 0], { yaw: 45, pitch: 0 });
    expect(right.depth).toBeLessThan(left.depth);
  });
});

describe('faces', () => {
  it('shows the front, right and top of a box seen from the right-front, above', () => {
    expect(faceVisible([0, -1, 0], RIGHT_FRONT_ABOVE)).toBe(true);
    expect(faceVisible([1, 0, 0], RIGHT_FRONT_ABOVE)).toBe(true);
    expect(faceVisible([0, 0, 1], RIGHT_FRONT_ABOVE)).toBe(true);
    expect(faceVisible([-1, 0, 0], RIGHT_FRONT_ABOVE)).toBe(false);
    expect(faceVisible([0, 1, 0], RIGHT_FRONT_ABOVE)).toBe(false);
    expect(faceVisible([0, 0, -1], RIGHT_FRONT_ABOVE)).toBe(false);
  });

  it('shades in three flat bands: the top light, the screen-left side mid, the screen-right side dark', () => {
    expect(faceTone([0, 0, 1], RIGHT_FRONT_ABOVE)).toBe('light');
    expect(faceTone([0, -1, 0], RIGHT_FRONT_ABOVE)).toBe('mid');
    expect(faceTone([1, 0, 0], RIGHT_FRONT_ABOVE)).toBe('dark');
  });

  it('returns only the visible faces of a block, at most three for a box', () => {
    const faces = blockFaces(boxBlock('b', [0, 0, 0], [100, 100, 100], 'body'), RIGHT_FRONT_ABOVE);
    expect(faces).toHaveLength(3);
    for (const face of faces) {
      expect(face.points).toHaveLength(4);
    }
  });

  it('turns a rotated block with its axes, so its normals follow the swing', () => {
    // A door slab hinged at x = 100 and swung 90° open: its outer face now
    // looks along +x instead of -y.
    const door = rotatedBlock('door', [100, 0, 0], [100, 0, 0], [0, 20, 0], [0, 0, 200], 90, 'door');
    const front = blockFaces(door, { yaw: 60, pitch: 0 });
    expect(front.some((face) => face.normal[0] > 0.9)).toBe(true);
  });
});

describe('paint', () => {
  const near = boxBlock('near', [0, 0, 0], [100, 100, 100], 'body');
  const far = boxBlock('far', [0, 500, 0], [100, 600, 100], 'body');

  it('paints the farther block first', () => {
    const order = paint([near, far], RIGHT_FRONT_ABOVE).map((face) => face.blockId);
    expect(order.indexOf('far')).toBeLessThan(order.indexOf('near'));
  });

  it('keeps a lower layer underneath whatever its depth', () => {
    const back: Block = { ...near, id: 'back', layer: 0 };
    const front: Block = { ...far, id: 'front', layer: 1 };
    const order = paint([front, back], RIGHT_FRONT_ABOVE).map((face) => face.blockId);
    expect(order.indexOf('back')).toBeLessThan(order.indexOf('front'));
  });

  it("keeps a block's faces together", () => {
    const order = paint([near, far], RIGHT_FRONT_ABOVE).map((face) => face.blockId);
    expect(order).toEqual(['far', 'far', 'far', 'near', 'near', 'near']);
  });
});

describe('clampCamera', () => {
  it('keeps the view in front of the freezer and above the floor', () => {
    expect(clampCamera({ yaw: 200, pitch: 89 })).toEqual({ yaw: 75, pitch: 60 });
    expect(clampCamera({ yaw: -200, pitch: -10 })).toEqual({ yaw: -75, pitch: 5 });
    expect(clampCamera({ yaw: 20, pitch: 25 })).toEqual({ yaw: 20, pitch: 25 });
  });
});
