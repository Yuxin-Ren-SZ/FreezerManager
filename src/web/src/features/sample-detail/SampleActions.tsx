// SPDX-License-Identifier: AGPL-3.0-or-later
import { useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useCheckoutSample, useMoveSample, useSamples, useSoftDeleteSample } from '../../api/hooks';
import { useCan } from '../../app/session';
import { CheckoutAction, SampleStatus, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { Button, ConfirmDialog, Dialog, Select, TextField, type SelectOption } from '../../ui';
import { mapServerFailure, type FailureMessage } from './serverErrors';
import styles from './SampleDetailScreen.module.css';
import type { SampleReferenceData } from './useSampleReferenceData';

/**
 * The five lifecycle actions of one sample (TODO.md G3.3): check out, check in,
 * discard, move and soft delete.
 *
 * Each action is gated on the permission the *server* enforces for that RPC
 * (`SampleServiceImpl.cc`): `sample.checkout` for the checkout family,
 * `sample.write` for move, `sample.delete_soft` for delete. That is UX only
 * (G-arch 8) — every mutation still maps a refusal back onto the screen.
 *
 * The two dialogs carry the fields the RPC actually uses, which is not the same
 * set for all three checkout actions (`storage::apply_checkout`): checking in
 * subtracts `volume_used`, discarding consumes whatever is left and keeps only
 * the reason, checking out takes neither. An input the server ignores would be
 * a lie about what the button does.
 */

type DialogKind = 'checkin' | 'discard' | 'move' | 'delete' | null;

/** The RPC field names a rejection can name; attributes, not copy. */
const FIELD = {
  volumeUsed: 'volumeUsed',
  reason: 'reason',
  boxId: 'boxId',
  positionLabel: 'positionLabel',
} as const;

/** HTML `step` for a fractional quantity: an attribute, not copy. */
const STEP_ANY = 'any';

export interface SampleActionsProps {
  readonly labId: string;
  readonly sample: Sample;
  readonly reference: SampleReferenceData;
  readonly onEdit: () => void;
  readonly onDeleted: () => void;
}

export function SampleActions({ labId, sample, reference, onEdit, onDeleted }: SampleActionsProps) {
  const { t } = useTranslation('sample-detail');
  const canWrite = useCan('sample.write', labId);
  const canCheckout = useCan('sample.checkout', labId);
  const canDelete = useCan('sample.delete_soft', labId);

  const [dialog, setDialog] = useState<DialogKind>(null);
  const [volumeUsed, setVolumeUsed] = useState('');
  const [reason, setReason] = useState('');
  const [destinationBoxId, setDestinationBoxId] = useState('');
  const [destinationPosition, setDestinationPosition] = useState('');
  const [fieldMessages, setFieldMessages] = useState<Record<string, FailureMessage | undefined>>(
    {},
  );
  const [formMessage, setFormMessage] = useState<FailureMessage | null>(null);

  const checkout = useCheckoutSample(labId);
  const move = useMoveSample(labId);
  const remove = useSoftDeleteSample(labId);

  // Where the sample could go: the destination box's own samples decide which
  // of its positions are free, exactly as on the create form.
  const destinationSamples = useSamples({
    labId,
    boxId: destinationBoxId === '' ? undefined : destinationBoxId,
    enabled: dialog === 'move' && destinationBoxId !== '',
  });

  const occupied = useMemo(() => {
    const labels = new Set<string>();
    for (const page of destinationSamples.data?.pages ?? []) {
      for (const candidate of page.samples) {
        if (candidate.id === sample.id) continue;
        const label = candidate.positionLabel ?? '';
        if (label !== '') {
          labels.add(label);
        }
      }
    }
    return labels;
  }, [destinationSamples.data, sample.id]);

  const destinationBox = reference.boxes.find((candidate) => candidate.id === destinationBoxId);
  const destinationBoxType = reference.boxTypes.find(
    (candidate) => candidate.id === destinationBox?.boxTypeId,
  );
  const freePositions: SelectOption[] = (destinationBoxType?.positions ?? [])
    .filter((position) => !occupied.has(position.label))
    .map((position) => ({ value: position.label, label: position.label }));

  const messageFor = (key: string): string | undefined => {
    const message = fieldMessages[key];
    return message === undefined ? undefined : String(t(message.key as never, { ns: message.ns }));
  };

  const close = () => {
    setDialog(null);
    setVolumeUsed('');
    setReason('');
    setDestinationBoxId('');
    setDestinationPosition('');
    setFieldMessages({});
    setFormMessage(null);
  };

  /** Run one mutation with the shared failure routing. */
  async function run(
    action: () => Promise<unknown>,
    operation: 'checkout' | 'move' | 'delete',
    onSuccess: () => void,
  ) {
    setFieldMessages({});
    setFormMessage(null);
    try {
      await action();
      onSuccess();
      close();
    } catch (error) {
      const mapped = mapServerFailure(error, operation);
      setFieldMessages(mapped.fields);
      setFormMessage(mapped.form);
    }
  }

  const active = sample.status === SampleStatus.ACTIVE;
  const checkedOut = sample.status === SampleStatus.CHECKED_OUT;

  return (
    <div className={styles.actions}>
      {canWrite ? <Button onClick={onEdit}>{t('actions.edit')}</Button> : null}

      {canCheckout && active ? (
        <Button
          loading={checkout.isPending}
          onClick={() => {
            void run(
              () =>
                checkout.mutateAsync({
                  sampleId: sample.id,
                  action: CheckoutAction.CHECKOUT,
                }),
              'checkout',
              () => undefined,
            );
          }}
        >
          {t('actions.checkout')}
        </Button>
      ) : null}

      {canCheckout && checkedOut ? (
        <Button
          onClick={() => {
            setDialog('checkin');
          }}
        >
          {t('actions.checkin')}
        </Button>
      ) : null}

      {canCheckout && (active || checkedOut) ? (
        <Button
          onClick={() => {
            setDialog('discard');
          }}
        >
          {t('actions.discard')}
        </Button>
      ) : null}

      {canWrite ? (
        <Button
          onClick={() => {
            setDialog('move');
          }}
        >
          {t('actions.move')}
        </Button>
      ) : null}

      {canDelete ? (
        <Button
          variant="danger"
          onClick={() => {
            setDialog('delete');
          }}
        >
          {t('actions.delete')}
        </Button>
      ) : null}

      <Dialog
        open={dialog === 'checkin'}
        onOpenChange={(open) => {
          if (!open) close();
        }}
        title={t('actions.checkinTitle')}
        footer={
          <>
            <Button onClick={close}>{t('form.cancel')}</Button>
            <Button
              variant="primary"
              loading={checkout.isPending}
              onClick={() => {
                void run(
                  () =>
                    checkout.mutateAsync({
                      sampleId: sample.id,
                      action: CheckoutAction.CHECKIN,
                      volumeUsed: volumeUsed === '' ? undefined : Number(volumeUsed),
                      reason: reason === '' ? undefined : reason,
                    }),
                  'checkout',
                  () => undefined,
                );
              }}
            >
              {t('actions.confirmCheckin')}
            </Button>
          </>
        }
      >
        {formMessage !== null ? (
          <p className={styles.formError} role="alert">
            {String(t(formMessage.key as never, { ns: formMessage.ns }))}
          </p>
        ) : null}
        <TextField
          label={t('actions.volumeUsed')}
          hint={t('actions.volumeUsedHint')}
          type="number"
          step={STEP_ANY}
          value={volumeUsed}
          onChange={(event) => {
            setVolumeUsed(event.target.value);
          }}
          error={messageFor(FIELD.volumeUsed)}
        />
        <TextField
          label={t('actions.reason')}
          value={reason}
          onChange={(event) => {
            setReason(event.target.value);
          }}
          error={messageFor(FIELD.reason)}
        />
      </Dialog>

      <Dialog
        open={dialog === 'discard'}
        onOpenChange={(open) => {
          if (!open) close();
        }}
        title={t('actions.discardTitle')}
        description={t('actions.discardHint')}
        footer={
          <>
            <Button onClick={close}>{t('form.cancel')}</Button>
            <Button
              variant="danger"
              loading={checkout.isPending}
              onClick={() => {
                void run(
                  () =>
                    checkout.mutateAsync({
                      sampleId: sample.id,
                      action: CheckoutAction.DISCARD,
                      reason: reason === '' ? undefined : reason,
                    }),
                  'checkout',
                  () => undefined,
                );
              }}
            >
              {t('actions.confirmDiscard')}
            </Button>
          </>
        }
      >
        {formMessage !== null ? (
          <p className={styles.formError} role="alert">
            {String(t(formMessage.key as never, { ns: formMessage.ns }))}
          </p>
        ) : null}
        <TextField
          label={t('actions.reason')}
          value={reason}
          onChange={(event) => {
            setReason(event.target.value);
          }}
          error={messageFor(FIELD.reason)}
        />
      </Dialog>

      <Dialog
        open={dialog === 'move'}
        onOpenChange={(open) => {
          if (!open) close();
        }}
        title={t('actions.moveTitle')}
        footer={
          <>
            <Button onClick={close}>{t('form.cancel')}</Button>
            <Button
              variant="primary"
              loading={move.isPending}
              onClick={() => {
                void run(
                  () =>
                    move.mutateAsync({
                      sampleId: sample.id,
                      destBoxId: destinationBoxId,
                      destPosition: destinationPosition,
                    }),
                  'move',
                  () => undefined,
                );
              }}
            >
              {t('actions.confirmMove')}
            </Button>
          </>
        }
      >
        {formMessage !== null ? (
          <p className={styles.formError} role="alert">
            {String(t(formMessage.key as never, { ns: formMessage.ns }))}
          </p>
        ) : null}
        <Select
          label={t('actions.destinationBox')}
          value={destinationBoxId}
          onChange={(event) => {
            setDestinationBoxId(event.target.value);
            setDestinationPosition('');
          }}
          options={[
            { value: '', label: t('form.noChoice') },
            ...reference.boxes.map((box) => ({
              value: box.id,
              label: box.label === '' ? box.id : box.label,
            })),
          ]}
          error={messageFor(FIELD.boxId)}
        />
        <Select
          label={t('actions.destinationPosition')}
          value={destinationPosition}
          onChange={(event) => {
            setDestinationPosition(event.target.value);
          }}
          options={[{ value: '', label: t('form.noChoice') }, ...freePositions]}
          error={messageFor(FIELD.positionLabel)}
          disabled={destinationBoxId === ''}
        />
      </Dialog>

      <ConfirmDialog
        open={dialog === 'delete'}
        onOpenChange={(open) => {
          if (!open) close();
        }}
        title={t('actions.deleteTitle')}
        description={t('actions.deleteBody')}
        confirmLabel={t('actions.confirmDelete')}
        cancelLabel={t('actions.keep')}
        tone="danger"
        pending={remove.isPending}
        onConfirm={() => {
          void run(() => remove.mutateAsync({ sampleId: sample.id }), 'delete', onDeleted);
        }}
      >
        {formMessage !== null ? (
          <p className={styles.formError} role="alert">
            {String(t(formMessage.key as never, { ns: formMessage.ns }))}
          </p>
        ) : null}
      </ConfirmDialog>
    </div>
  );
}
