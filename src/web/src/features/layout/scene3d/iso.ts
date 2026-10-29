// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * A zero-dependency isometric engine for the 3D layout view (ADR 0003 §5).
 *
 * World axes, all in millimetres: **x** left → right as seen from the front,
 * **y** front → back (the front face of a freezer is at small y), **z** up.
 *
 * The camera is orthographic. It turns about the vertical axis (`yaw`, degrees,
 * positive = the viewer walks round to the right) and looks down (`pitch`,
 * degrees). Faces are shaded in three flat bands — top light, the side facing
 * screen-left mid, the side facing screen-right dark — which is what makes a
 * flat-colour drawing read as a solid without lighting maths.
 *
 * Everything is drawn as **blocks**: an origin plus three edge vectors (width,
 * depth, height). An axis-aligned box and a door swung about its hinge are the
 * same thing to the engine. Hidden-surface removal is back-face culling plus a
 * painter's sort by block, inside explicit layers. Within a layer, blocks
 * whose outlines overlap on screen are ordered by a face plane that separates
 * them, never by comparing centres: a centre sort paints a rack over the long
 * shelf above it, or a handle through the door it is mounted on. Layers are
 * for containment, which no separating plane can express: the outer shell is a
 * box round everything inside it.
 *
 * Every block stands upright, turned about the vertical axis at most, so two
 * blocks that do not intersect are always separated by a face plane of one of
 * them. Callers keep parts of one layer from passing through each other; a
 * pair that does intersect is left in centre order.
 */

export type Vec3 = readonly [number, number, number];

export interface Camera {
  /** Degrees about the vertical axis; positive shows the right-hand side. */
  readonly yaw: number;
  /** Degrees of downward tilt; 0 is a straight elevation. */
  readonly pitch: number;
}

export interface Projected {
  readonly x: number;
  readonly y: number;
  /** Distance into the screen; larger is further away. */
  readonly depth: number;
}

export type Tone = 'light' | 'mid' | 'dark';

/**
 * What a block is made of. The view maps each material and tone to a CSS
 * class whose colours come from the design tokens, so light and dark themes
 * need no code.
 */
export type Material =
  | 'body'
  | 'plinth'
  | 'grille'
  | 'interior'
  | 'shelf'
  | 'rack'
  | 'rackActive'
  | 'innerDoor'
  | 'door'
  | 'gasket'
  | 'handle'
  | 'panel'
  | 'boxEmpty'
  | 'boxPartial'
  | 'boxFull'
  | 'drawer';

/** What a click on a block means; the view turns it into a scene action. */
export type BlockTarget =
  | { readonly kind: 'door' }
  | { readonly kind: 'innerDoor'; readonly index: number }
  | { readonly kind: 'rack'; readonly level: number; readonly column: number }
  | { readonly kind: 'box'; readonly id: string };

export type FaceName = 'front' | 'back' | 'left' | 'right' | 'bottom' | 'top';

export interface Block {
  readonly id: string;
  readonly origin: Vec3;
  /** Width edge (local +x). */
  readonly u: Vec3;
  /** Depth edge (local +y, front → back). */
  readonly v: Vec3;
  /** Height edge (local +z). */
  readonly w: Vec3;
  readonly material: Material;
  /** Lower layers are painted first, whatever their depth. */
  readonly layer: number;
  readonly target?: BlockTarget;
  /**
   * Draw only these faces. A cabinet's outer shell is drawn from outside only,
   * so its inner faces never cover what is inside; the liner draws those.
   */
  readonly only?: readonly FaceName[];
}

export interface Face {
  /** Unique within one paint: the block id plus the face name. */
  readonly key: string;
  readonly blockId: string;
  readonly points: readonly Projected[];
  readonly normal: Vec3;
  readonly tone: Tone;
  readonly material: Material;
  readonly target?: BlockTarget;
}

const DEG = Math.PI / 180;

/** The view the screen opens with: a little to the right and from above. */
export const DEFAULT_CAMERA: Camera = { yaw: 28, pitch: 24 };

const YAW_LIMIT = 75;
const PITCH_MIN = 5;
const PITCH_MAX = 60;

/**
 * Keeps the camera in front of the freezer and above the floor. The view is
 * for reaching into a cabinet, so turning it to face the back is not offered.
 */
export function clampCamera(camera: Camera): Camera {
  return {
    yaw: Math.min(YAW_LIMIT, Math.max(-YAW_LIMIT, camera.yaw)),
    pitch: Math.min(PITCH_MAX, Math.max(PITCH_MIN, camera.pitch)),
  };
}

/** The world vector expressed in the camera's turned frame (z unchanged). */
function turn(vector: Vec3, camera: Camera): Vec3 {
  const angle = camera.yaw * DEG;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  return [vector[0] * cos + vector[1] * sin, -vector[0] * sin + vector[1] * cos, vector[2]];
}

export function project(point: Vec3, camera: Camera): Projected {
  const [x, y, z] = turn(point, camera);
  const pitch = camera.pitch * DEG;
  const cos = Math.cos(pitch);
  const sin = Math.sin(pitch);
  return { x, y: -(z * cos + y * sin), depth: y * cos - z * sin };
}

/** A face is drawn when its outward normal points back at the viewer. */
export function faceVisible(normal: Vec3, camera: Camera): boolean {
  const [, ny, nz] = turn(normal, camera);
  const pitch = camera.pitch * DEG;
  return ny * Math.cos(pitch) - nz * Math.sin(pitch) < -1e-6;
}

export function faceTone(normal: Vec3, camera: Camera): Tone {
  if (normal[2] > 0.5) {
    return 'light';
  }
  const [nx] = turn(normal, camera);
  return nx > 1e-6 ? 'dark' : 'mid';
}

const add = (a: Vec3, b: Vec3): Vec3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scale = (a: Vec3, factor: number): Vec3 => [a[0] * factor, a[1] * factor, a[2] * factor];
const length = (a: Vec3) => Math.hypot(a[0], a[1], a[2]);
const unit = (a: Vec3): Vec3 => {
  const size = length(a);
  return size === 0 ? [0, 0, 0] : scale(a, 1 / size);
};

/** An axis-aligned box between two corners. */
export function boxBlock(
  id: string,
  min: Vec3,
  max: Vec3,
  material: Material,
  options: { layer?: number; target?: BlockTarget; only?: readonly FaceName[] } = {},
): Block {
  return {
    id,
    origin: min,
    u: [max[0] - min[0], 0, 0],
    v: [0, max[1] - min[1], 0],
    w: [0, 0, max[2] - min[2]],
    material,
    layer: options.layer ?? 1,
    target: options.target,
    only: options.only,
  };
}

/**
 * A block turned `degrees` about the vertical line through `pivot` — a door on
 * its hinge. `u`, `v`, `w` are the unturned edges; `originOffset` is where the
 * unturned block's origin sits relative to the pivot (default: on it).
 */
export function rotatedBlock(
  id: string,
  pivot: Vec3,
  u: Vec3,
  v: Vec3,
  w: Vec3,
  degrees: number,
  material: Material,
  options: { layer?: number; target?: BlockTarget; originOffset?: Vec3 } = {},
): Block {
  const angle = degrees * DEG;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  const spin = (vector: Vec3): Vec3 => [
    vector[0] * cos - vector[1] * sin,
    vector[0] * sin + vector[1] * cos,
    vector[2],
  ];
  return {
    id,
    origin: add(pivot, spin(options.originOffset ?? [0, 0, 0])),
    u: spin(u),
    v: spin(v),
    w: spin(w),
    material,
    layer: options.layer ?? 1,
    target: options.target,
  };
}

interface FaceSpec {
  readonly name: FaceName;
  readonly corners: readonly Vec3[];
  readonly normal: Vec3;
}

function faceSpecs(block: Block): FaceSpec[] {
  const { origin: o, u, v, w } = block;
  const ou = add(o, u);
  const ov = add(o, v);
  const ow = add(o, w);
  const ouv = add(ou, v);
  const ouw = add(ou, w);
  const ovw = add(ov, w);
  const ouvw = add(ouv, w);
  return [
    { name: 'front', corners: [o, ou, ouw, ow], normal: unit(scale(v, -1)) },
    { name: 'back', corners: [ov, ovw, ouvw, ouv], normal: unit(v) },
    { name: 'left', corners: [o, ow, ovw, ov], normal: unit(scale(u, -1)) },
    { name: 'right', corners: [ou, ouv, ouvw, ouw], normal: unit(u) },
    { name: 'bottom', corners: [o, ov, ouv, ou], normal: unit(scale(w, -1)) },
    { name: 'top', corners: [ow, ouw, ouvw, ovw], normal: unit(w) },
  ];
}

/** The faces of `block` the camera can see, projected. */
export function blockFaces(block: Block, camera: Camera): Face[] {
  return faceSpecs(block)
    .filter((spec) => block.only === undefined || block.only.includes(spec.name))
    .filter((spec) => faceVisible(spec.normal, camera))
    .map((spec) => ({
      key: `${block.id}:${spec.name}`,
      blockId: block.id,
      points: spec.corners.map((corner) => project(corner, camera)),
      normal: spec.normal,
      tone: faceTone(spec.normal, camera),
      material: block.material,
      target: block.target,
    }));
}

function blockCentre(block: Block): Vec3 {
  return add(block.origin, scale(add(add(block.u, block.v), block.w), 0.5));
}

function blockCorners(block: Block): Vec3[] {
  const { origin: o, u, v, w } = block;
  const ou = add(o, u);
  const ov = add(o, v);
  return [o, ou, ov, add(ou, v), add(o, w), add(ou, w), add(ov, w), add(add(ou, v), w)];
}

const dot = (a: Vec3, b: Vec3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const subtract = (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];

/** How far two parts may touch or press into each other and still count as apart, in mm. */
const CONTACT = 2;

/**
 * Whether `front` must be painted after `back`, decided by a face plane of one
 * of them with the other wholly beyond it. The block on the viewer's side of
 * such a plane can never be hidden by the other. Every block in a scene stands
 * upright, so for two that do not intersect one of their face planes always
 * separates them; for two that do, nothing is decided.
 */
function paintsOver(front: Block, back: Block, camera: Camera): boolean | null {
  const frontCorners = blockCorners(front);
  const backCorners = blockCorners(back);
  for (const spec of faceSpecs(back)) {
    const beyond = frontCorners.every(
      (corner) => dot(subtract(corner, spec.corners[0]), spec.normal) >= -CONTACT,
    );
    if (beyond) return faceVisible(spec.normal, camera);
  }
  for (const spec of faceSpecs(front)) {
    const beyond = backCorners.every(
      (corner) => dot(subtract(corner, spec.corners[0]), spec.normal) >= -CONTACT,
    );
    if (beyond) return !faceVisible(spec.normal, camera);
  }
  return null;
}

type Point = readonly [number, number];

/** A block's outline on screen: the convex hull of its corners, and its bounds. */
interface Placed {
  readonly block: Block;
  readonly outline: readonly Point[];
  readonly minX: number;
  readonly maxX: number;
  readonly minY: number;
  readonly maxY: number;
}

const cross = (o: Point, a: Point, b: Point) =>
  (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);

/** Andrew's monotone chain: the convex hull of `points`, in order. */
function hull(points: readonly Point[]): Point[] {
  const sorted = [...points].sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  const chain = (input: readonly Point[]) => {
    const out: Point[] = [];
    for (const point of input) {
      while (out.length >= 2 && cross(out[out.length - 2], out[out.length - 1], point) <= 0) {
        out.pop();
      }
      out.push(point);
    }
    out.pop();
    return out;
  };
  return [...chain(sorted), ...chain([...sorted].reverse())];
}

function place(block: Block, camera: Camera): Placed {
  const corners = blockCorners(block).map((corner) => project(corner, camera));
  const xs = corners.map((point) => point.x);
  const ys = corners.map((point) => point.y);
  return {
    block,
    outline: hull(corners.map((point): Point => [point.x, point.y])),
    minX: Math.min(...xs),
    maxX: Math.max(...xs),
    minY: Math.min(...ys),
    maxY: Math.max(...ys),
  };
}

/** Outlines that only touch, or overlap by less than this, are apart. */
const TOUCH = 0.5;

/** Whether some edge of `a`'s outline has all of `b` on its outer side. */
function edgeSeparates(a: readonly Point[], b: readonly Point[]): boolean {
  return a.some((start, index) => {
    const end = a[(index + 1) % a.length];
    const length = Math.hypot(end[0] - start[0], end[1] - start[1]);
    if (length === 0) return false;
    return b.every((point) => cross(start, end, point) / length <= TOUCH);
  });
}

/**
 * Whether two outlines really overlap on screen. Two blocks that do not can be
 * painted in either order, and ordering them anyway can make a cycle with a
 * third block that overlaps both.
 */
function screensOverlap(a: Placed, b: Placed): boolean {
  if (a.minX >= b.maxX || b.minX >= a.maxX || a.minY >= b.maxY || b.minY >= a.maxY) {
    return false;
  }
  return !edgeSeparates(a.outline, b.outline) && !edgeSeparates(b.outline, a.outline);
}

/**
 * Every visible face, in drawing order: layer by layer, and within a layer in
 * occlusion order. Two blocks whose outlines overlap on screen are ordered by
 * a plane that separates them (`paintsOver`); the farthest block by centre
 * goes first wherever that leaves a choice, and breaks the rare cycle. A
 * block's faces stay together, so a box never has a neighbour painted between
 * its own sides.
 */
export function paint(blocks: readonly Block[], camera: Camera): Face[] {
  const ranked = blocks
    .map((block, index) => ({ block, index, depth: project(blockCentre(block), camera).depth }))
    .sort(
      (left, right) =>
        left.block.layer - right.block.layer ||
        right.depth - left.depth ||
        left.index - right.index,
    )
    .map(({ block }) => place(block, camera));

  // after[i]: the blocks that must be painted after block i. Only blocks of
  // one layer are compared; the layers themselves are painted in order.
  const count = ranked.length;
  const after: number[][] = ranked.map(() => []);
  const waiting = new Array<number>(count).fill(0);
  for (let i = 0; i < count; i += 1) {
    for (let j = i + 1; j < count && ranked[j].block.layer === ranked[i].block.layer; j += 1) {
      if (!screensOverlap(ranked[i], ranked[j])) continue;
      const jOverI = paintsOver(ranked[j].block, ranked[i].block, camera);
      if (jOverI === null) continue;
      const [first, second] = jOverI ? [i, j] : [j, i];
      after[first].push(second);
      waiting[second] += 1;
    }
  }

  // Kahn's sort, always taking the best-ranked block that is free to go. If
  // every remaining block waits on another (a cycle), the best-ranked one goes
  // anyway.
  const order: number[] = [];
  const done = new Array<boolean>(count).fill(false);
  let start = 0;
  while (order.length < count) {
    while (done[start]) start += 1;
    // Only the current layer is searched: a cycle is broken inside its layer,
    // never by painting a block of the next layer early.
    const layer = ranked[start].block.layer;
    let next = start;
    while (
      next < count &&
      ranked[next].block.layer === layer &&
      (done[next] || waiting[next] > 0)
    ) {
      next += 1;
    }
    if (next === count || ranked[next].block.layer !== layer) next = start;
    done[next] = true;
    order.push(next);
    for (const later of after[next]) waiting[later] -= 1;
  }
  return order.flatMap((index) => blockFaces(ranked[index].block, camera));
}

/** The 2D box around a set of world points, for the SVG `viewBox`. */
export function screenBounds(points: readonly Vec3[], camera: Camera) {
  const projected = points.map((point) => project(point, camera));
  const xs = projected.map((point) => point.x);
  const ys = projected.map((point) => point.y);
  return {
    minX: Math.min(...xs),
    minY: Math.min(...ys),
    maxX: Math.max(...xs),
    maxY: Math.max(...ys),
  };
}
