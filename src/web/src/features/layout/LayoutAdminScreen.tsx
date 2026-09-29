// SPDX-License-Identifier: AGPL-3.0-or-later
import { PlaceholderScreen } from '../../app/pages/PlaceholderScreen';

// Technical identifiers, not user-visible copy: constants keep them out of JSX
// so `i18next/no-literal-string` stays a guard for real text.
const NAMESPACE = 'layout';
const TASK = 'G3.8';

/**
 * Storage layout — placeholder created by G1.3 (G-arch 11).
 *
 * TODO(G3.8): replace the body with the real screen. Nothing outside this
 * directory needs to change: the route, the nav entry and the permission gate
 * are already registered in `src/app/route-map.tsx`, and this namespace
 * (`layout`) is already loaded.
 */
export function LayoutAdminScreen() {
  return <PlaceholderScreen namespace={NAMESPACE} task={TASK} />;
}
