// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import type { TFunction } from 'i18next';
import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { enumValueName } from '../../api/helpers';
import {
  CustomFieldDefinitionSchema,
  FieldDataType,
  FieldDataTypeSchema,
  ScopeKind,
  type CustomFieldDefinition,
} from '../../gen/fmgr/v1/item_type_pb';
import { Button, Checkbox, Dialog, Select, TextField, type SelectOption } from '../../ui';
import { DEFINABLE_DATA_TYPES, constraintPart, dataTypeLabel } from './fieldSummary';
import {
  definitionProblems,
  draftConstraints,
  parseValidation,
  serializeValidation,
  tightenViolations,
  type DefinitionProblemCode,
  type EffectiveField,
  type TightenViolation,
} from './itemTypeModel';
import styles from './ItemTypesScreen.module.css';

/**
 * The field-definition form (TODO.md G3.9, N5).
 *
 * Three modes over one form:
 *
 *  - `create` — a new definition on the open node;
 *  - `edit` — a definition the node already owns;
 *  - `tighten` — a *new* definition on the open node for a `key` it inherits,
 *    with the key and the data type fixed. That is what "a child may tighten a
 *    parent's field" means in this data model: the resolver picks the
 *    most-derived definition per key, so narrowing means defining the same key
 *    one level deeper.
 *
 * **The rules are enforced before the request, not after it.** `problems` runs
 * `definitionProblems` (L10 and the lab's PHI mode) and `tightenViolations`
 * (required stays required, constraints only narrow) over the draft, shows each
 * as a sentence, and disables Save. The submit handler re-derives them, so a
 * form that somehow rendered an enabled Save still cannot send a loosening —
 * and the server's own refusals are handled by the screen as a second line.
 */

export interface DefinitionInput {
  readonly key: string;
  readonly label: string;
  readonly dataType: FieldDataType;
  readonly required: boolean;
  readonly indexed: boolean;
  readonly isPhi: boolean;
  readonly validationJson: string;
}

export interface CustomFieldFormProps {
  readonly mode: 'create' | 'edit' | 'tighten';
  readonly nodeId: string;
  readonly labId: string;
  /** The lab's PHI mode: `is_phi` is offered only when it is on. */
  readonly phiEnabled: boolean;
  /** The definition being edited, or the inherited one being tightened. */
  readonly existing?: CustomFieldDefinition;
  /** The inherited definition of the same key, if there is one. */
  readonly lookupInherited: (key: string) => EffectiveField | null;
  readonly pending: boolean;
  readonly onOpenChange: (open: boolean) => void;
  readonly onSubmit: (input: DefinitionInput) => void;
}

/** The form's raw state: numbers stay strings until they parse. */
interface FormState {
  readonly key: string;
  readonly label: string;
  readonly dataType: FieldDataType;
  readonly required: boolean;
  readonly indexed: boolean;
  readonly isPhi: boolean;
  readonly maxLength: string;
  readonly min: string;
  readonly max: string;
  readonly values: string;
}

function initialState(existing: CustomFieldDefinition | undefined): FormState {
  const constraints = parseValidation(existing?.validationJson ?? '{}');
  return {
    key: existing?.key ?? '',
    label: existing?.label ?? '',
    dataType: existing?.dataType ?? FieldDataType.TEXT,
    required: existing?.required ?? false,
    indexed: existing?.indexed ?? false,
    isPhi: existing?.isPhi ?? false,
    maxLength: constraints.maxLength === undefined ? '' : String(constraints.maxLength),
    min: constraints.min === undefined ? '' : String(constraints.min),
    max: constraints.max === undefined ? '' : String(constraints.max),
    values: constraints.values === undefined ? '' : constraints.values.join(', '),
  };
}

function parseNumber(raw: string): number | undefined {
  if (raw.trim() === '') {
    return undefined;
  }
  const value = Number(raw);
  return Number.isFinite(value) ? value : undefined;
}

function splitValues(raw: string): readonly string[] {
  return raw
    .split(',')
    .map((value) => value.trim())
    .filter((value) => value !== '');
}

/** Whether the data type takes a numeric range (`int`, `float`) or strings. */
function numericRange(dataType: FieldDataType): boolean {
  return dataType === FieldDataType.INT || dataType === FieldDataType.FLOAT;
}

function hasRange(dataType: FieldDataType): boolean {
  return (
    numericRange(dataType) || dataType === FieldDataType.DATE || dataType === FieldDataType.DATETIME
  );
}

export function CustomFieldForm({
  mode,
  nodeId,
  labId,
  phiEnabled,
  existing,
  lookupInherited,
  pending,
  onOpenChange,
  onSubmit,
}: CustomFieldFormProps) {
  const { t } = useTranslation('itemTypes');
  const [state, setState] = useState<FormState>(() => initialState(existing));

  const update = (patch: Partial<FormState>) => {
    setState((current) => ({ ...current, ...patch }));
  };

  const values = splitValues(state.values);
  const constrains = {
    maxLength: parseNumber(state.maxLength),
    min: hasRange(state.dataType)
      ? numericRange(state.dataType)
        ? parseNumber(state.min)
        : state.min.trim() === ''
          ? undefined
          : state.min
      : undefined,
    max: hasRange(state.dataType)
      ? numericRange(state.dataType)
        ? parseNumber(state.max)
        : state.max.trim() === ''
          ? undefined
          : state.max
      : undefined,
    values: state.dataType === FieldDataType.ENUM ? values : undefined,
  };

  const validationJson = serializeValidation(
    draftConstraints({ ...state, ...constrains }),
    state.dataType,
    existing?.validationJson,
  );

  const candidate = create(CustomFieldDefinitionSchema, {
    id: existing?.id ?? '',
    labId,
    scopeKind: ScopeKind.SAMPLE,
    itemTypeId: nodeId,
    key: state.key,
    label: state.label,
    dataType: state.dataType,
    required: state.required,
    validationJson,
    indexed: state.indexed,
    isPhi: state.isPhi,
  });

  const inherited = lookupInherited(state.key);
  const violations = inherited === null ? [] : tightenViolations(inherited.cfd, candidate);
  const problems = definitionProblems(
    { ...state, ...constrains, key: state.key, label: state.label },
    { phiEnabled },
  );

  const messages = [
    ...problems.map((problem) => problemMessage(t, problem.code)),
    ...violations.map((violation) =>
      violationMessage(t, violation, inherited?.cfd ?? null, inheritedOrigin(t, inherited)),
    ),
  ];

  const locked = mode === 'tighten';
  const keyLocked =
    locked || (mode === 'edit' && existing?.itemTypeId === nodeId && inherited !== null);
  const missingRequired = messages.length > 0;
  const title =
    mode === 'create'
      ? t('form.createTitle')
      : mode === 'tighten'
        ? t('form.tightenTitle', { label: existing?.label ?? '' })
        : t('form.editTitle', { label: existing?.label ?? '' });

  return (
    <Dialog
      open
      onOpenChange={onOpenChange}
      title={title}
      description={inheritedHint(t, inherited, mode)}
      footer={
        <>
          <Button
            disabled={pending}
            onClick={() => {
              onOpenChange(false);
            }}
          >
            {t('form.cancel')}
          </Button>
          <Button
            variant="primary"
            loading={pending}
            disabled={missingRequired}
            onClick={() => {
              // Re-derived, not read from `messages`: the submit path refuses a
              // loosening even if the button was somehow enabled.
              if (messages.length > 0) {
                return;
              }
              onSubmit({
                key: state.key,
                label: state.label,
                dataType: state.dataType,
                required: state.required,
                indexed: state.indexed,
                isPhi: state.isPhi,
                validationJson,
              });
            }}
          >
            {t('form.save')}
          </Button>
        </>
      }
    >
      <div className={styles.formStack}>
        <TextField
          label={t('form.key')}
          hint={t('form.keyHint')}
          value={state.key}
          disabled={keyLocked}
          onChange={(event) => {
            update({ key: event.target.value });
          }}
        />
        <TextField
          label={t('form.label')}
          value={state.label}
          onChange={(event) => {
            update({ label: event.target.value });
          }}
        />
        <Select
          label={t('form.dataType')}
          options={dataTypeOptions(t)}
          value={enumValueName(FieldDataTypeSchema, state.dataType) ?? ''}
          disabled={locked}
          onChange={(event) => {
            const chosen = FieldDataTypeSchema.values.find(
              (value) => value.name === event.target.value,
            );
            if (chosen !== undefined) {
              update({ dataType: chosen.number });
            }
          }}
        />

        {state.dataType === FieldDataType.TEXT ? (
          <TextField
            label={t('form.maxLength')}
            type="number"
            value={state.maxLength}
            onChange={(event) => {
              update({ maxLength: event.target.value });
            }}
          />
        ) : null}

        {hasRange(state.dataType) ? (
          <div className={styles.rangeRow}>
            <TextField
              label={t('form.min')}
              type={numericRange(state.dataType) ? 'number' : 'text'}
              value={state.min}
              onChange={(event) => {
                update({ min: event.target.value });
              }}
            />
            <TextField
              label={t('form.max')}
              type={numericRange(state.dataType) ? 'number' : 'text'}
              value={state.max}
              onChange={(event) => {
                update({ max: event.target.value });
              }}
            />
          </div>
        ) : null}

        {state.dataType === FieldDataType.ENUM ? (
          <TextField
            label={t('form.values')}
            hint={t('form.valuesHint')}
            value={state.values}
            onChange={(event) => {
              update({ values: event.target.value });
            }}
          />
        ) : null}

        <Checkbox
          label={t('form.required')}
          checked={state.required}
          onChange={(event) => {
            update({ required: event.target.checked });
          }}
        />
        {/* `is_phi` is offered only in a lab with PHI mode on; a definition that
            already carries it stays visible (and disabled) so the row is not a
            field the form silently hides. */}
        {phiEnabled || state.isPhi ? (
          <Checkbox
            label={t('form.phi')}
            hint={t('form.phiHint')}
            checked={state.isPhi}
            disabled={!phiEnabled}
            onChange={(event) => {
              update({ isPhi: event.target.checked });
            }}
          />
        ) : null}
        <Checkbox
          label={t('form.indexed')}
          hint={t('form.indexedHint')}
          checked={state.indexed}
          onChange={(event) => {
            update({ indexed: event.target.checked });
          }}
        />

        {messages.length > 0 ? (
          <ul className={styles.problems} role="alert">
            {messages.map((message) => (
              <li key={message}>{message}</li>
            ))}
          </ul>
        ) : null}
      </div>
    </Dialog>
  );
}

function dataTypeOptions(t: TFunction<'itemTypes'>): SelectOption[] {
  return DEFINABLE_DATA_TYPES.map((dataType) => ({
    value: enumValueName(FieldDataTypeSchema, dataType) ?? String(dataType),
    label: dataTypeLabel(t, dataType),
  }));
}

function problemMessage(t: TFunction<'itemTypes'>, code: DefinitionProblemCode): string {
  switch (code) {
    case 'key-required':
      return t('form.problems.keyRequired');
    case 'label-required':
      return t('form.problems.labelRequired');
    case 'phi-and-indexed':
      return t('form.problems.phiIndexed');
    case 'phi-not-enabled':
      return t('form.problems.phiNotEnabled');
    case 'enum-values-required':
      return t('form.problems.enumValuesRequired');
    case 'enum-values-duplicated':
      return t('form.problems.enumValuesDuplicated');
    case 'range-inverted':
      return t('form.problems.rangeInverted');
  }
}

function inheritedOrigin(t: TFunction<'itemTypes'>, inherited: EffectiveField | null): string {
  if (inherited === null) {
    return '';
  }
  return inherited.origin === 'lab' ? t('fields.originLab') : (inherited.originName ?? '');
}

/** The one-line explanation over a tightening form; nothing otherwise. */
function inheritedHint(
  t: TFunction<'itemTypes'>,
  inherited: EffectiveField | null,
  mode: 'create' | 'edit' | 'tighten',
): string | undefined {
  if (mode !== 'tighten' || inherited === null) {
    return undefined;
  }
  return t('form.tightenHint', {
    key: inherited.cfd.key,
    origin: inheritedOrigin(t, inherited),
  });
}

function violationMessage(
  t: TFunction<'itemTypes'>,
  violation: TightenViolation,
  parent: CustomFieldDefinition | null,
  origin: string,
): string {
  if (parent === null) {
    return '';
  }
  const label = parent.label;
  const constraints = parseValidation(parent.validationJson);
  const constraint =
    violation.constraint === 'max_length' ||
    violation.constraint === 'min' ||
    violation.constraint === 'max' ||
    violation.constraint === 'values'
      ? constraintPart(t, constraints, violation.constraint)
      : null;
  switch (violation.code) {
    case 'required-dropped':
      return t('form.problems.requiredDropped', { label, origin });
    case 'phi-dropped':
      return t('form.problems.phiDropped', { label, origin });
    case 'data-type-changed':
      return t('form.problems.dataTypeChanged', { label, origin });
    case 'scope-changed':
      return t('form.problems.scopeChanged', { label, origin });
    case 'constraint-dropped':
      return t('form.problems.constraintDropped', {
        label,
        origin,
        constraint: constraint ?? violation.constraint ?? '',
      });
    case 'constraint-widened':
      return t('form.problems.constraintWidened', {
        label,
        origin,
        constraint: constraint ?? violation.constraint ?? '',
      });
  }
}
