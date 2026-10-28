// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import {
  FreezerForm,
  FreezerModelSchema,
  HingeSide,
  type FreezerModel,
} from '../gen/fmgr/v1/box_pb';

/**
 * The built-in freezer catalogue as `freezerd` serves it (`src/core/freezer_models.h`,
 * ADR 0003 §1), for the fakes and the 3D view's tests.
 *
 * A test copy, not a second source of truth: the SPA never ships these numbers.
 * It draws whatever `freezer/scene` and `freezer-model/list` return.
 */

const compartment = (
  level: number,
  floorMm: number,
  heightMm: number,
  rackSlots: number,
  slot: { width: number; depth: number; height: number },
) => ({
  level,
  floorMm,
  heightMm,
  rackSlots,
  rackSlotMm: { widthMm: slot.width, depthMm: slot.depth, heightMm: slot.height },
});

const F740_SLOT = { width: 139, depth: 560, height: 430 };
const TSX_SLOT = { width: 136, depth: 665, height: 295 };

export const F740_MODEL: FreezerModel = create(FreezerModelSchema, {
  id: 'eppendorf-cryocube-f740hi',
  manufacturer: 'Eppendorf',
  modelCode: 'F740hi',
  displayName: 'CryoCube F740hi',
  form: FreezerForm.UPRIGHT,
  exteriorMm: { widthMm: 1099, depthMm: 980, heightMm: 1973 },
  interiorMm: { widthMm: 865, depthMm: 622, heightMm: 1390 },
  hinge: HingeSide.RIGHT,
  innerDoors: [
    { firstLevel: 0, lastLevel: 0 },
    { firstLevel: 1, lastLevel: 1 },
    { firstLevel: 2, lastLevel: 2 },
  ],
  compartments: [
    compartment(0, 0, 457, 6, F740_SLOT),
    compartment(1, 466, 457, 6, F740_SLOT),
    compartment(2, 932, 457, 6, F740_SLOT),
  ],
  renderKey: 'cryocube-f740',
});

export const TSX_MODEL: FreezerModel = create(FreezerModelSchema, {
  id: 'thermo-tsx40086a',
  manufacturer: 'Thermo Scientific',
  modelCode: 'TSX40086A',
  displayName: 'TSX Universal TSX40086A',
  form: FreezerForm.UPRIGHT,
  exteriorMm: { widthMm: 719, depthMm: 955, heightMm: 1981 },
  interiorMm: { widthMm: 588, depthMm: 719, heightMm: 1301 },
  hinge: HingeSide.RIGHT,
  innerDoors: [
    { firstLevel: 0, lastLevel: 1 },
    { firstLevel: 2, lastLevel: 3 },
  ],
  compartments: [
    compartment(0, 0, 320, 4, TSX_SLOT),
    compartment(1, 325, 320, 4, TSX_SLOT),
    compartment(2, 650, 320, 4, TSX_SLOT),
    compartment(3, 975, 320, 4, TSX_SLOT),
  ],
  renderKey: 'tsx-40086',
});

export const BUILTIN_FREEZER_MODELS: readonly FreezerModel[] = [F740_MODEL, TSX_MODEL];
