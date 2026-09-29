// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * Joins CSS-module class names, dropping the falsy ones.
 *
 * A 4-line local helper instead of `clsx`/`classnames`: G-arch 3 makes every
 * new runtime dependency a `lock:deps` decision, and this is the whole feature
 * we need from either package.
 */
export function classNames(...values: readonly (string | false | null | undefined)[]): string {
  return values.filter((value): value is string => Boolean(value)).join(' ');
}
