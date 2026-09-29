// SPDX-License-Identifier: AGPL-3.0-or-later
import { PlaceholderScreen } from '../../app/pages/PlaceholderScreen';

// Technical identifiers, not user-visible copy: constants keep them out of JSX
// so `i18next/no-literal-string` stays a guard for real text.
const NAMESPACE = 'lookup';
const TASK = 'G3.5';

/**
 * Lookup — placeholder created by G1.3 (G-arch 11).
 *
 * TODO(G3.5): replace the body with the real screen. Nothing outside this
 * directory needs to change: the route, the nav entry and the permission gate
 * are already registered in `src/app/route-map.tsx`, and this namespace
 * (`lookup`) is already loaded.
 */
export function LookupScreen() {
  return <PlaceholderScreen namespace={NAMESPACE} task={TASK} />;
}
