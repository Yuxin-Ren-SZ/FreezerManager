// SPDX-License-Identifier: AGPL-3.0-or-later
import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import type { TFunction } from 'i18next';
import type { ItemType } from '../../gen/fmgr/v1/item_type_pb';
import { Button, Dialog, Select, TextField, type SelectOption } from '../../ui';
import { pathLabel } from './fieldSummary';
import { canReparent, type ItemTypeNode, type ItemTypeTree } from './itemTypeModel';
import styles from './ItemTypesScreen.module.css';

/**
 * The two dialogs that write `ItemTypeService` (TODO.md G3.9): create/rename a
 * node, and move one.
 *
 * The move dialog is not decoration over the drag: it is the keyboard path to
 * the same `item-type/update` call, and its option list is built with the same
 * guard the drop handlers use, so a target that would close a cycle is not
 * offered at all rather than refused after it is picked.
 */

/** Depth-first node ids in the order the tree renders them. */
function flatten(nodes: readonly ItemTypeNode[]): readonly string[] {
  return nodes.flatMap((node) => [node.type.id, ...flatten(node.children)]);
}

function parentOptions(
  t: TFunction<'itemTypes'>,
  types: readonly ItemType[],
  tree: ItemTypeTree,
  hidden: ReadonlySet<string>,
): readonly SelectOption[] {
  return [
    { value: '', label: t('itemTypeForm.root') },
    ...flatten(tree.roots)
      .filter((id) => !hidden.has(id))
      .map((id) => ({ value: id, label: pathLabel(types, id) })),
  ];
}

export interface ItemTypeFormDialogProps {
  readonly mode: 'create' | 'rename';
  /** The node being renamed; absent when creating. */
  readonly node?: ItemType;
  readonly types: readonly ItemType[];
  readonly tree: ItemTypeTree;
  /** Preselected parent for a new node — usually the selected one. */
  readonly defaultParentId: string | null;
  readonly pending: boolean;
  readonly onOpenChange: (open: boolean) => void;
  readonly onSubmit: (values: { readonly name: string; readonly parentId: string | null }) => void;
}

/** Create a node anywhere, or rename one. */
export function ItemTypeFormDialog({
  mode,
  node,
  types,
  tree,
  defaultParentId,
  pending,
  onOpenChange,
  onSubmit,
}: ItemTypeFormDialogProps) {
  const { t } = useTranslation('itemTypes');
  const [name, setName] = useState(node?.name ?? '');
  const [parentId, setParentId] = useState(defaultParentId ?? '');

  const isCreate = mode === 'create';
  const options = parentOptions(t, types, tree, new Set());

  return (
    <Dialog
      open
      onOpenChange={onOpenChange}
      size="sm"
      title={
        isCreate
          ? t('itemTypeForm.createTitle')
          : t('itemTypeForm.renameTitle', { name: node?.name ?? '' })
      }
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
            onClick={() => {
              onSubmit({ name, parentId: parentId === '' ? null : parentId });
            }}
          >
            {isCreate ? t('itemTypeForm.create') : t('itemTypeForm.rename')}
          </Button>
        </>
      }
    >
      <div className={styles.formStack}>
        <TextField
          label={t('itemTypeForm.name')}
          value={name}
          onChange={(event) => {
            setName(event.target.value);
          }}
        />
        {isCreate ? (
          <Select
            label={t('itemTypeForm.parent')}
            options={options}
            value={parentId}
            onChange={(event) => {
              setParentId(event.target.value);
            }}
          />
        ) : null}
      </div>
    </Dialog>
  );
}

export interface MoveItemTypeDialogProps {
  readonly node: ItemType;
  readonly types: readonly ItemType[];
  readonly tree: ItemTypeTree;
  readonly pending: boolean;
  readonly onOpenChange: (open: boolean) => void;
  readonly onSubmit: (parentId: string | null) => void;
}

/** Re-parent a node without a mouse. */
export function MoveItemTypeDialog({
  node,
  types,
  tree,
  pending,
  onOpenChange,
  onSubmit,
}: MoveItemTypeDialogProps) {
  const { t } = useTranslation('itemTypes');
  const [parentId, setParentId] = useState(node.parentId ?? '');

  // Every node that would close a cycle is left out of the list, so the guard
  // is visible as an absence rather than as a refusal after the fact. The walk
  // is `canReparent`'s, so the list and the drop handler cannot disagree.
  const hidden = new Set<string>([node.id]);
  for (const candidate of types) {
    if (canReparent(types, node.id, candidate.id) === 'descendant') {
      hidden.add(candidate.id);
    }
  }
  const options = parentOptions(t, types, tree, hidden);

  return (
    <Dialog
      open
      onOpenChange={onOpenChange}
      size="sm"
      title={t('move.title', { name: node.name })}
      footer={
        <>
          <Button
            disabled={pending}
            onClick={() => {
              onOpenChange(false);
            }}
          >
            {t('move.cancel')}
          </Button>
          <Button
            variant="primary"
            loading={pending}
            onClick={() => {
              onSubmit(parentId === '' ? null : parentId);
            }}
          >
            {t('move.submit')}
          </Button>
        </>
      }
    >
      <Select
        label={t('move.parent')}
        options={options}
        value={parentId}
        onChange={(event) => {
          setParentId(event.target.value);
        }}
      />
    </Dialog>
  );
}
