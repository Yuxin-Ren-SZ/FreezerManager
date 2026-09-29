// SPDX-License-Identifier: AGPL-3.0-or-later
import { useTranslation } from 'react-i18next';
import { NavLink, useParams } from 'react-router-dom';
import { can, useSession } from '../session';
import { useLabs } from '../labs';
import { NAV_ROUTES, routeHref, type AppRoute, type NavSection } from '../route-map';
import styles from './SideNav.module.css';
import { classNames } from '../../ui';

const SECTIONS: readonly NavSection[] = ['primary', 'admin', 'account'];

const SECTION_LABELS: Record<NavSection, `nav.${NavSection}`> = {
  primary: 'nav.primary',
  admin: 'nav.admin',
  account: 'nav.account',
};

function isVisible(
  route: AppRoute,
  user: ReturnType<typeof useSession>['user'],
  labId: string | null,
): boolean {
  if (route.permissions === null) {
    return true;
  }
  // Any-of: a screen that needs `user.invite` *or* `user.manage_roles` shows up
  // for somebody who holds only one of them.
  return route.permissions.some((permission) =>
    can(user, permission, route.scoped ? labId : undefined),
  );
}

/**
 * The side nav, built from the route map rather than written out by hand.
 *
 * A screen that the user's role cannot reach is *absent*, not disabled: a nav
 * full of greyed-out entries tells an attacker exactly which screens exist and
 * tells a member nothing useful. G-arch 8 — the server still refuses the calls.
 *
 * Below 48 rem the stylesheet turns this into a horizontal strip above the
 * content, which is what keeps the layout usable at 360 px.
 */
export function SideNav() {
  const { t } = useTranslation('shell');
  const { user } = useSession();
  const { selectedLabId } = useLabs();
  const params = useParams();
  const labId = params.labId ?? selectedLabId ?? null;

  return (
    <nav className={styles.nav} aria-label={t('nav.label')}>
      {SECTIONS.map((section) => {
        const entries = NAV_ROUTES.filter((route) => route.nav?.section === section).flatMap(
          (route) => {
            const href = routeHref(route, labId);
            if (href === null || !isVisible(route, user, labId)) {
              return [];
            }
            return [{ route, href }];
          },
        );

        if (entries.length === 0) {
          return null;
        }

        return (
          <div key={section} className={styles.section}>
            <h2 className={styles.sectionTitle}>{t(SECTION_LABELS[section])}</h2>
            <ul className={styles.list}>
              {entries.map(({ route, href }) => (
                <li key={route.id}>
                  <NavLink
                    to={href}
                    // `end` on `/` stops the home link being "active" everywhere.
                    end={route.path === '/'}
                    className={({ isActive }) => classNames(styles.link, isActive && styles.active)}
                  >
                    {t(route.nav?.labelKey ?? 'navLabels.home')}
                  </NavLink>
                </li>
              ))}
            </ul>
          </div>
        );
      })}
    </nav>
  );
}
