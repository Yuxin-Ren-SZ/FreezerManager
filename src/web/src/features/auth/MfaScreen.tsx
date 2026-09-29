// SPDX-License-Identifier: AGPL-3.0-or-later
import { PlaceholderScreen } from '../../app/pages/PlaceholderScreen';

// Technical identifiers, not user-visible copy: constants keep them out of JSX
// so `i18next/no-literal-string` stays a guard for real text.
const NAMESPACE = 'auth';
const TASK = 'G2.1';

/**
 * Sign in — placeholder created by G1.3 (G-arch 11).
 *
 * TODO(G2.1): replace the body with the real screen. Nothing outside this
 * directory needs to change: the route, the nav entry and the permission gate
 * are already registered in `src/app/route-map.tsx`, and this namespace
 * (`auth`) is already loaded.
 */
export function MfaScreen() {
  return <PlaceholderScreen namespace={NAMESPACE} task={TASK} />;
}
