// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useCreateSample, useSamples, useUpdateSample } from '../../api/hooks';
import { useCan } from '../../app/session';
import { SampleSchema, type Sample } from '../../gen/fmgr/v1/sample_pb';
import { FieldDataType, type CustomFieldDefinition } from '../../gen/fmgr/v1/item_type_pb';
import { Button, Checkbox, Select, TextField, type SelectOption } from '../../ui';
import {
  constraintsOf,
  enumValues,
  parseCustomFieldValues,
  resolveInheritedDefinitions,
  serializeCustomFieldValues,
  validateCustomFieldValues,
} from './customFields';
import { mapServerFailure, type FailureMessage } from './serverErrors';
import styles from './SampleForm.module.css';
import type { SampleReferenceData } from './useSampleReferenceData';

/**
 * The generated sample create/edit form (TODO.md G3.3).
 *
 * Three contracts shape it, and each is visible in the code below:
 *
 *  1. **The field list is the item type's *inherited* definitions.**
 *     `resolveInheritedDefinitions` walks the hierarchy, because the server's
 *     `ListCustomFieldDefinitions` does not: passing an `item_type_id` returns
 *     that node's definitions alone. A leaf-only form looks correct against an
 *     item type with no parent, which is why that case is the one the tests do
 *     not rely on.
 *  2. **The client validates by mirroring `core::validate_custom_fields`, and
 *     the server decides.** Submitting runs the mirror; a server
 *     `INVALID_ARGUMENT` is attributed to the field it names
 *     (`serverErrors.ts`) and shown there, not as a banner. A rule only the
 *     server has — the container-type size class, a position taken between the
 *     picker being drawn and the save — can therefore only ever surface that
 *     way.
 *  3. **Nothing is invented about PHI.** The form submits the fields it
 *     rendered and no others, so a value the server withheld is never sent
 *     back as a blank.
 */

/** What the inputs actually hold: text-ish values and checkboxes. */
type RawValue = string | boolean;

/** The core fields' names, which are also the keys a server rejection uses. */
const FIELD = {
  name: 'name',
  barcode: 'barcode',
  itemTypeId: 'itemTypeId',
  containerTypeId: 'containerTypeId',
  boxId: 'boxId',
  positionLabel: 'positionLabel',
  volumeValue: 'volumeValue',
  volumeUnit: 'volumeUnit',
  massValue: 'massValue',
  massUnit: 'massUnit',
  parentSampleId: 'parentSampleId',
} as const;

export interface SampleFormProps {
  readonly labId: string;
  /** The sample being edited, or `null` to create a new one. */
  readonly sample: Sample | null;
  readonly reference: SampleReferenceData;
  readonly onSaved: (sample: Sample) => void;
  readonly onCancel: () => void;
}

/** `mL`/`µL` and `mg`/`g` are the only units `core::parse_*_unit` accepts. */
const VOLUME_UNITS = ['', 'mL', '\u00b5L'];
/** HTML `step` values: attributes, not copy. */
const STEP_ANY = 'any';
const STEP_INTEGER = '1';
const MASS_UNITS = ['', 'mg', 'g'];

const defaultRaw = (cfd: CustomFieldDefinition): RawValue =>
  cfd.dataType === FieldDataType.BOOL ? false : '';

/** A stored value as the control that edits it wants it. */
function rawFromStored(cfd: CustomFieldDefinition, stored: unknown): RawValue {
  if (cfd.dataType === FieldDataType.BOOL) {
    return stored === true;
  }
  if (typeof stored === 'number') {
    return String(stored);
  }
  if (typeof stored === 'string') {
    // `datetime-local` only accepts minute precision; the seconds are added
    // back on submit, so a stored "…T10:30:00" must not be truncated to a value
    // the input refuses.
    return cfd.dataType === FieldDataType.DATETIME ? stored.slice(0, 16) : stored;
  }
  return '';
}

/** The submitted value for one field, or `undefined` to leave it out entirely. */
function wireValue(cfd: CustomFieldDefinition, raw: RawValue): unknown {
  if (cfd.dataType === FieldDataType.BOOL) {
    return raw === true;
  }
  const text = typeof raw === 'string' ? raw : '';
  if (text === '') {
    // An empty control means "no value", not "an empty value". Leaving the key
    // out is what makes a required field fail as required rather than pass as
    // an empty string.
    return undefined;
  }
  switch (cfd.dataType) {
    case FieldDataType.INT:
    case FieldDataType.FLOAT:
      return Number(text);
    case FieldDataType.DATETIME:
      // The validator wants `YYYY-MM-DDTHH:MM:SS`; `datetime-local` gives
      // `YYYY-MM-DDTHH:MM`.
      return text.length === 16 ? `${text}:00` : text;
    default:
      return text;
  }
}

export function SampleForm({ labId, sample, reference, onSaved, onCancel }: SampleFormProps) {
  const { t } = useTranslation('sample-detail');
  const isEdit = sample !== null;

  const [itemTypeId, setItemTypeId] = useState(sample?.itemTypeId ?? '');
  const [name, setName] = useState(sample?.name ?? '');
  const [barcode, setBarcode] = useState(sample?.barcode ?? '');
  const [containerTypeId, setContainerTypeId] = useState(sample?.containerTypeId ?? '');
  const [boxId, setBoxId] = useState(sample?.boxId ?? '');
  const [positionLabel, setPositionLabel] = useState(sample?.positionLabel ?? '');
  const [volumeValue, setVolumeValue] = useState(
    sample?.volumeValue === undefined ? '' : String(sample.volumeValue),
  );
  const [volumeUnit, setVolumeUnit] = useState(sample?.volumeUnit ?? '');
  const [massValue, setMassValue] = useState(
    sample?.massValue === undefined ? '' : String(sample.massValue),
  );
  const [massUnit, setMassUnit] = useState(sample?.massUnit ?? '');
  const [parentSampleId, setParentSampleId] = useState(sample?.parentSampleId ?? '');

  /**
   * Only what the user has typed. The loaded sample's values are read through
   * `rawFromStored` in the memo below instead of being seeded here: a stored
   * `3` is a *string* "3" to a text input, and seeding the raw JSON here is how
   * an integer field ends up rendering empty.
   */
  const [edited, setEdited] = useState<Record<string, RawValue>>({});

  const [fieldMessages, setFieldMessages] = useState<Record<string, FailureMessage | undefined>>(
    {},
  );
  const [formMessage, setFormMessage] = useState<FailureMessage | null>(null);
  const [saved, setSaved] = useState(false);

  const definitions = useMemo(
    () => resolveInheritedDefinitions(reference.itemTypes, reference.cfds, itemTypeId),
    [reference.itemTypes, reference.cfds, itemTypeId],
  );

  /**
   * The controls' current values: whatever was edited, falling back to the
   * value the sample was loaded with, then to the type's empty value. Deriving
   * it (rather than seeding state in an effect) is what keeps a definition list
   * that arrives *after* the first paint — which is every load — from wiping
   * what the user has typed.
   */
  const rawValues = useMemo(() => {
    const merged: Record<string, RawValue> = {};
    const stored = parseCustomFieldValues(sample?.customFieldsJson ?? '{}');
    for (const cfd of definitions) {
      // `Object.hasOwn`, not a comparison against `undefined`: an index
      // signature is typed as always present, and "the user cleared this field"
      // and "the user never touched it" must stay distinguishable anyway.
      if (Object.hasOwn(edited, cfd.key)) {
        merged[cfd.key] = edited[cfd.key];
        continue;
      }
      const loaded = stored[cfd.key];
      merged[cfd.key] = loaded === undefined ? defaultRaw(cfd) : rawFromStored(cfd, loaded);
    }
    return merged;
  }, [definitions, edited, sample]);

  const setRaw = (key: string, value: RawValue) => {
    setEdited((current) => ({ ...current, [key]: value }));
  };

  // The box's own samples decide which positions are free. Same query the
  // browser screen uses, so the cache is shared.
  const boxSamples = useSamples({ labId, boxId: boxId === '' ? undefined : boxId });
  const occupied = useMemo(() => {
    const labels = new Set<string>();
    for (const page of boxSamples.data?.pages ?? []) {
      for (const candidate of page.samples) {
        // Editing: this sample's own position is not "taken" from itself.
        if (candidate.id === sample?.id) continue;
        if (candidate.positionLabel !== undefined && candidate.positionLabel !== '') {
          labels.add(candidate.positionLabel);
        }
      }
    }
    return labels;
  }, [boxSamples.data, sample?.id]);

  const box = reference.boxes.find((candidate) => candidate.id === boxId);
  const boxType = reference.boxTypes.find((candidate) => candidate.id === box?.boxTypeId);
  const positions = (boxType?.positions ?? []).filter((position) => !occupied.has(position.label));

  const itemTypeOptions: SelectOption[] = reference.itemTypes.map((itemType) => ({
    value: itemType.id,
    label: itemType.name,
  }));
  const containerTypeOptions: SelectOption[] = reference.containerTypes.map((containerType) => ({
    value: containerType.id,
    label: containerType.name,
  }));
  const boxOptions: SelectOption[] = reference.boxes.map((candidate) => ({
    value: candidate.id,
    label: candidate.label === '' ? candidate.id : candidate.label,
  }));
  const positionOptions: SelectOption[] = positions.map((position) => ({
    value: position.label,
    label: position.label,
  }));

  const noChoice = { value: '', label: t('form.noChoice') };

  const canReadPhi = useCan('phi.read', labId);
  const hasPhiDefinition = definitions.some((cfd) => cfd.isPhi);

  const createSample = useCreateSample(labId);
  const updateSample = useUpdateSample(labId);
  const pending = createSample.isPending || updateSample.isPending;

  /** The blob the server will validate, built from the rendered fields only. */
  const wireValues = useMemo(() => {
    const values: Record<string, unknown> = {};
    for (const cfd of definitions) {
      const value = wireValue(cfd, rawValues[cfd.key] ?? defaultRaw(cfd));
      if (value !== undefined) {
        values[cfd.key] = value;
      }
    }
    return values;
  }, [definitions, rawValues]);

  const renderedKeys = new Set<string>([
    ...definitions.map((cfd) => cfd.key),
    ...Object.values(FIELD),
  ]);

  /**
   * Server messages for fields this form has no control for — a required
   * custom field on an item type whose definitions could not be read, say. They
   * are still named, because "the server rejected something" is exactly the
   * banner the acceptance criteria rule out.
   */
  const unattached: { key: string; message: FailureMessage }[] = [];
  for (const [key, message] of Object.entries(fieldMessages)) {
    if (message !== undefined && !renderedKeys.has(key)) {
      unattached.push({ key, message });
    }
  }

  const messageFor = (key: string): string | undefined => {
    const message = fieldMessages[key];
    return message === undefined ? undefined : String(t(message.key as never, { ns: message.ns }));
  };

  async function submit(event: { preventDefault: () => void }) {
    event.preventDefault();

    const mirror = validateCustomFieldValues(definitions, wireValues);
    const messages: Record<string, FailureMessage | undefined> = {};
    if (name.trim() === '') {
      messages.name = { ns: 'sample-detail', key: 'validation.required' };
    }
    if (itemTypeId === '') {
      messages.itemTypeId = { ns: 'sample-detail', key: 'validation.required' };
    }
    for (const error of mirror) {
      messages[error.key] = { ns: 'sample-detail', key: error.messageKey };
    }

    setFormMessage(null);
    setSaved(false);
    setFieldMessages(messages);
    if (Object.keys(messages).length > 0) {
      return;
    }

    const optional = (value: string) => (value === '' ? undefined : value);
    const number = (value: string) => (value === '' ? undefined : Number(value));

    try {
      const payload = {
        itemTypeId,
        name,
        barcode: optional(barcode),
        containerTypeId: optional(containerTypeId),
        boxId: optional(boxId),
        positionLabel: optional(positionLabel),
        volumeValue: number(volumeValue),
        volumeUnit: optional(volumeUnit),
        massValue: number(massValue),
        massUnit: optional(massUnit),
        parentSampleId: optional(parentSampleId),
        customFieldsJson: serializeCustomFieldValues(wireValues),
      };

      const response =
        sample === null
          ? await createSample.mutateAsync({ labId, ...payload })
          : await updateSample.mutateAsync({
              sample: create(SampleSchema, { ...sample, ...payload }),
            });

      setFieldMessages({});
      setSaved(true);
      if (response.sample !== undefined) {
        onSaved(response.sample);
      }
    } catch (error) {
      const mapped = mapServerFailure(error, isEdit ? 'update' : 'create');
      setFieldMessages(mapped.fields);
      setFormMessage(mapped.form);
    }
  }

  return (
    <form className={styles.form} onSubmit={(event) => void submit(event)} noValidate>
      <h1 className={styles.title}>{isEdit ? t('form.editTitle') : t('form.createTitle')}</h1>

      {formMessage !== null ? (
        <p className={styles.formError} role="alert">
          {String(t(formMessage.key as never, { ns: formMessage.ns }))}
        </p>
      ) : null}

      {saved ? (
        <p className={styles.saved} role="status">
          {t('form.saved')}
        </p>
      ) : null}

      {unattached.length > 0 ? (
        <div className={styles.serverErrors} data-testid="server-field-errors">
          <ul className={styles.serverErrorList}>
            {unattached.map(({ key, message }) => (
              <li key={key}>
                <strong>{key}</strong>
                <span>{String(t(message.key as never, { ns: message.ns }))}</span>
              </li>
            ))}
          </ul>
        </div>
      ) : null}

      <TextField
        label={t('form.name')}
        value={name}
        onChange={(event) => {
          setName(event.target.value);
        }}
        error={messageFor(FIELD.name)}
        required
      />

      <Select
        label={t('form.itemType')}
        value={itemTypeId}
        onChange={(event) => {
          setItemTypeId(event.target.value);
        }}
        options={[noChoice, ...itemTypeOptions]}
        error={messageFor(FIELD.itemTypeId)}
        // Changing the item type would re-generate the field list under values
        // that no longer belong to it, and the server keys PHI partitioning off
        // the type: an edit keeps it, a create picks it.
        disabled={isEdit}
        required
      />

      <TextField
        label={t('form.barcode')}
        value={barcode}
        onChange={(event) => {
          setBarcode(event.target.value);
        }}
        error={messageFor(FIELD.barcode)}
      />

      <Select
        label={t('form.containerType')}
        value={containerTypeId}
        onChange={(event) => {
          setContainerTypeId(event.target.value);
        }}
        options={[noChoice, ...containerTypeOptions]}
        error={messageFor(FIELD.containerTypeId)}
      />

      <Select
        label={t('form.box')}
        value={boxId}
        onChange={(event) => {
          setBoxId(event.target.value);
          // The old position belongs to the old box.
          setPositionLabel('');
        }}
        options={[noChoice, ...boxOptions]}
        error={messageFor(FIELD.boxId)}
      />

      <Select
        label={t('form.position')}
        value={positionLabel}
        onChange={(event) => {
          setPositionLabel(event.target.value);
        }}
        options={[noChoice, ...positionOptions]}
        error={messageFor(FIELD.positionLabel)}
        disabled={boxId === ''}
      />

      <div className={styles.row}>
        <TextField
          label={t('form.volumeValue')}
          type="number"
          step={STEP_ANY}
          value={volumeValue}
          onChange={(event) => {
            setVolumeValue(event.target.value);
          }}
          error={messageFor(FIELD.volumeValue)}
        />
        <Select
          label={t('form.volumeUnit')}
          value={volumeUnit}
          onChange={(event) => {
            setVolumeUnit(event.target.value);
          }}
          options={VOLUME_UNITS.map((unit) => ({ value: unit, label: unit || t('form.noChoice') }))}
          error={messageFor(FIELD.volumeUnit)}
        />
      </div>

      <div className={styles.row}>
        <TextField
          label={t('form.massValue')}
          type="number"
          step={STEP_ANY}
          value={massValue}
          onChange={(event) => {
            setMassValue(event.target.value);
          }}
          error={messageFor(FIELD.massValue)}
        />
        <Select
          label={t('form.massUnit')}
          value={massUnit}
          onChange={(event) => {
            setMassUnit(event.target.value);
          }}
          options={MASS_UNITS.map((unit) => ({ value: unit, label: unit || t('form.noChoice') }))}
          error={messageFor(FIELD.massUnit)}
        />
      </div>

      <TextField
        label={t('form.parentSample')}
        hint={t('form.parentSampleHint')}
        value={parentSampleId}
        onChange={(event) => {
          setParentSampleId(event.target.value);
        }}
        error={messageFor(FIELD.parentSampleId)}
      />

      <fieldset className={styles.customFields}>
        <legend className={styles.legend}>{t('form.customFields')}</legend>

        {!reference.definitionsReadable ? (
          <p className={styles.notice}>{t('form.customFieldsUnavailable')}</p>
        ) : null}

        {isEdit && hasPhiDefinition && !canReadPhi ? (
          <p className={styles.notice} role="status">
            {t('form.phiWriteWarning')}
          </p>
        ) : null}

        {definitions.map((cfd) => (
          <CustomFieldInput
            key={cfd.key}
            cfd={cfd}
            value={rawValues[cfd.key] ?? defaultRaw(cfd)}
            error={messageFor(cfd.key)}
            onChange={(value) => {
              setRaw(cfd.key, value);
            }}
          />
        ))}
      </fieldset>

      <div className={styles.actions}>
        <Button type="button" onClick={onCancel}>
          {t('form.cancel')}
        </Button>
        <Button type="submit" variant="primary" loading={pending}>
          {isEdit ? t('form.submitUpdate') : t('form.submitCreate')}
        </Button>
      </div>
    </form>
  );
}

/** One generated control, chosen by `FieldDataType`. */
function CustomFieldInput({
  cfd,
  value,
  error,
  onChange,
}: {
  cfd: CustomFieldDefinition;
  value: RawValue;
  error?: string;
  onChange: (value: RawValue) => void;
}) {
  const { t } = useTranslation('sample-detail');
  // A PHI field is marked wherever it is shown; the server decides whether the
  // value arrives at all (`phi.read`), and the label is what tells the user.
  const label = cfd.isPhi ? `${cfd.label} \u2014 ${t('phi.badge')}` : cfd.label;
  const constraints = constraintsOf(cfd);
  const hint =
    typeof constraints.max_length === 'number'
      ? t('hint.maxLength', { count: constraints.max_length })
      : undefined;

  if (cfd.dataType === FieldDataType.BOOL) {
    return (
      <Checkbox
        label={label}
        checked={value === true}
        onChange={(event) => {
          onChange(event.target.checked);
        }}
        error={error}
      />
    );
  }

  if (cfd.dataType === FieldDataType.ENUM) {
    const values = enumValues(cfd);
    return (
      <Select
        label={label}
        value={typeof value === 'string' ? value : ''}
        onChange={(event) => {
          onChange(event.target.value);
        }}
        options={[
          { value: '', label: t('form.noChoice') },
          ...values.map((allowed) => ({ value: allowed, label: allowed })),
        ]}
        error={error}
        required={cfd.required}
      />
    );
  }

  const inputType =
    cfd.dataType === FieldDataType.INT || cfd.dataType === FieldDataType.FLOAT
      ? 'number'
      : cfd.dataType === FieldDataType.DATE
        ? 'date'
        : cfd.dataType === FieldDataType.DATETIME
          ? 'datetime-local'
          : 'text';

  return (
    <TextField
      label={label}
      type={inputType}
      step={cfd.dataType === FieldDataType.INT ? STEP_INTEGER : undefined}
      hint={
        hint ?? (cfd.dataType === FieldDataType.REFERENCE ? t('form.referenceHint') : undefined)
      }
      value={typeof value === 'string' ? value : ''}
      onChange={(event) => {
        onChange(event.target.value);
      }}
      error={error}
      required={cfd.required}
    />
  );
}
