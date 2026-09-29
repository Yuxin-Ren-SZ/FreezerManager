// SPDX-License-Identifier: AGPL-3.0-or-later
import type { ReactNode } from 'react';
import shell from '../../locales/en/shell.json';
import { lazyScreen } from './lazyScreen';
import type { PermissionKey } from './permissions';

/**
 * Every screen, fetched when its route is first rendered — not when the app
 * boots (issue #64).
 *
 * These were static imports until G3.2: a screen that renders TanStack Table
 * pulls TanStack Table and TanStack Virtual in with it, and with all 17 screens
 * statically imported the entry chunk carried every one of them. G3.1 left
 * 196.0 KiB of a 250 KiB budget; G3.2 alone took that to 235.4 KiB, so G3.3 and
 * G3.4 would have failed `npm run build` and the failure would have read as
 * "this screen is too big" rather than "the entry is carrying every screen".
 *
 * The shell is deliberately *not* deferred: `AppShell`, the nav and the kit
 * they use stay eager, so the frame paints immediately and only the screen
 * shows a fallback. `scripts/check-bundle-size.mjs` fails the build if a screen
 * here goes back to a static import.
 *
 * `lazyScreen` names the export so a rename is a type error at this line rather
 * than a blank route at runtime.
 */
const AccountScreen = lazyScreen(
  () => import('../features/account/AccountScreen'),
  'AccountScreen',
);
const AuditScreen = lazyScreen(() => import('../features/audit/AuditScreen'), 'AuditScreen');
const LoginScreen = lazyScreen(() => import('../features/auth/LoginScreen'), 'LoginScreen');
const MfaScreen = lazyScreen(() => import('../features/auth/MfaScreen'), 'MfaScreen');
const HomeScreen = lazyScreen(() => import('../features/home/HomeScreen'), 'HomeScreen');
const ImportScreen = lazyScreen(() => import('../features/import/ImportScreen'), 'ImportScreen');
const ItemTypesScreen = lazyScreen(
  () => import('../features/item-types/ItemTypesScreen'),
  'ItemTypesScreen',
);
const BoxScreen = lazyScreen(() => import('../features/layout/BoxScreen'), 'BoxScreen');
const LayoutAdminScreen = lazyScreen(
  () => import('../features/layout/LayoutAdminScreen'),
  'LayoutAdminScreen',
);
const LayoutTreeScreen = lazyScreen(
  () => import('../features/layout/LayoutTreeScreen'),
  'LayoutTreeScreen',
);
const LookupScreen = lazyScreen(() => import('../features/lookup/LookupScreen'), 'LookupScreen');
const MembersScreen = lazyScreen(() => import('../features/members/MembersScreen'), 'MembersScreen');
const SampleBrowserScreen = lazyScreen(
  () => import('../features/samples/SampleBrowserScreen'),
  'SampleBrowserScreen',
);
const SampleCreateScreen = lazyScreen(
  () => import('../features/samples/SampleCreateScreen'),
  'SampleCreateScreen',
);
const SampleDetailScreen = lazyScreen(
  () => import('../features/samples/SampleDetailScreen'),
  'SampleDetailScreen',
);
const ScanScreen = lazyScreen(() => import('../features/scan/ScanScreen'), 'ScanScreen');
const SharesScreen = lazyScreen(() => import('../features/shares/SharesScreen'), 'SharesScreen');

/** Where a nav entry is grouped. */
export type NavSection = 'primary' | 'admin' | 'account';

/** A literal key into `shell.json`'s `navLabels`, derived from the JSON itself. */
export type NavLabelKey = `navLabels.${keyof typeof shell.navLabels}`;

export interface AppRoute {
  /** Stable id: used as the React key, in tests, and to find a route by name. */
  id: string;
  /** Absolute path, with `:labId` / `:sampleId` params as React Router spells them. */
  path: string;
  /** The TODO.md item that replaces this placeholder. */
  task: string;
  /**
   * The screen. Since issue #64 every screen here is a lazy element, so the
   * router has to render it inside a `Suspense` boundary — `router.tsx` does,
   * with the fallback in the shell's content area.
   */
  element: ReactNode;
  /**
   * Any-of permissions required to see the screen. `null` means "any signed-in
   * user". Login screens are `null` and live outside the shell.
   */
  permissions: readonly PermissionKey[] | null;
  /** `true` when the path contains `:labId` and therefore needs a lab. */
  scoped: boolean;
  /** `'shell'` renders inside the app shell; `'bare'` is a full-page route. */
  layout: 'shell' | 'bare';
  /** `null` for screens that are not in the side nav (detail and edit views). */
  nav: { section: NavSection; labelKey: NavLabelKey } | null;
}

/**
 * The route map from TODO.md §Section G, "Route map", in one place.
 *
 * G-arch 11: G1.3 registers a placeholder route and a nav entry for every
 * screen, so a feature task edits only its own `features/<name>/` directory.
 * Keeping the routes and the nav in one array — rather than a `<Routes>` tree
 * plus a hand-written nav — is what makes that promise hold: a screen cannot be
 * reachable but invisible, or in the nav but 404, without a test noticing.
 *
 * The permission column is the *future* permission of each screen, taken from
 * `src/core/permissions.h`. G-arch 8: it is UX only, and the server is the
 * enforcement point.
 */
export const ROUTES: readonly AppRoute[] = [
  {
    id: 'login',
    path: '/login',
    task: 'G2.1',
    element: <LoginScreen />,
    permissions: null,
    scoped: false,
    layout: 'bare',
    nav: null,
  },
  {
    id: 'login-mfa',
    path: '/login/mfa',
    task: 'G2.1',
    element: <MfaScreen />,
    permissions: null,
    scoped: false,
    layout: 'bare',
    nav: null,
  },
  {
    id: 'home',
    path: '/',
    task: 'G4.2',
    element: <HomeScreen />,
    permissions: null,
    scoped: false,
    layout: 'shell',
    nav: { section: 'primary', labelKey: 'navLabels.home' },
  },
  {
    id: 'lookup',
    path: '/lookup',
    task: 'G3.5',
    element: <LookupScreen />,
    permissions: ['sample.read'],
    scoped: false,
    layout: 'shell',
    nav: { section: 'primary', labelKey: 'navLabels.lookup' },
  },
  {
    id: 'samples',
    path: '/labs/:labId/samples',
    task: 'G3.2',
    element: <SampleBrowserScreen />,
    permissions: ['sample.read'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'primary', labelKey: 'navLabels.samples' },
  },
  {
    id: 'sample-new',
    path: '/labs/:labId/samples/new',
    task: 'G3.3',
    element: <SampleCreateScreen />,
    permissions: ['sample.write'],
    scoped: true,
    layout: 'shell',
    nav: null,
  },
  {
    id: 'sample-detail',
    path: '/labs/:labId/samples/:sampleId',
    task: 'G3.3',
    element: <SampleDetailScreen />,
    // Read, then edit in place: the detail view must open for a ReadOnly member
    // even though the edit affordances inside it will not.
    permissions: ['sample.read'],
    scoped: true,
    layout: 'shell',
    nav: null,
  },
  {
    id: 'layout',
    path: '/labs/:labId/layout',
    task: 'G3.1',
    element: <LayoutTreeScreen />,
    // There is no `box.read` in the catalog; browsing the layout is covered by
    // `sample.read`, which is what lets a member find their own box.
    permissions: ['sample.read'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'primary', labelKey: 'navLabels.layout' },
  },
  {
    id: 'box',
    path: '/labs/:labId/boxes/:boxId',
    task: 'G3.4',
    element: <BoxScreen />,
    permissions: ['sample.read'],
    scoped: true,
    layout: 'shell',
    nav: null,
  },
  {
    id: 'scan',
    path: '/labs/:labId/scan',
    task: 'G3.6',
    element: <ScanScreen />,
    permissions: ['sample.checkout'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'primary', labelKey: 'navLabels.scan' },
  },
  {
    id: 'csv-import',
    path: '/labs/:labId/import',
    task: 'G3.7',
    element: <ImportScreen />,
    permissions: ['sample.write'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'primary', labelKey: 'navLabels.import' },
  },
  {
    id: 'shares',
    path: '/labs/:labId/shares',
    task: 'G3.13',
    element: <SharesScreen />,
    permissions: ['share.request', 'share.approve'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'primary', labelKey: 'navLabels.shares' },
  },
  {
    id: 'admin-layout',
    path: '/labs/:labId/admin/layout',
    task: 'G3.8',
    element: <LayoutAdminScreen />,
    permissions: ['freezer.configure'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'admin', labelKey: 'navLabels.adminLayout' },
  },
  {
    id: 'item-types',
    path: '/labs/:labId/admin/item-types',
    task: 'G3.9',
    element: <ItemTypesScreen />,
    permissions: ['item_type.define'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'admin', labelKey: 'navLabels.itemTypes' },
  },
  {
    id: 'members',
    path: '/labs/:labId/admin/members',
    task: 'G3.10',
    element: <MembersScreen />,
    permissions: ['user.invite', 'user.manage_roles'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'admin', labelKey: 'navLabels.members' },
  },
  {
    id: 'audit',
    path: '/labs/:labId/audit',
    task: 'G3.12',
    element: <AuditScreen />,
    permissions: ['audit.read'],
    scoped: true,
    layout: 'shell',
    nav: { section: 'admin', labelKey: 'navLabels.audit' },
  },
  {
    id: 'account',
    path: '/account',
    task: 'G3.11',
    element: <AccountScreen />,
    permissions: null,
    scoped: false,
    layout: 'shell',
    nav: { section: 'account', labelKey: 'navLabels.account' },
  },
];

/** The screens that appear in the side nav, in the order they were declared. */
export const NAV_ROUTES: readonly AppRoute[] = ROUTES.filter((route) => route.nav !== null);

/**
 * Substitutes `:labId` in a scoped route, so the nav can link to the lab the
 * user is actually in. Returns `null` when the route needs a lab and there is
 * none — the nav renders those entries as unavailable rather than linking to
 * `/labs//samples`.
 */
export function routeHref(route: AppRoute, labId: string | null): string | null {
  if (!route.scoped) {
    return route.path;
  }
  if (labId === null) {
    return null;
  }
  return route.path.replace(':labId', encodeURIComponent(labId));
}
