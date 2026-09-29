// SPDX-License-Identifier: AGPL-3.0-or-later
import type { MessageInitShape } from '@bufbuild/protobuf';
import { useMemo, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useParams } from 'react-router-dom';
import { isApiError } from '../../api/errors';
import { apiErrorMessage } from '../../api/helpers';
import { useCustomFieldDefinitions, useItemTypes } from '../../api/hooks';
import { useCan, useSession } from '../../app/session';
import {
  CustomFieldDefinitionSchema,
  ScopeKind,
  type ItemType,
} from '../../gen/fmgr/v1/item_type_pb';
import { Button, ErrorState, Spinner, useToast } from '../../ui';
import { CustomFieldForm, type DefinitionInput } from './CustomFieldForm';
import { FieldList } from './FieldList';
import { pathLabel } from './fieldSummary';
import { ItemTypeFormDialog, MoveItemTypeDialog } from './ItemTypeDialogs';
import { ItemTypeTree } from './ItemTypeTree';
import {
  buildItemTypeTree,
  canReparent,
  resolveFields,
  resolveInheritedFields,
  type EffectiveField,
  type ReparentRefusal,
} from './itemTypeModel';
import { isItemTypeCycleRefusal, isPhiIndexedRefusal } from './serverErrors';
import { useItemTypeAdmin } from './useItemTypeAdmin';
import styles from './ItemTypesScreen.module.css';

/**
 * The item-type and custom-field admin screen (TODO.md G3.9, PRD §4.3, N5).
 *
 * **What is editable is two permissions, not one.** The route is gated on
 * `item_type.define` (G1.3's route map), and that is right: this is the screen
 * that defines what a sample *is*. `custom_field.define` gates the field
 * catalogue separately, so a role that may shape the taxonomy but not the field
 * catalogue gets a working tree with a read-only field pane. The *reads* are
 * gated on `sample.read` since #69 — a different permission again — so the
 * field pane also has a "cannot read" state, which is where a caller holding
 * `item_type.define` without `sample.read` lands.
 *
 * **Two guards against a cycle, and they are not redundant.** The client
 * refuses a drop into the node's own subtree (`canReparent`), which is the
 * mistake a drag makes; the server refuses the write anyway
 * (`ItemTypeRepositories.cc::check_no_cycle`), which is the one a *stale* tree
 * makes — this screen's data can be seconds old while another admin re-parents
 * the same nodes. That refusal is explained and the tree is reloaded, because
 * after it the tree on screen is known to be wrong.
 */

type FormTarget =
  | { readonly kind: 'create' }
  | { readonly kind: 'edit'; readonly field: EffectiveField }
  | { readonly kind: 'tighten'; readonly field: EffectiveField };

type NodeDialog =
  | { readonly kind: 'create'; readonly parentId: string | null }
  | { readonly kind: 'rename'; readonly node: ItemType }
  | { readonly kind: 'move'; readonly node: ItemType };

export function ItemTypesScreen() {
  const { t } = useTranslation('itemTypes');
  const { t: tCommon } = useTranslation();
  const params = useParams();
  const labId = params.labId ?? '';
  const toast = useToast();
  const { user } = useSession();

  const canDefineTypes = useCan('item_type.define', labId);
  const canDefineFields = useCan('custom_field.define', labId);
  const readsCatalog = useCan('sample.read', labId);
  const phiEnabled = user?.labs.find((lab) => lab.labId === labId)?.isPhiEnabled ?? false;

  const itemTypesQuery = useItemTypes(labId);
  const cfdsQuery = useCustomFieldDefinitions(labId, { enabled: readsCatalog });
  const admin = useItemTypeAdmin(labId);

  // Built inside a `useMemo`: `?? []` allocates a new array per render, which
  // would make every memo below (and every effect that watches them) recompute
  // for no reason.
  const itemTypes = useMemo(() => itemTypesQuery.data?.itemTypes ?? [], [itemTypesQuery.data]);
  const cfds = useMemo(() => cfdsQuery.data?.cfds ?? [], [cfdsQuery.data]);
  const fieldsUnreadable = !readsCatalog || cfdsQuery.isError;

  const tree = useMemo(() => buildItemTypeTree(itemTypes), [itemTypes]);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [collapsed, setCollapsed] = useState<ReadonlySet<string>>(() => new Set());
  const [nodeDialog, setNodeDialog] = useState<NodeDialog | null>(null);
  const [form, setForm] = useState<FormTarget | null>(null);

  // `.at(0)` rather than `[0]`: the empty tree has to stay a `null` selection,
  // and `[0]` is typed as a node even when there is none.
  const selected = selectedId ?? tree.roots.at(0)?.type.id ?? null;
  const selectedNode = itemTypes.find((candidate) => candidate.id === selected);
  const fields = useMemo(
    () => resolveFields(cfds, itemTypes, selected ?? ''),
    [cfds, itemTypes, selected],
  );
  const inheritedFields = useMemo(
    () => resolveInheritedFields(cfds, itemTypes, selected ?? ''),
    [cfds, itemTypes, selected],
  );
  const inherited = fields.filter((field) => field.origin !== 'node');
  const own = fields.filter((field) => field.origin === 'node');

  const lookupInherited = (key: string): EffectiveField | null =>
    inheritedFields.find((field) => field.cfd.key === key) ?? null;

  const applyMove = async (nodeId: string, parentId: string | null) => {
    const node = itemTypes.find((candidate) => candidate.id === nodeId);
    if (node === undefined) {
      return;
    }
    try {
      await admin.updateItemType.mutateAsync({
        itemType: {
          id: node.id,
          labId: node.labId,
          parentId: parentId ?? undefined,
          name: node.name,
        },
      });
      setNodeDialog(null);
      toast.show({ tone: 'success', title: t('move.moved', { name: node.name }) });
    } catch (error) {
      if (isItemTypeCycleRefusal(error)) {
        // The tree this drop was judged against is stale by definition: the
        // server saw a shape we do not have. Reload before anything else.
        admin.invalidate();
        setNodeDialog(null);
        toast.show({ tone: 'danger', title: t('move.refusedCycle') });
        return;
      }
      toast.show({
        tone: 'danger',
        title: t('move.failed'),
        description: apiErrorMessage(tCommon, error),
      });
    }
  };

  const handleDrop = (draggedId: string, targetId: string | null) => {
    const refusal: ReparentRefusal | null = canReparent(itemTypes, draggedId, targetId);
    if (refusal === null) {
      void applyMove(draggedId, targetId);
      return;
    }
    if (refusal === 'unchanged') {
      return;
    }
    toast.show({
      tone: 'warning',
      title: refusal === 'self' ? t('move.refusedSelf') : t('move.refusedDescendant'),
    });
  };

  const submitNode = async (values: {
    readonly name: string;
    readonly parentId: string | null;
  }) => {
    const dialog = nodeDialog;
    if (dialog === null || dialog.kind === 'move') {
      return;
    }
    try {
      if (dialog.kind === 'rename') {
        await admin.updateItemType.mutateAsync({
          itemType: {
            id: dialog.node.id,
            labId: dialog.node.labId,
            parentId: dialog.node.parentId,
            name: values.name,
          },
        });
        toast.show({ tone: 'success', title: t('itemTypeForm.renamed', { name: values.name }) });
      } else {
        const created = await admin.createItemType.mutateAsync({
          labId,
          parentId: values.parentId ?? undefined,
          name: values.name,
        });
        // Open the node that was just created: it has no fields yet, and that
        // is the next thing the admin came here to add.
        setSelectedId(created.itemType?.id ?? null);
        toast.show({ tone: 'success', title: t('itemTypeForm.created', { name: values.name }) });
      }
      setNodeDialog(null);
    } catch (error) {
      // The dialog stays open: the name is the thing to fix, and closing it
      // would throw the typed text away.
      if (isApiError(error) && error.code === 'ALREADY_EXISTS') {
        toast.show({ tone: 'warning', title: t('itemTypeForm.duplicateName') });
        return;
      }
      toast.show({
        tone: 'danger',
        title:
          dialog.kind === 'rename'
            ? t('itemTypeForm.renameTitle', { name: dialog.node.name })
            : t('itemTypeForm.createTitle'),
        description: apiErrorMessage(tCommon, error),
      });
    }
  };

  const submitDefinition = async (input: DefinitionInput) => {
    const target = form;
    if (target === null || selected === null) {
      return;
    }
    const cfd: MessageInitShape<typeof CustomFieldDefinitionSchema> = {
      labId,
      scopeKind: ScopeKind.SAMPLE,
      itemTypeId: selected,
      ...input,
    };
    try {
      if (target.kind === 'edit') {
        // The update carries the definition's own id; `scope_kind` and
        // `item_type_id` are sent as the server expects to read them back.
        await admin.updateDefinition.mutateAsync({
          cfd: { ...cfd, id: target.field.cfd.id },
        });
      } else {
        await admin.createDefinition.mutateAsync({ cfd });
      }
      setForm(null);
      toast.show({ tone: 'success', title: t('form.saved', { label: input.label }) });
    } catch (error) {
      if (isPhiIndexedRefusal(error)) {
        toast.show({ tone: 'warning', title: t('form.serverPhiIndexed') });
        return;
      }
      if (isApiError(error) && error.code === 'ALREADY_EXISTS') {
        toast.show({ tone: 'warning', title: t('form.duplicateKey') });
        return;
      }
      toast.show({
        tone: 'danger',
        title: t('form.save'),
        description: apiErrorMessage(tCommon, error),
      });
    }
  };

  const heading = (
    <header className={styles.header}>
      <h1 className={styles.title}>{t('title')}</h1>
      <p className={styles.subtitle}>{t('subtitle')}</p>
    </header>
  );

  if (itemTypesQuery.isPending) {
    return (
      <div className={styles.screen}>
        {heading}
        <Spinner />
      </div>
    );
  }

  if (itemTypesQuery.isError) {
    return (
      <div className={styles.screen}>
        {heading}
        <ErrorState
          title={t('errors.loadFailed')}
          description={apiErrorMessage(tCommon, itemTypesQuery.error)}
          onRetry={() => {
            void itemTypesQuery.refetch();
          }}
        />
      </div>
    );
  }

  return (
    <div className={styles.screen}>
      {heading}
      <div className={styles.panes}>
        <ItemTypeTree
          tree={tree}
          selectedId={selected}
          collapsed={collapsed}
          editable={canDefineTypes}
          onSelect={setSelectedId}
          onToggle={(id) => {
            setCollapsed((current) => {
              const next = new Set(current);
              if (next.has(id)) {
                next.delete(id);
              } else {
                next.add(id);
              }
              return next;
            });
          }}
          onNew={() => {
            setNodeDialog({ kind: 'create', parentId: selected });
          }}
          onRename={(node) => {
            setNodeDialog({ kind: 'rename', node });
          }}
          onMove={(node) => {
            setNodeDialog({ kind: 'move', node });
          }}
          onDropNode={handleDrop}
          canDrop={(draggedId, targetId) => {
            const refusal = canReparent(itemTypes, draggedId, targetId);
            return refusal === null || refusal === 'unchanged';
          }}
        />

        <section className={styles.detail} aria-label={t('title')}>
          {selectedNode === undefined ? (
            <p className={styles.muted}>{t('detail.select')}</p>
          ) : (
            <>
              <h2 className={styles.nodeTitle}>{selectedNode.name}</h2>
              <p className={styles.path}>
                {t('detail.path', { path: pathLabel(itemTypes, selectedNode.id) })}
              </p>
              <p className={styles.muted}>{t('detail.fieldCount', { count: fields.length })}</p>

              {fieldsUnreadable ? (
                <p className={styles.warning} role="status">
                  {t('fields.unreadable')}
                </p>
              ) : (
                <>
                  <FieldList
                    title={t('fields.inheritedTitle')}
                    fields={inherited}
                    emptyText={t('fields.noneInherited')}
                    actionLabel={canDefineFields ? t('fields.overrideLabel') : undefined}
                    actionAriaLabel={(field) => t('fields.override', { label: field.cfd.label })}
                    onAction={
                      canDefineFields
                        ? (field) => {
                            setForm({ kind: 'tighten', field });
                          }
                        : undefined
                    }
                  />
                  <FieldList
                    title={t('fields.ownTitle')}
                    fields={own}
                    emptyText={t('fields.noneOwn')}
                    actionLabel={canDefineFields ? t('fields.editLabel') : undefined}
                    actionAriaLabel={(field) => t('fields.edit', { label: field.cfd.label })}
                    onAction={
                      canDefineFields
                        ? (field) => {
                            setForm({ kind: 'edit', field });
                          }
                        : undefined
                    }
                  />
                  {canDefineFields ? (
                    <Button
                      onClick={() => {
                        setForm({ kind: 'create' });
                      }}
                    >
                      {t('fields.add')}
                    </Button>
                  ) : null}
                </>
              )}
            </>
          )}
        </section>
      </div>

      {nodeDialog !== null && nodeDialog.kind !== 'move' ? (
        <ItemTypeFormDialog
          mode={nodeDialog.kind === 'create' ? 'create' : 'rename'}
          node={nodeDialog.kind === 'rename' ? nodeDialog.node : undefined}
          types={itemTypes}
          tree={tree}
          defaultParentId={nodeDialog.kind === 'create' ? nodeDialog.parentId : null}
          pending={admin.createItemType.isPending || admin.updateItemType.isPending}
          onOpenChange={() => {
            setNodeDialog(null);
          }}
          onSubmit={(values) => {
            void submitNode(values);
          }}
        />
      ) : null}

      {nodeDialog !== null && nodeDialog.kind === 'move' ? (
        <MoveItemTypeDialog
          node={nodeDialog.node}
          types={itemTypes}
          tree={tree}
          pending={admin.updateItemType.isPending}
          onOpenChange={() => {
            setNodeDialog(null);
          }}
          onSubmit={(parentId) => {
            void applyMove(nodeDialog.node.id, parentId);
          }}
        />
      ) : null}

      {form !== null ? (
        <CustomFieldForm
          mode={form.kind}
          labId={labId}
          nodeId={selected ?? ''}
          phiEnabled={phiEnabled}
          existing={form.kind === 'create' ? undefined : form.field.cfd}
          lookupInherited={lookupInherited}
          pending={admin.createDefinition.isPending || admin.updateDefinition.isPending}
          onOpenChange={() => {
            setForm(null);
          }}
          onSubmit={(input) => {
            void submitDefinition(input);
          }}
        />
      ) : null}
    </div>
  );
}
