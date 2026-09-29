// SPDX-License-Identifier: AGPL-3.0-or-later
import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import account from '../../locales/en/account.json';
import audit from '../../locales/en/audit.json';
import auth from '../../locales/en/auth.json';
import common from '../../locales/en/common.json';
import home from '../../locales/en/home.json';
import csvImport from '../../locales/en/import.json';
import itemTypes from '../../locales/en/itemTypes.json';
import layout from '../../locales/en/layout.json';
import lookup from '../../locales/en/lookup.json';
import members from '../../locales/en/members.json';
import samples from '../../locales/en/samples.json';
import scan from '../../locales/en/scan.json';
import shares from '../../locales/en/shares.json';
import shell from '../../locales/en/shell.json';
import ui from '../../locales/en/ui.json';

/**
 * i18next setup (G-arch 1, G-arch 11).
 *
 * Translations are bundled from `locales/<lng>/<namespace>.json` instead of
 * being fetched at runtime: the SPA is same-origin only and served with a
 * strict CSP (G0.3), so an HTTP backend would be one more moving part for no
 * gain.
 *
 * Every namespace below is a directory in `src/features/` (G-arch 11), plus
 * three that G1.3 owns: `common` (G1.1's), `ui` (the shared kit) and `shell`
 * (the top bar, nav, error pages and the placeholder screen). A language is
 * added by adding a directory.
 *
 * The `import` directory's namespace is `csvImport` here because `import` is a
 * reserved word in a destructuring/import position; the JSON file keeps the
 * directory's name.
 */
export const defaultNS = 'common';

export const resources = {
  en: {
    common,
    ui,
    shell,
    auth,
    home,
    lookup,
    samples,
    layout,
    scan,
    csvImport,
    itemTypes,
    members,
    account,
    audit,
    shares,
  },
} as const;

void i18n.use(initReactI18next).init({
  lng: 'en',
  fallbackLng: 'en',
  ns: Object.keys(resources.en),
  defaultNS,
  resources,
  // The resources above are in the bundle, so there is nothing to wait for:
  // initialising synchronously (i18next `initAsync: false`) avoids a frame of
  // raw keys on first paint and makes component tests deterministic.
  initAsync: false,
  interpolation: {
    // React already escapes everything it renders.
    escapeValue: false,
  },
});

export default i18n;
