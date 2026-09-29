// SPDX-License-Identifier: AGPL-3.0-or-later
import { useCallback, useId, useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate, useParams } from 'react-router-dom';
import { isApiError } from '../../api/errors';
import { apiErrorMessage } from '../../api/helpers';
import { useMoveSample, useSampleLive, useSamples } from '../../api/hooks';
import { useCan } from '../../app/session';
import type { Sample } from '../../gen/fmgr/v1/sample_pb';
import { Button, EmptyState, ErrorState, Spinner, useToast } from '../../ui';
import { useLabLayout } from '../layout/useLabLayout';
import { placementPath } from '../samples/placement';
import { BoxGrid } from './BoxGrid';
import { BoxPrintSheet } from './BoxPrintSheet';
import styles from './BoxScreen.module.css';
import { buildBoxGrid, gapCount, type BoxGridCell } from './boxGridModel';
import { classifyMoveFailure } from './moveFailure';

/**
 * The box view (TODO.md G3.4, PRD §9 / F6.3): what is in this box, where, and
 * how things move between positions — plus a printable map for the bench.
 *
 * **G3.1's layout is the whole data layer**, as the issue asks: one
 * `useLabLayout(labId)` gives the boxes and the box types the grid is drawn
 * from, and its `locationPath` gives the freezer → … → box line. Nothing here
 * re-derives a position or walks a parent chain.
 *
 * **The box's samples are a `sample/list?box_id=` query, not a slice of the
 * lab.** That is also what makes the live feed work: `useSampleLive` subscribes
 * to `sample/watch?box_id=` and merges each frame into the *list caches by
 * filter*, so a frame that moves a sample out of this box removes its row and a
 * tombstoned frame drops it out entirely (G1.2's `mergeSampleFrame`). A screen
 * that cached the samples itself would need its own merge, and would get the
 * tombstone rule wrong sooner or later.
 *
 * **One move.** `requestMove` is the only place `sample/move` is called, and
 * both the drag path and the keyboard path reach it through the same `payload`
 * state — `BoxGrid` sets it on drag start and on Space, and activates a target
 * cell either way.
 *
 * **A failed move keeps the sample in hand**, so the user can immediately aim
 * at another position instead of picking it up again; the toast explains which
 * of the two rejections happened and what fixes it.
 */

/** The route's two params, defaulted so the screen never renders `undefined`. */
function useBoxParams(): { labId: string; boxId: string } {
  const params = useParams();
  return { labId: params.labId ?? '', boxId: params.boxId ?? '' };
}

export function BoxScreen() {
  const { t } = useTranslation('box');
  // Failure sentences come from `errors.*` in the default namespace.
  const { t: tCommon } = useTranslation();
  const { labId, boxId } = useBoxParams();
  const navigate = useNavigate();
  const toast = useToast();
  const headingId = useId();

  const layout = useLabLayout(labId);
  const samplesQuery = useSamples({ labId, boxId });
  const live = useSampleLive({ labId, boxId });
  const move = useMoveSample(labId);
  const canMove = useCan('sample.write', labId);
  const [payload, setPayload] = useState<Sample | null>(null);

  const box = layout.boxes.find((candidate) => candidate.id === boxId);
  const boxType =
    box === undefined
      ? undefined
      : layout.boxTypes.find((candidate) => candidate.id === box.boxTypeId);
  const boxLabel = box === undefined || box.label === '' ? boxId : box.label;

  const samples = useMemo(
    () => (samplesQuery.data?.pages ?? []).flatMap((page) => page.samples),
    [samplesQuery.data],
  );
  const grid = useMemo(() => buildBoxGrid(boxType, samples), [boxType, samples]);
  const path = placementPath(layout.locationPath(boxId));

  const { mutateAsync: mutateMove } = move;
  const requestMove = useCallback(
    async (sample: Sample, target: BoxGridCell) => {
      const position = target.position.label;
      try {
        await mutateMove({ sampleId: sample.id, destBoxId: boxId, destPosition: position });
        setPayload(null);
        toast.show({
          tone: 'success',
          title: t('move.success', { name: sample.name, position }),
        });
      } catch (error) {
        // Left picked up on purpose: the next position is one activation away.
        setPayload(sample);
        const failure = classifyMoveFailure(error);
        if (failure.kind === 'position-taken') {
          toast.show({
            tone: 'warning',
            title: t('move.positionTakenTitle'),
            description: t('move.positionTakenBody', { position }),
          });
          return;
        }
        if (failure.kind === 'size-mismatch') {
          toast.show({
            tone: 'warning',
            title: t('move.sizeMismatchTitle'),
            description: t('move.sizeMismatchBody', { name: sample.name, position }),
          });
          return;
        }
        toast.show({
          tone: 'danger',
          title: t('move.failedTitle'),
          description: apiErrorMessage(tCommon, error),
        });
      }
    },
    [mutateMove, boxId, toast, t, tCommon],
  );

  const onTarget = useCallback(
    (target: BoxGridCell) => {
      if (payload === null) {
        return;
      }
      void requestMove(payload, target);
    },
    [payload, requestMove],
  );

  const isPending = layout.isPending || samplesQuery.isPending;
  const isError = layout.isError || samplesQuery.isError;
  const error = layout.error ?? samplesQuery.error;
  const retry = useCallback(() => {
    void layout.refetch();
    void samplesQuery.refetch();
  }, [layout, samplesQuery]);

  const liveLabel =
    live.status === 'live'
      ? t('live.live')
      : live.status === 'connecting'
        ? t('live.connecting')
        : t('live.error');

  const gaps = gapCount(grid);

  return (
    <section className={styles.screen} aria-labelledby={headingId}>
      <div className={styles.noPrint}>
        <div className={styles.header}>
          <h1 className={styles.title} id={headingId}>
            {t('title', { label: boxLabel })}
          </h1>
          <p className={styles.live} role="status" aria-label={t('live.label')}>
            {liveLabel}
          </p>
        </div>

        {path === '' ? null : <p className={styles.path}>{path}</p>}

        {isPending ? <Spinner /> : null}

        {!isPending && isError ? (
          <ErrorState
            title={t('errorTitle')}
            description={apiErrorMessage(tCommon, error)}
            requestId={isApiError(error) ? (error.requestId ?? undefined) : undefined}
            onRetry={retry}
          />
        ) : null}

        {!isPending && !isError && box === undefined ? (
          <EmptyState title={t('notFoundTitle')} description={t('notFoundHint')} />
        ) : null}

        {!isPending && !isError && box !== undefined && boxType === undefined ? (
          <EmptyState title={t('noBoxTypeTitle')} description={t('noBoxTypeHint')} />
        ) : null}

        {!isPending && !isError && box !== undefined && boxType !== undefined ? (
          <>
            <p className={styles.selection} role="status" aria-label={t('selection.label')}>
              {payload === null
                ? t('selection.none')
                : t('selection.picked', {
                    name: payload.name,
                    position: payload.positionLabel ?? '',
                  })}
            </p>

            <p className={styles.summary}>
              {t('grid.summary', {
                occupied: grid.sampleCount,
                total: grid.positions.length,
              })}
              {gaps > 0 ? ` ${t('grid.gaps', { count: gaps })}` : ''}
            </p>

            {canMove ? <p className={styles.hint}>{t('grid.hint')}</p> : null}

            {grid.unplaced.length > 0 ? (
              <p className={styles.warning}>
                {t('grid.unplaced', { count: grid.unplaced.length })}
              </p>
            ) : null}

            <BoxGrid
              grid={grid}
              boxLabel={t('grid.label', { box: boxLabel })}
              canMove={canMove}
              payload={payload}
              onPayloadChange={setPayload}
              onTarget={onTarget}
              onOpen={(sample) => {
                void navigate(
                  `/labs/${encodeURIComponent(labId)}/samples/${encodeURIComponent(sample.id)}`,
                );
              }}
            />

            {payload === null ? null : (
              <p>
                <Button
                  variant="ghost"
                  onClick={() => {
                    setPayload(null);
                  }}
                >
                  {t('selection.cancel')}
                </Button>
              </p>
            )}
          </>
        ) : null}
      </div>

      {box !== undefined && boxType !== undefined ? (
        <BoxPrintSheet grid={grid} boxLabel={boxLabel} path={path} />
      ) : null}
    </section>
  );
}
