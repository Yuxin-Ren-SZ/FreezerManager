// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import { FreezerModelSchema, HingeSide } from '../../../gen/fmgr/v1/box_pb';
import { F740_MODEL, TSX_MODEL } from '../../../test/freezerModels';
import { cabinetFor, slotBounds } from './cabinet';

/**
 * A freezer model → the cabinet the engine draws (ADR 0003 §5). `render_key`
 * picks a hand-tuned profile; anything else is drawn from the model's own
 * dimensions, and a freezer without a model still gets a cabinet.
 */

describe('cabinetFor', () => {
  it('builds the CryoCube F740 from its profile: 3 compartments of 6 side-access racks, right hinge', () => {
    const cabinet = cabinetFor(F740_MODEL);
    expect(cabinet.profile).toBe('cryocube-f740');
    expect(cabinet.rackStyle).toBe('tiers');
    expect(cabinet.hinge).toBe('right');
    expect(cabinet.compartments.map((compartment) => compartment.slots)).toEqual([6, 6, 6]);
    expect(cabinet.innerDoors).toHaveLength(3);
  });

  it('builds the TSX40086A with drawer racks and two inner doors over two levels each', () => {
    const cabinet = cabinetFor(TSX_MODEL);
    expect(cabinet.profile).toBe('tsx-40086');
    expect(cabinet.rackStyle).toBe('drawers');
    expect(cabinet.innerDoors).toEqual([
      { firstLevel: 0, lastLevel: 1 },
      { firstLevel: 2, lastLevel: 3 },
    ]);
  });

  it('keeps the interior and every compartment inside the shell', () => {
    for (const model of [F740_MODEL, TSX_MODEL]) {
      const cabinet = cabinetFor(model);
      const { interior, exterior } = cabinet;
      expect(interior.x0).toBeGreaterThan(0);
      expect(interior.x0 + interior.w).toBeLessThan(exterior.w);
      expect(interior.y0).toBeGreaterThan(0);
      expect(interior.y0 + interior.d).toBeLessThan(exterior.d);
      expect(interior.z0).toBeGreaterThanOrEqual(cabinet.plinth);
      expect(interior.z0 + interior.h).toBeLessThan(exterior.h);
      for (const compartment of cabinet.compartments) {
        expect(compartment.z0).toBeGreaterThanOrEqual(interior.z0);
        expect(compartment.z0 + compartment.h).toBeLessThanOrEqual(interior.z0 + interior.h);
      }
    }
  });

  it('draws an unknown render key from the model dimensions, left hinge respected', () => {
    const custom = create(FreezerModelSchema, {
      ...TSX_MODEL,
      id: 'lab-custom',
      renderKey: 'not-a-renderer',
      hinge: HingeSide.LEFT,
    });
    const cabinet = cabinetFor(custom);
    expect(cabinet.profile).toBe('generic');
    expect(cabinet.hinge).toBe('left');
    expect(cabinet.compartments).toHaveLength(4);
  });

  it('gives a freezer without a model a plain cabinet with no rack slots', () => {
    const cabinet = cabinetFor(undefined);
    expect(cabinet.profile).toBe('generic');
    expect(cabinet.compartments).toEqual([]);
    expect(cabinet.innerDoors).toEqual([]);
    expect(cabinet.exterior.h).toBeGreaterThan(cabinet.exterior.w);
  });
});

describe('slotBounds', () => {
  it('spreads the rack slots evenly across the interior, left to right', () => {
    const cabinet = cabinetFor(F740_MODEL);
    const first = slotBounds(cabinet, 0, 0);
    const last = slotBounds(cabinet, 0, 5);
    if (first === null || last === null) {
      throw new Error('the F740 has six slots in its bottom compartment');
    }
    expect(first.x0).toBeGreaterThanOrEqual(cabinet.interior.x0);
    expect(last.x0 + last.w).toBeLessThanOrEqual(cabinet.interior.x0 + cabinet.interior.w);
    expect(last.x0).toBeGreaterThan(first.x0);
    expect(slotBounds(cabinet, 0, 6)).toBeNull();
    expect(slotBounds(cabinet, 3, 0)).toBeNull();
  });
});
