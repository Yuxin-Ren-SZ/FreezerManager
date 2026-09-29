// SPDX-License-Identifier: AGPL-3.0-or-later
import { useId, useRef, useState } from 'react';
import type { KeyboardEvent, ReactNode } from 'react';
import styles from './Tabs.module.css';
import { classNames } from './classNames';

export interface TabItem {
  id: string;
  label: string;
  content: ReactNode;
  disabled?: boolean;
}

export interface TabsProps {
  /** Accessible name for the tab list, e.g. "Sample sections". */
  label: string;
  items: readonly TabItem[];
  /** Uncontrolled initial selection; defaults to the first enabled tab. */
  defaultValue?: string;
  /** Controlled selection. */
  value?: string;
  onValueChange?: (id: string) => void;
  className?: string;
}

function firstEnabledId(items: readonly TabItem[]): string {
  return items.find((item) => item.disabled !== true)?.id ?? '';
}

/**
 * A tab set with the ARIA "automatic activation" keyboard model: the arrow keys
 * move focus *and* select, Home/End jump to the ends, and disabled tabs are
 * skipped rather than focused-and-inert.
 *
 * The `tablist` is a single tab stop (roving `tabindex`), which is what makes
 * `Tab` from the address bar land on the tab set rather than on each tab. No
 * Radix dependency: the pattern is a `role`, three attributes and one key
 * handler, all of which are testable here.
 */
export function Tabs({ label, items, defaultValue, value, onValueChange, className }: TabsProps) {
  const baseId = useId();
  const tabRefs = useRef(new Map<string, HTMLButtonElement>());
  const [internalValue, setInternalValue] = useState(() => defaultValue ?? firstEnabledId(items));
  const selectedId = value ?? internalValue;
  const activeItem = items.find((item) => item.id === selectedId && item.disabled !== true);

  const select = (id: string) => {
    if (value === undefined) {
      setInternalValue(id);
    }
    onValueChange?.(id);
  };

  const focusAndSelect = (id: string) => {
    tabRefs.current.get(id)?.focus();
    select(id);
  };

  const handleKeyDown = (event: KeyboardEvent<HTMLButtonElement>) => {
    const enabled = items.filter((item) => item.disabled !== true);
    if (enabled.length === 0) {
      return;
    }
    const currentIndex = enabled.findIndex((item) => item.id === selectedId);

    switch (event.key) {
      case 'ArrowRight':
      case 'ArrowDown': {
        event.preventDefault();
        focusAndSelect(enabled[(currentIndex + 1) % enabled.length].id);
        break;
      }
      case 'ArrowLeft':
      case 'ArrowUp': {
        event.preventDefault();
        focusAndSelect(enabled[(currentIndex - 1 + enabled.length) % enabled.length].id);
        break;
      }
      case 'Home': {
        event.preventDefault();
        focusAndSelect(enabled[0].id);
        break;
      }
      case 'End': {
        event.preventDefault();
        focusAndSelect(enabled[enabled.length - 1].id);
        break;
      }
      default:
        break;
    }
  };

  return (
    <div className={classNames(styles.root, className)}>
      {/* The key handler sits on each tab rather than on the tablist: the tabs
          are the focusable elements in the roving-tabindex pattern, and a
          keydown listener on a non-focusable container is dead code that
          `jsx-a11y/interactive-supports-focus` rightly flags. */}
      <div className={styles.tablist} role="tablist" aria-label={label}>
        {items.map((item) => {
          const selected = item.id === selectedId && item.disabled !== true;
          return (
            <button
              key={item.id}
              ref={(element) => {
                if (element) {
                  tabRefs.current.set(item.id, element);
                } else {
                  tabRefs.current.delete(item.id);
                }
              }}
              type="button"
              role="tab"
              id={`${baseId}-tab-${item.id}`}
              aria-controls={`${baseId}-panel-${item.id}`}
              aria-selected={selected}
              // Roving tabindex: the selected tab is the tab stop.
              tabIndex={selected ? 0 : -1}
              disabled={item.disabled}
              className={classNames(styles.tab, selected && styles.selected)}
              onKeyDown={handleKeyDown}
              onClick={() => {
                if (item.disabled !== true) {
                  select(item.id);
                }
              }}
            >
              {item.label}
            </button>
          );
        })}
      </div>
      {activeItem ? (
        <div
          className={styles.panel}
          role="tabpanel"
          id={`${baseId}-panel-${activeItem.id}`}
          aria-labelledby={`${baseId}-tab-${activeItem.id}`}
          tabIndex={0}
        >
          {activeItem.content}
        </div>
      ) : null}
    </div>
  );
}
