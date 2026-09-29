// SPDX-License-Identifier: AGPL-3.0-or-later
import { useState } from 'react';
import type { DragEvent } from 'react';
import { useTranslation } from 'react-i18next';
import type { ItemType } from '../../gen/fmgr/v1/item_type_pb';
import { Button, IconButton, classNames } from '../../ui';
import type { ItemTypeNode, ItemTypeTree } from './itemTypeModel';
import styles from './ItemTypesScreen.module.css';

/**
 * The taxonomy tree (TODO.md G3.9).
 *
 * A disclosure list of buttons, not an ARIA `tree`: `role="tree"` promises
 * roving focus and arrow-key navigation, and claiming those without
 * implementing them is worse for a keyboard user than plain buttons (the same
 * choice G3.1's layout tree records).
 *
 * **Two paths to one mutation.** Dragging a node onto another re-parents it;
 * the "Move…" action on every row opens a dialog over the same
 * `item-type/update` call. The drag is the quick path, the dialog is the one a
 * keyboard or screen-reader user gets — and it is also what makes the guard
 * testable without synthesising a drop.
 *
 * **The guard runs in the drop handlers, not in the mutation.** `canDrop` is
 * consulted on `dragover` so the browser shows a no-drop cursor, and again on
 * `drop` before anything is sent: a drop a user cannot see must not become a
 * request they cannot explain (G3.1's `layoutModel` guards, same standard).
 */

export interface ItemTypeTreeProps {
  readonly tree: ItemTypeTree;
  readonly selectedId: string | null;
  /** Ids whose children are hidden. Everything is open until a user closes it. */
  readonly collapsed: ReadonlySet<string>;
  /** `item_type.define`: without it the tree renders but offers no edits. */
  readonly editable: boolean;
  readonly onSelect: (id: string) => void;
  readonly onToggle: (id: string) => void;
  readonly onNew: () => void;
  readonly onRename: (node: ItemType) => void;
  readonly onMove: (node: ItemType) => void;
  readonly onDropNode: (draggedId: string, targetId: string | null) => void;
  /** Whether that drop would be accepted; `false` leaves the browser's no-drop. */
  readonly canDrop: (draggedId: string, targetId: string | null) => boolean;
}

// Decoration, not copy: escapes rather than literal glyphs so
// `i18next/no-literal-string` stays a guard for text a user actually reads.
const COLLAPSED_GLYPH = '\u25B8'; // ▸
const EXPANDED_GLYPH = '\u25BE'; // ▾

function childrenId(nodeId: string): string {
  return `item-type-children-${nodeId}`;
}

export function ItemTypeTree({
  tree,
  selectedId,
  collapsed,
  editable,
  onSelect,
  onToggle,
  onNew,
  onRename,
  onMove,
  onDropNode,
  canDrop,
}: ItemTypeTreeProps) {
  const { t } = useTranslation('itemTypes');
  const [draggedId, setDraggedId] = useState<string | null>(null);

  const dropHandlers = (targetId: string | null) => ({
    onDragOver: (event: DragEvent<HTMLElement>) => {
      if (draggedId !== null && canDrop(draggedId, targetId)) {
        event.preventDefault();
      }
    },
    onDrop: (event: DragEvent<HTMLElement>) => {
      event.preventDefault();
      const dragged = draggedId;
      setDraggedId(null);
      if (dragged !== null) {
        onDropNode(dragged, targetId);
      }
    },
  });

  return (
    <div className={styles.treePane}>
      <div className={styles.treeToolbar}>
        {editable ? (
          <Button size="sm" onClick={onNew}>
            {t('tree.new')}
          </Button>
        ) : null}
        {editable ? (
          <div className={styles.rootDrop} {...dropHandlers(null)}>
            {t('tree.dropToRoot')}
          </div>
        ) : null}
      </div>

      {tree.roots.length === 0 ? (
        <p className={styles.muted}>{t('tree.empty')}</p>
      ) : (
        <ul className={styles.tree} aria-label={t('tree.label')}>
          {tree.roots.map((node) => (
            <TreeItem
              key={node.type.id}
              node={node}
              selectedId={selectedId}
              collapsed={collapsed}
              editable={editable}
              draggedId={draggedId}
              onSelect={onSelect}
              onToggle={onToggle}
              onRename={onRename}
              onMove={onMove}
              onDragStart={setDraggedId}
              dropHandlers={dropHandlers}
            />
          ))}
        </ul>
      )}

      {tree.cyclic.length > 0 ? (
        // Not hidden: the data has a problem, and quietly re-rooting two nodes
        // would leave an admin with a tree that disagrees with the database.
        <p className={styles.warning} role="status">
          {t('tree.cyclic', { count: tree.cyclic.length })}
        </p>
      ) : null}
      {tree.orphaned.length > 0 ? (
        <p className={styles.warning} role="status">
          {t('tree.orphaned', { count: tree.orphaned.length })}
        </p>
      ) : null}
    </div>
  );
}

interface DropHandlers {
  onDragOver: (event: DragEvent<HTMLElement>) => void;
  onDrop: (event: DragEvent<HTMLElement>) => void;
}

interface TreeItemProps {
  readonly node: ItemTypeNode;
  readonly selectedId: string | null;
  readonly collapsed: ReadonlySet<string>;
  readonly editable: boolean;
  readonly draggedId: string | null;
  readonly onSelect: (id: string) => void;
  readonly onToggle: (id: string) => void;
  readonly onRename: (node: ItemType) => void;
  readonly onMove: (node: ItemType) => void;
  readonly onDragStart: (id: string | null) => void;
  readonly dropHandlers: (targetId: string | null) => DropHandlers;
}

function TreeItem({
  node,
  selectedId,
  collapsed,
  editable,
  draggedId,
  onSelect,
  onToggle,
  onRename,
  onMove,
  onDragStart,
  dropHandlers,
}: TreeItemProps) {
  const { t } = useTranslation('itemTypes');
  const name = node.type.name === '' ? node.type.id : node.type.name;
  const hasChildren = node.children.length > 0;
  const isCollapsed = collapsed.has(node.type.id);
  const isSelected = selectedId === node.type.id;

  return (
    <li className={styles.treeItem}>
      <div
        className={classNames(styles.treeRow, draggedId === node.type.id && styles.dragging)}
        data-node-id={node.type.id}
        draggable={editable}
        onDragStart={(event) => {
          onDragStart(node.type.id);
          event.dataTransfer.setData('text/plain', node.type.id);
          event.dataTransfer.effectAllowed = 'move';
        }}
        onDragEnd={() => {
          onDragStart(null);
        }}
        {...dropHandlers(node.type.id)}
      >
        {hasChildren ? (
          <IconButton
            size="sm"
            label={isCollapsed ? t('tree.expand', { name }) : t('tree.collapse', { name })}
            aria-expanded={!isCollapsed}
            aria-controls={childrenId(node.type.id)}
            onClick={() => {
              onToggle(node.type.id);
            }}
          >
            {isCollapsed ? COLLAPSED_GLYPH : EXPANDED_GLYPH}
          </IconButton>
        ) : (
          <span className={styles.glyph} aria-hidden="true" />
        )}

        <button
          type="button"
          className={styles.nodeName}
          aria-current={isSelected ? 'true' : undefined}
          onClick={() => {
            onSelect(node.type.id);
          }}
        >
          {name}
        </button>

        {editable ? (
          <span className={styles.rowActions}>
            <Button
              size="sm"
              variant="ghost"
              aria-label={t('tree.move', { name })}
              onClick={() => {
                onMove(node.type);
              }}
            >
              {t('tree.moveLabel')}
            </Button>
            <Button
              size="sm"
              variant="ghost"
              aria-label={t('tree.rename', { name })}
              onClick={() => {
                onRename(node.type);
              }}
            >
              {t('tree.renameLabel')}
            </Button>
          </span>
        ) : null}
      </div>

      {hasChildren ? (
        <ul id={childrenId(node.type.id)} className={styles.tree} hidden={isCollapsed}>
          {node.children.map((child) => (
            <TreeItem
              key={child.type.id}
              node={child}
              selectedId={selectedId}
              collapsed={collapsed}
              editable={editable}
              draggedId={draggedId}
              onSelect={onSelect}
              onToggle={onToggle}
              onRename={onRename}
              onMove={onMove}
              onDragStart={onDragStart}
              dropHandlers={dropHandlers}
            />
          ))}
        </ul>
      ) : null}
    </li>
  );
}
