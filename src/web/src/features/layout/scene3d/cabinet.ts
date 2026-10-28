// SPDX-License-Identifier: AGPL-3.0-or-later
import { HingeSide, type FreezerModel } from '../../../gen/fmgr/v1/box_pb';
import type { InnerDoorRange } from './interaction';

/**
 * A freezer model → the cabinet the engine draws (ADR 0003 §5), in world
 * millimetres (x left → right, y front → back, z up; see `iso.ts`).
 *
 * The geometry that matters — compartments, rack slots, inner doors, hinge —
 * always comes from the `FreezerModel` the server sends, so the drawing and the
 * placement rules cannot disagree. `render_key` only selects a *profile*: the
 * hand-tuned shell details of a catalogued model (base height, door depth,
 * control panel, rack style). An unknown key, or a lab's own model later, is
 * drawn as the generic profile from its own dimensions; a freezer without a
 * model gets a plain cabinet with no rack slots, and its contents are listed as
 * unplaced.
 */

export type CabinetProfile = 'generic' | 'cryocube-f740' | 'tsx-40086';

/** Side-access racks of tiers (CryoCube), or racks of pull-out drawers (TSX). */
export type RackStyle = 'tiers' | 'drawers';

export interface Extent {
  readonly x0: number;
  readonly y0: number;
  readonly z0: number;
  readonly w: number;
  readonly d: number;
  readonly h: number;
}

export interface CompartmentGeometry {
  readonly level: number;
  readonly z0: number;
  readonly h: number;
  readonly slots: number;
  readonly slotW: number;
  readonly slotD: number;
  readonly slotH: number;
}

export interface CabinetSpec {
  readonly profile: CabinetProfile;
  readonly rackStyle: RackStyle;
  /** The outer shell, door included; the door's outer face is at y = 0. */
  readonly exterior: { readonly w: number; readonly d: number; readonly h: number };
  readonly interior: Extent;
  /** Height of the machine base under the cabinet. */
  readonly plinth: number;
  readonly doorThickness: number;
  readonly hinge: 'left' | 'right';
  readonly compartments: readonly CompartmentGeometry[];
  readonly innerDoors: readonly InnerDoorRange[];
  /** A control panel on the door, as a fraction of the door height from the top. */
  readonly panel: { readonly width: number; readonly height: number; readonly fromTop: number };
  /** A grille in the base (CryoCube) or a hinged base door (TSX). */
  readonly base: 'grille' | 'door';
}

interface ProfileShell {
  readonly plinth: number;
  readonly doorThickness: number;
  readonly rackStyle: RackStyle;
  readonly panel: CabinetSpec['panel'];
  readonly base: CabinetSpec['base'];
}

/** Shell details per catalogued model, read off the design demos. */
const PROFILES: Readonly<Record<Exclude<CabinetProfile, 'generic'>, ProfileShell>> = {
  // CryoCube F740: a deep compressor base with a front grille, a thick door
  // with the touchscreen high on the handle side.
  'cryocube-f740': {
    plinth: 360,
    doorThickness: 90,
    rackStyle: 'tiers',
    panel: { width: 220, height: 150, fromTop: 0.08 },
    base: 'grille',
  },
  // TSX40086A: a tall base behind a hinged bottom door, the 7-inch screen at
  // eye level.
  'tsx-40086': {
    plinth: 430,
    doorThickness: 80,
    rackStyle: 'drawers',
    panel: { width: 180, height: 120, fromTop: 0.22 },
    base: 'door',
  },
};

const isProfile = (key: string): key is Exclude<CabinetProfile, 'generic'> => key in PROFILES;

const clamp = (value: number, low: number, high: number) => Math.min(high, Math.max(low, value));

/** A plain upright cabinet for a freezer without a model. */
const UNMODELLED = {
  exterior: { w: 800, d: 850, h: 1950 },
  interior: { w: 640, d: 600, h: 1400 },
};

export function cabinetFor(model: FreezerModel | undefined): CabinetSpec {
  const exteriorMm = model?.exteriorMm;
  const interiorMm = model?.interiorMm;
  const exterior =
    exteriorMm === undefined
      ? UNMODELLED.exterior
      : { w: exteriorMm.widthMm, d: exteriorMm.depthMm, h: exteriorMm.heightMm };
  const inner =
    interiorMm === undefined
      ? UNMODELLED.interior
      : { w: interiorMm.widthMm, d: interiorMm.depthMm, h: interiorMm.heightMm };

  const key = model?.renderKey ?? '';
  const profile: CabinetProfile = isProfile(key) ? key : 'generic';
  const shell: ProfileShell = isProfile(key)
    ? PROFILES[key]
    : {
        plinth: clamp((exterior.h - inner.h) * 0.6, 80, 500),
        doorThickness: clamp((exterior.d - inner.d) * 0.3, 40, 120),
        rackStyle: 'tiers',
        panel: { width: Math.min(200, exterior.w * 0.25), height: 120, fromTop: 0.1 },
        base: 'grille',
      };

  // Centre the interior across the width; sit it behind the door and its
  // frame; split the remaining height between floor and roof insulation.
  const y0 = shell.doorThickness + 20;
  const depth = Math.min(inner.d, exterior.d - y0 - 40);
  const spare = Math.max(0, exterior.h - shell.plinth - inner.h);
  const interior: Extent = {
    x0: (exterior.w - inner.w) / 2,
    y0,
    z0: shell.plinth + spare * 0.45,
    w: inner.w,
    d: depth,
    h: Math.min(inner.h, exterior.h - shell.plinth - 40),
  };

  const compartments: CompartmentGeometry[] = (model?.compartments ?? []).map((spec) => ({
    level: spec.level,
    z0: interior.z0 + spec.floorMm,
    h: Math.min(spec.heightMm, interior.h - spec.floorMm),
    slots: spec.rackSlots,
    slotW: spec.rackSlotMm?.widthMm ?? interior.w / Math.max(1, spec.rackSlots),
    slotD: Math.min(spec.rackSlotMm?.depthMm ?? interior.d, interior.d - 20),
    slotH: Math.min(spec.rackSlotMm?.heightMm ?? spec.heightMm, spec.heightMm),
  }));

  return {
    profile,
    rackStyle: shell.rackStyle,
    exterior,
    interior,
    plinth: shell.plinth,
    doorThickness: shell.doorThickness,
    hinge: model?.hinge === HingeSide.LEFT ? 'left' : 'right',
    compartments,
    innerDoors: (model?.innerDoors ?? []).map((door) => ({
      firstLevel: door.firstLevel,
      lastLevel: door.lastLevel,
    })),
    panel: shell.panel,
    base: shell.base,
  };
}

/**
 * Where rack slot `column` of compartment `level` stands, or `null` when the
 * cabinet has no such slot. Slots are spread evenly with equal gaps, left to
 * right, and stand at the front of the compartment.
 */
export function slotBounds(cabinet: CabinetSpec, level: number, column: number): Extent | null {
  const compartment = cabinet.compartments.find((candidate) => candidate.level === level);
  if (compartment === undefined || column < 0 || column >= compartment.slots) {
    return null;
  }
  const { interior } = cabinet;
  const width = Math.min(compartment.slotW, interior.w / compartment.slots);
  const gap = (interior.w - width * compartment.slots) / (compartment.slots + 1);
  return {
    x0: interior.x0 + gap + column * (width + gap),
    y0: interior.y0 + 10,
    z0: compartment.z0 + 4,
    w: width,
    d: compartment.slotD,
    h: compartment.slotH,
  };
}
