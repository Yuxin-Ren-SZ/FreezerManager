// SPDX-License-Identifier: AGPL-3.0-or-later
import { useMemo, useReducer, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { useFreezerScene } from '../../../api/hooks';
import { isApiError } from '../../../api/errors';
import { apiErrorMessage } from '../../../api/helpers';
import { Button, ErrorState, Select, Spinner } from '../../../ui';
import { boxPath } from '../paths';
import { cabinetFor, type CabinetSpec } from './cabinet';
import { FreezerFigure } from './FreezerFigure';
import type { BlockTarget } from './iso';
import {
  homeCamera,
  initialSceneState,
  reduceScene,
  type InnerDoorRange,
  type SceneAction,
  type SceneState,
} from './interaction';
import {
  buildRenderModel,
  fillLevel,
  rackAt,
  type RackModel,
  type RenderModel,
} from './sceneModel';
import { useSceneFrame } from './useSceneFrame';
import styles from './FreezerScene.module.css';

/**
 * The 3D tab of the layout screen (ADR 0003 §5, issue #219).
 *
 * One freezer at a time, picked from the lab's freezers. The figure is drawn
 * from `freezer/scene`; beside it, the same actions as real controls, the
 * pulled rack's boxes as links into the box view (G3.4), and every item that
 * has no rack slot under "Unplaced". The tree tab stays the full text
 * alternative.
 */

export interface SceneFreezer {
  readonly id: string;
  readonly label: string;
}

export interface FreezerScenePanelProps {
  readonly labId: string;
  readonly freezers: readonly SceneFreezer[];
}

export function FreezerScenePanel({ labId, freezers }: FreezerScenePanelProps) {
  const { t } = useTranslation('layout');
  const { t: tCommon } = useTranslation();
  const [freezerId, setFreezerId] = useState(() => freezers.at(0)?.id ?? '');
  const selected = freezers.some((freezer) => freezer.id === freezerId)
    ? freezerId
    : (freezers.at(0)?.id ?? '');
  const query = useFreezerScene(labId, selected);

  return (
    <div className={styles.side}>
      <div className={styles.picker}>
        <Select
          label={t('scene.freezer')}
          value={selected}
          options={freezers.map((freezer) => ({ value: freezer.id, label: freezer.label }))}
          onChange={(event) => {
            setFreezerId(event.target.value);
          }}
        />
      </div>

      {query.isPending ? <Spinner /> : null}

      {query.isError ? (
        <ErrorState
          title={t('scene.errorTitle')}
          description={apiErrorMessage(tCommon, query.error)}
          requestId={isApiError(query.error) ? (query.error.requestId ?? undefined) : undefined}
          onRetry={() => {
            void query.refetch();
          }}
        />
      ) : null}

      {query.isSuccess ? (
        // Keyed by freezer: a new freezer starts closed and from the front.
        <FreezerScene key={selected} labId={labId} scene={buildRenderModel(query.data)} />
      ) : null}
    </div>
  );
}

interface FreezerSceneProps {
  readonly labId: string;
  readonly scene: RenderModel;
}

function FreezerScene({ labId, scene }: FreezerSceneProps) {
  const { t } = useTranslation('layout');
  const cabinet = useMemo(() => cabinetFor(scene.model ?? undefined), [scene.model]);
  const doors = cabinet.innerDoors;
  const home = useMemo(() => homeCamera(cabinet.hinge), [cabinet.hinge]);
  const [state, dispatch] = useReducer(
    (current: SceneState, action: SceneAction) => reduceScene(doors, current, action, home),
    { ...initialSceneState, camera: home },
  );
  const frame = useSceneFrame(state);

  const doorName = (index: number) => innerDoorName(t, doors[index]);
  const pulled =
    state.pulledRack === null
      ? undefined
      : rackAt(scene, state.pulledRack.level, state.pulledRack.column);

  const status = !state.doorOpen
    ? t('scene.state.closed')
    : state.innerDoor === null
      ? t('scene.state.doorOpen')
      : t('scene.state.innerOpen', { door: doorName(state.innerDoor) });
  const fullStatus =
    pulled === undefined ? status : `${status} ${t('scene.state.rackOut', { rack: pulled.label })}`;

  const onTarget = (target: BlockTarget) => {
    switch (target.kind) {
      case 'door':
        dispatch({ type: state.doorOpen ? 'closeDoor' : 'openDoor' });
        break;
      case 'innerDoor':
        dispatch(
          state.innerDoor === target.index
            ? { type: 'closeInnerDoor' }
            : { type: 'openInnerDoor', index: target.index },
        );
        break;
      case 'rack':
        dispatch({ type: 'pullRack', level: target.level, column: target.column });
        break;
      case 'box':
        // Boxes are links in the rack list; a click on the drawing selects the
        // same box there rather than navigating away mid-gesture.
        document.getElementById(boxAnchor(target.id))?.focus();
        break;
    }
  };

  return (
    <div className={styles.scene}>
      <FreezerFigure
        cabinet={cabinet}
        scene={scene}
        frame={frame}
        camera={state.camera}
        label={t('scene.figureLabel', { name: scene.freezerName, state: fullStatus })}
        onTarget={onTarget}
        onRotate={(dYaw, dPitch) => {
          dispatch({ type: 'rotate', dYaw, dPitch });
        }}
      />

      <div className={styles.side}>
        <SceneControls
          cabinet={cabinet}
          scene={scene}
          state={state}
          dispatch={dispatch}
          doorName={doorName}
        />
        <p className={styles.status} aria-live="polite">
          {fullStatus}
        </p>

        {scene.model === null ? <p className={styles.muted}>{t('scene.model.none')}</p> : null}

        {pulled === undefined ? null : (
          <RackContents labId={labId} rack={pulled} drawers={cabinet.rackStyle === 'drawers'} />
        )}

        <ul className={styles.legend} aria-label={t('scene.legend.label')}>
          {FILLS.map((fill) => (
            <li key={fill}>
              <span className={styles.swatch} data-fill={fill} aria-hidden="true" />
              {t(`scene.legend.${fill}`)}
            </li>
          ))}
        </ul>

        {scene.unplaced.length === 0 ? null : (
          <section className={styles.section} aria-labelledby="scene-unplaced">
            <h2 id="scene-unplaced">{t('scene.unplaced.title')}</h2>
            <p className={styles.muted}>{t('scene.unplaced.hint')}</p>
            <ul className={styles.list}>
              {scene.unplaced.map((item) => (
                <li key={item.id} className={styles.row}>
                  {item.kind === 'box' ? (
                    <Link to={boxPath(labId, item.id)}>{item.label}</Link>
                  ) : (
                    <span>{item.label}</span>
                  )}
                  <span className={styles.muted}>
                    {t('scene.unplaced.boxes', { count: item.boxes })}
                  </span>
                </li>
              ))}
            </ul>
          </section>
        )}
      </div>
    </div>
  );
}

type LayoutT = ReturnType<typeof useTranslation<'layout'>>['t'];

function innerDoorName(t: LayoutT, door: InnerDoorRange | undefined): string {
  if (door === undefined) return '';
  return door.firstLevel === door.lastLevel
    ? t('scene.innerDoorOne', { level: door.firstLevel + 1 })
    : t('scene.innerDoorRange', { first: door.firstLevel + 1, last: door.lastLevel + 1 });
}

const boxAnchor = (boxId: string) => `scene-box-${boxId}`;

const FILLS = ['empty', 'partial', 'full'] as const;

interface SceneControlsProps {
  readonly cabinet: CabinetSpec;
  readonly scene: RenderModel;
  readonly state: SceneState;
  readonly dispatch: (action: SceneAction) => void;
  readonly doorName: (index: number) => string;
}

const NONE = '';

function SceneControls({ cabinet, scene, state, dispatch, doorName }: SceneControlsProps) {
  const { t } = useTranslation('layout');
  const rackValue = (rack: { level: number; column: number }) =>
    `${String(rack.level)}:${String(rack.column)}`;

  return (
    <div className={styles.controls} role="group" aria-label={t('scene.controls.label')}>
      <Button
        onClick={() => {
          dispatch({ type: state.doorOpen ? 'closeDoor' : 'openDoor' });
        }}
      >
        {state.doorOpen ? t('scene.controls.closeDoor') : t('scene.controls.openDoor')}
      </Button>

      {cabinet.innerDoors.length === 0 ? null : (
        <Select
          label={t('scene.controls.innerDoor')}
          value={state.innerDoor === null ? NONE : String(state.innerDoor)}
          options={[
            { value: NONE, label: t('scene.controls.innerDoorNone') },
            ...cabinet.innerDoors.map((_door, index) => ({
              value: String(index),
              label: doorName(index),
            })),
          ]}
          onChange={(event) => {
            const value = event.target.value;
            dispatch(
              value === NONE
                ? { type: 'closeInnerDoor' }
                : { type: 'openInnerDoor', index: Number(value) },
            );
          }}
        />
      )}

      {scene.racks.length === 0 ? null : (
        <Select
          label={t('scene.controls.rack')}
          value={state.pulledRack === null ? NONE : rackValue(state.pulledRack)}
          options={[
            { value: NONE, label: t('scene.controls.rackNone') },
            ...scene.racks.map((rack) => ({
              value: rackValue(rack),
              label: t('scene.rackOption', {
                level: rack.level + 1,
                column: rack.column + 1,
                label: rack.label,
              }),
            })),
          ]}
          onChange={(event) => {
            const value = event.target.value;
            if (value === NONE) {
              dispatch({ type: 'pushRack' });
              return;
            }
            const [level, column] = value.split(':').map(Number);
            dispatch({ type: 'pullRack', level, column });
          }}
        />
      )}

      <Button
        variant="ghost"
        onClick={() => {
          dispatch({ type: 'rotate', dYaw: 15, dPitch: 0 });
        }}
      >
        {t('scene.controls.turnLeft')}
      </Button>
      <Button
        variant="ghost"
        onClick={() => {
          dispatch({ type: 'rotate', dYaw: -15, dPitch: 0 });
        }}
      >
        {t('scene.controls.turnRight')}
      </Button>
      <Button
        variant="ghost"
        onClick={() => {
          dispatch({ type: 'resetView' });
        }}
      >
        {t('scene.controls.frontView')}
      </Button>
    </div>
  );
}

interface RackContentsProps {
  readonly labId: string;
  readonly rack: RackModel;
  readonly drawers: boolean;
}

function RackContents({ labId, rack, drawers }: RackContentsProps) {
  const { t } = useTranslation('layout');
  const cellName = (level: number, depth: number) =>
    t(drawers ? 'scene.rack.drawerCell' : 'scene.rack.tierCell', {
      level: level + 1,
      depth: depth + 1,
    });
  const boxRow = (box: RackModel['loose'][number], where: string) => (
    <li key={box.id} className={styles.row} data-fill={fillLevel(box.occupied, box.total)}>
      <Link id={boxAnchor(box.id)} to={boxPath(labId, box.id)}>
        {box.label}
      </Link>
      <span className={styles.muted}>
        {where} · {t('scene.rack.fill', { occupied: box.occupied, total: box.total })}
      </span>
    </li>
  );

  return (
    <section className={styles.section} aria-labelledby="scene-rack">
      <h2 id="scene-rack">{rack.label}</h2>
      <p className={styles.muted}>
        {t('scene.rack.summary', {
          count: rack.boxCount,
          occupied: rack.occupied,
          total: rack.total,
        })}
      </p>
      {rack.boxCount === 0 ? (
        <p className={styles.muted}>{t('scene.rack.empty')}</p>
      ) : (
        <ul className={styles.list}>
          {rack.cells.flatMap((cell) =>
            cell.boxes.map((box) => boxRow(box, cellName(cell.level, cell.depth))),
          )}
          {rack.loose.map((box) => boxRow(box, t('scene.rack.loose')))}
        </ul>
      )}
    </section>
  );
}
