// SPDX-License-Identifier: AGPL-3.0-or-later
import { lazy, type ComponentType, type LazyExoticComponent } from 'react';

/**
 * Whether React could render `value` as an element type.
 *
 * `memo()` and `forwardRef()` hand back objects rather than functions, so a
 * plain `typeof value === 'function'` would reject two ways of writing a screen
 * that are perfectly valid.
 */
function isComponent(value: unknown): value is ComponentType {
  if (typeof value === 'function') {
    return true;
  }
  return typeof value === 'object' && value !== null && '$$typeof' in value;
}

/**
 * A route screen that is fetched when its route is first rendered (issue #64).
 *
 * The shell is what paints first — a spinner for the app frame would be a
 * regression in feel — so `src/app/shell/` and the kit it uses stay eager. A
 * *screen* is different: the user chose to open it, so a brief fallback inside
 * the frame is worth not shipping every screen in the entry chunk. Measured
 * cost of the alternative: with every screen static, G3.2's TanStack Table
 * screen alone put the entry at 235.4 KiB of a 250 KiB budget.
 *
 * `name` names the export rather than taking the whole module, for two reasons:
 * `module.default` would force every screen to grow a second, default export,
 * and a mistyped name is then a type error at the call site instead of a blank
 * route. The runtime check below covers the case the type system cannot — a
 * module the caller only knows as a plain record.
 */
export function lazyScreen<
  TModule extends Record<string, unknown>,
  TKey extends keyof TModule & string,
>(load: () => Promise<TModule>, name: TKey): LazyExoticComponent<ComponentType> {
  return lazy(async () => {
    const module = await load();
    const screen = module[name];
    if (!isComponent(screen)) {
      throw new Error(
        `lazyScreen: ${JSON.stringify(name)} is not a component export of the lazily ` +
          'imported module — the route would render nothing.',
      );
    }
    return { default: screen };
  });
}
