// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import { enumLabel } from '../../api/helpers';
import { ContainerKindSchema } from '../../gen/fmgr/v1/box_pb';
import { Badge } from '../../ui';
import type { LayoutNode } from './layoutModel';
import { boxPath } from './paths';
import styles from './LayoutTree.module.css';

/**
 * The collapsible layout tree (TODO.md G3.1).
 *
 * A disclosure list, not an ARIA `tree`: `role="tree"` promises roving focus
 * and arrow-key navigation, and a widget that claims those roles without
 * implementing them is worse for a keyboard user than plain buttons and links.
 * Here a container row is a `<button aria-expanded>` that owns its children,
 * and a box row is a `<Link>` — because selecting a box *is* navigation to the
 * box view (G3.4), and a link is what the browser can open in a new tab.
 *
 * Collapsed branches stay in the DOM with `hidden` on the `<ul>` rather than
 * being unmounted, so `aria-controls` always points at a real element and the
 * browser can restore the state cheaply.
 */

export interface LayoutTreeProps {
  readonly nodes: readonly LayoutNode[];
  readonly labId: string;
  /** Ids whose children are hidden. Everything is open until a user closes it. */
  readonly collapsed: ReadonlySet<string>;
  readonly onToggle: (nodeId: string) => void;
}

export function LayoutTree({ nodes, labId, collapsed, onToggle }: LayoutTreeProps) {
  const { t } = useTranslation('layout');

  return (
    <ul className={styles.tree} aria-label={t('tree.label')}>
      {nodes.map((node) => (
        <LayoutTreeItem
          key={node.id}
          node={node}
          labId={labId}
          collapsed={collapsed}
          onToggle={onToggle}
        />
      ))}
    </ul>
  );
}

interface LayoutTreeItemProps {
  readonly node: LayoutNode;
  readonly labId: string;
  readonly collapsed: ReadonlySet<string>;
  readonly onToggle: (nodeId: string) => void;
}

// Decoration, not copy: no translation, and `aria-hidden` on the element. They
// are escapes rather than literal glyphs so `i18next/no-literal-string` stays a
// guard for text a user actually reads.
const COLLAPSED_GLYPH = '\u25B8'; // ▸
const EXPANDED_GLYPH = '\u25BE'; // ▾

function childrenId(nodeId: string): string {
  return `layout-children-${nodeId}`;
}

function LayoutTreeItem({ node, labId, collapsed, onToggle }: LayoutTreeItemProps) {
  const { t } = useTranslation('layout');
  // Container kind names live in `enums.ContainerKind.*` under the *default*
  // namespace (`common`), which is where `apiErrorMessage`'s `errors.*` keys
  // are too — hence a second, default-namespace `t` next to the layout one.
  const { t: tCommon } = useTranslation();

  if (node.kind === 'box') {
    return (
      <li className={styles.item}>
        <Link className={styles.row} to={boxPath(labId, node.id)}>
          <span className={styles.glyph} aria-hidden="true" />
          <span className={styles.label}>{node.label}</span>
          {node.positionCount === null ? null : (
            <Badge className={styles.count}>
              {t('tree.positionCount', { count: node.positionCount })}
            </Badge>
          )}
        </Link>
      </li>
    );
  }

  const hasChildren = node.children.length > 0;
  const isCollapsed = collapsed.has(node.id);

  return (
    <li className={styles.item}>
      {hasChildren ? (
        <button
          type="button"
          className={styles.row}
          aria-expanded={!isCollapsed}
          aria-controls={childrenId(node.id)}
          onClick={() => {
            onToggle(node.id);
          }}
        >
          <span className={styles.glyph} aria-hidden="true">
            {isCollapsed ? COLLAPSED_GLYPH : EXPANDED_GLYPH}
          </span>
          <span className={styles.label}>{node.label}</span>
          {node.containerKind === null ? null : (
            <span className={styles.detail}>
              {enumLabel(tCommon, ContainerKindSchema, node.containerKind)}
            </span>
          )}
          <Badge className={styles.count}>{t('tree.boxCount', { count: node.boxCount })}</Badge>
        </button>
      ) : (
        // A leaf container has nothing to disclose, so it is not a button: a
        // control that does nothing when pressed is a lie about the UI.
        <div className={styles.row}>
          <span className={styles.glyph} aria-hidden="true" />
          <span className={styles.label}>{node.label}</span>
          {node.containerKind === null ? null : (
            <span className={styles.detail}>
              {enumLabel(tCommon, ContainerKindSchema, node.containerKind)}
            </span>
          )}
          <Badge className={styles.count}>{t('tree.boxCount', { count: node.boxCount })}</Badge>
        </div>
      )}

      {hasChildren ? (
        <ul id={childrenId(node.id)} className={styles.children} hidden={isCollapsed}>
          {node.children.map((child) => (
            <LayoutTreeItem
              key={child.id}
              node={child}
              labId={labId}
              collapsed={collapsed}
              onToggle={onToggle}
            />
          ))}
        </ul>
      ) : null}
    </li>
  );
}
