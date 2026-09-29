// SPDX-License-Identifier: AGPL-3.0-or-later
import type { TFunction } from 'i18next';
import { useTranslation } from 'react-i18next';
import type { CustomFieldDefinition } from '../../gen/fmgr/v1/item_type_pb';
import { Badge, Button } from '../../ui';
import { constraintSummary, dataTypeLabel, originLabel } from './fieldSummary';
import type { EffectiveField } from './itemTypeModel';
import styles from './ItemTypesScreen.module.css';

/**
 * One list of field definitions (TODO.md G3.9).
 *
 * Used twice on the detail pane, and the difference between the two uses is the
 * acceptance criterion: **inherited** rows are read-only — no input, no edit
 * action — while a node's own rows carry the edit action. The only thing a user
 * can do to an inherited field is *override* it, which creates a definition on
 * the open node that narrows the inherited one; it never rewrites the ancestor,
 * which other item types share.
 */

export interface FieldListProps {
  readonly title: string;
  readonly fields: readonly EffectiveField[];
  readonly emptyText: string;
  /** The action on each row, when the caller may define fields at all. */
  readonly actionLabel?: string;
  readonly actionAriaLabel?: (field: EffectiveField) => string;
  readonly onAction?: (field: EffectiveField) => void;
}

export function FieldList({
  title,
  fields,
  emptyText,
  actionLabel,
  actionAriaLabel,
  onAction,
}: FieldListProps) {
  const { t } = useTranslation('itemTypes');
  const headingId = `field-list-${title.replace(/\s+/g, '-').toLowerCase()}`;

  return (
    <section className={styles.fieldSection} aria-labelledby={headingId}>
      <h3 id={headingId} className={styles.sectionTitle}>
        {title}
      </h3>
      {fields.length === 0 ? (
        <p className={styles.muted}>{emptyText}</p>
      ) : (
        <ul className={styles.fieldList}>
          {fields.map((field) => (
            <li key={field.cfd.id} className={styles.fieldRow}>
              <span className={styles.fieldLabel}>{field.cfd.label}</span>
              <code className={styles.fieldKey}>{field.cfd.key}</code>
              <span className={styles.fieldType}>{dataTypeLabel(t, field.cfd.dataType)}</span>
              <Constraints cfd={field.cfd} />
              <Badges cfd={field.cfd} />
              <span className={styles.fieldOrigin}>{originText(t, field)}</span>
              {actionLabel !== undefined && onAction !== undefined ? (
                <Button
                  size="sm"
                  variant="ghost"
                  aria-label={actionAriaLabel?.(field) ?? actionLabel}
                  onClick={() => {
                    onAction(field);
                  }}
                >
                  {actionLabel}
                </Button>
              ) : null}
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}

function Constraints({ cfd }: { readonly cfd: CustomFieldDefinition }) {
  const { t } = useTranslation('itemTypes');
  const parts = constraintSummary(t, cfd);
  if (parts.length === 0) {
    return null;
  }
  return <span className={styles.fieldConstraints}>{parts.join(' \u00b7 ')}</span>;
}

function Badges({ cfd }: { readonly cfd: CustomFieldDefinition }) {
  const { t } = useTranslation('itemTypes');
  return (
    <span className={styles.badges}>
      {cfd.required ? <Badge tone="info">{t('fields.required')}</Badge> : null}
      {cfd.isPhi ? <Badge tone="warning">{t('fields.phi')}</Badge> : null}
      {cfd.indexed ? <Badge tone="neutral">{t('fields.indexed')}</Badge> : null}
    </span>
  );
}

/** Where the row comes from, and — for an override — what it narrows. */
function originText(t: TFunction<'itemTypes'>, field: EffectiveField): string {
  if (field.origin !== 'node') {
    return originLabel(t, field);
  }
  if (field.tightenedFrom === null) {
    return '';
  }
  return t('fields.tightens', {
    parent: field.tightenedFrom.label,
    origin: field.tightenedFromName ?? t('fields.originLab'),
  });
}
