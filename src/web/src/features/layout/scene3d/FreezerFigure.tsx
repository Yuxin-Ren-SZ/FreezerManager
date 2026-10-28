// SPDX-License-Identifier: AGPL-3.0-or-later
import { useMemo, useRef, useState } from 'react';
import type { PointerEvent } from 'react';
import type { CabinetSpec } from './cabinet';
import { buildBlocks, sceneEnvelope, type SceneFrame } from './geometry';
import { paint, project, screenBounds, type BlockTarget, type Camera } from './iso';
import type { RenderModel } from './sceneModel';
import styles from './FreezerScene.module.css';

/**
 * The drawing itself: an SVG of painted faces.
 *
 * The SVG is `aria-hidden`; the wrapper is an image whose name says what is
 * open. Everything the drawing does — open, pull, turn, choose a box — the
 * controls and the rack list beside it do too, with real buttons and links, so
 * the figure is a pointer convenience on top of an accessible page (ADR 0003
 * §5) and needs no keyboard handling of its own.
 */

export interface FreezerFigureProps {
  readonly cabinet: CabinetSpec;
  readonly scene: RenderModel;
  readonly frame: SceneFrame;
  readonly camera: Camera;
  readonly label: string;
  readonly onTarget: (target: BlockTarget) => void;
  readonly onRotate: (dYaw: number, dPitch: number) => void;
}

/** Degrees turned per pixel dragged. */
const DRAG_RATE = 0.4;
/** A press that moves less than this is a click, not a drag. */
const CLICK_SLOP = 4;

const POINT = (value: number) => value.toFixed(1);

export function FreezerFigure({
  cabinet,
  scene,
  frame,
  camera,
  label,
  onTarget,
  onRotate,
}: FreezerFigureProps) {
  // Where the press started, how far it has moved, and the face it landed on:
  // a press that barely moves is a click on that face.
  const drag = useRef<{ x: number; y: number; moved: number; face: string | undefined } | null>(
    null,
  );
  const [dragging, setDragging] = useState(false);

  const faces = useMemo(
    () => paint(buildBlocks(cabinet, scene, frame), camera),
    [cabinet, scene, frame, camera],
  );

  // Fixed per cabinet and camera, so the drawing does not jump while doors move.
  const viewBox = useMemo(() => {
    const bounds = screenBounds(sceneEnvelope(cabinet), camera);
    const margin = 40;
    return [
      bounds.minX - margin,
      bounds.minY - margin,
      bounds.maxX - bounds.minX + margin * 2,
      bounds.maxY - bounds.minY + margin * 2,
    ]
      .map(POINT)
      .join(' ');
  }, [cabinet, camera]);

  // A soft footprint under the cabinet grounds it on the page.
  const floor = useMemo(() => {
    const { w, d } = cabinet.exterior;
    return [
      [-30, -30, 0],
      [w + 30, -30, 0],
      [w + 30, d + 60, 0],
      [-30, d + 60, 0],
    ]
      .map((corner) => project(corner as [number, number, number], camera))
      .map((point) => `${POINT(point.x)},${POINT(point.y)}`)
      .join(' ');
  }, [cabinet, camera]);

  const onPointerDown = (event: PointerEvent<HTMLDivElement>) => {
    const face = event.target instanceof SVGElement ? event.target.dataset.face : undefined;
    drag.current = { x: event.clientX, y: event.clientY, moved: 0, face };
  };

  const onPointerMove = (event: PointerEvent<HTMLDivElement>) => {
    const start = drag.current;
    if (start === null) return;
    const dx = event.clientX - start.x;
    const dy = event.clientY - start.y;
    drag.current = {
      ...start,
      x: event.clientX,
      y: event.clientY,
      moved: start.moved + Math.abs(dx) + Math.abs(dy),
    };
    if (drag.current.moved > CLICK_SLOP) {
      setDragging(true);
      onRotate(-dx * DRAG_RATE, dy * DRAG_RATE);
    }
  };

  const endDrag = () => {
    drag.current = null;
    setDragging(false);
  };

  const onPointerUp = () => {
    const start = drag.current;
    endDrag();
    if (start === null || start.moved > CLICK_SLOP) return;
    // The face the press landed on is the topmost one there: the DOM already
    // holds the painted order.
    const face = faces.find((candidate) => candidate.key === start.face);
    if (face?.target !== undefined) {
      onTarget(face.target);
    }
  };

  return (
    <div
      className={styles.figure}
      role="img"
      aria-label={label}
      data-dragging={dragging}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerLeave={endDrag}
      onPointerCancel={endDrag}
    >
      <svg className={styles.svg} viewBox={viewBox} aria-hidden="true" focusable="false">
        <polygon className={styles.floor} points={floor} />
        {faces.map((face) => (
          <polygon
            key={face.key}
            className={styles.face}
            data-face={face.key}
            data-material={face.material}
            data-tone={face.tone}
            data-clickable={face.target !== undefined}
            points={face.points.map((point) => `${POINT(point.x)},${POINT(point.y)}`).join(' ')}
          />
        ))}
      </svg>
    </div>
  );
}
