// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import { useTranslation } from 'react-i18next';
import { Navigate, useLocation, useParams } from 'react-router-dom';
import { ErrorState, Spinner } from '../ui';
import { useLabs } from './labs';
import { can, useSession, type LabMembership } from './session';
import type { AppRoute } from './route-map';
import { NoAccess } from './pages/NoAccess';
import styles from './guards.module.css';

/**
 * Whether there is a session at all.
 *
 * The three states are kept apart on purpose:
 *
 * - `loading` shows a spinner rather than the login page, or every reload would
 *   flash the sign-in form before `auth/whoami` answered.
 * - `error` (the request itself failed) shows a retry, *not* a redirect: a
 *   dropped connection must not look like being signed out.
 * - `unauthenticated` redirects to `/login?next=…`, and `next` is the path plus
 *   query string G2.1 will validate as same-origin before using it.
 */
export function RequireSession({ children }: { children: ReactNode }) {
  const { t } = useTranslation('shell');
  const { status, error, reload } = useSession();
  const location = useLocation();

  if (status === 'loading') {
    return (
      <div className={styles.centred}>
        <Spinner size="lg" label={t('session.loading')} />
      </div>
    );
  }

  if (status === 'error') {
    return (
      <main className={styles.page}>
        <ErrorState
          title={t('session.errorTitle')}
          description={error?.message ?? t('session.errorBody')}
          onRetry={reload}
        />
      </main>
    );
  }

  if (status === 'unauthenticated') {
    const next = `${location.pathname}${location.search}`;
    return <Navigate to={`/login?next=${encodeURIComponent(next)}`} replace />;
  }

  return children;
}

/**
 * Whether the signed-in user may see this screen (G-arch 8: UX, not security).
 *
 * The lab comes from the route param when there is one — a link to
 * `/labs/other-lab/samples` must be judged against *that* lab's membership, not
 * against whichever lab the picker happens to be showing — and falls back to
 * the selected lab for screens like `/lookup`.
 */
export function RouteGuard({ route, children }: { route: AppRoute; children: ReactNode }) {
  const { user } = useSession();
  const { selectedLabId } = useLabs();
  const params = useParams();
  const labId = params.labId ?? selectedLabId ?? null;

  // The lab check comes *before* the "no permission required" shortcut, not
  // after it. Today no route is both `permissions: null` and `scoped: true`, so
  // the order makes no difference — which is exactly why it is worth fixing
  // now: the first such route would otherwise render with no lab check at all,
  // and a `:labId` nothing validated is how a screen ends up showing another
  // lab's data shell.
  let membership: LabMembership | null = null;
  if (route.scoped) {
    // Signed in but a member of nothing: the screen exists and there is no lab
    // to show in it, which is a different fix from "ask an admin".
    if (labId === null || user === null || user.labs.length === 0) {
      return <NoAccess noLab />;
    }
    // A link to somebody else's lab is not a permission problem either.
    membership = user.labs.find((entry) => entry.labId === labId) ?? null;
    if (membership === null) {
      return <NoAccess notAMember />;
    }
  }

  if (route.permissions === null) {
    return children;
  }

  const allowed = route.permissions.some((permission) =>
    membership === null ? can(user, permission) : membership.permissions.includes(permission),
  );

  if (!allowed) {
    return <NoAccess permissions={route.permissions} />;
  }

  return children;
}
