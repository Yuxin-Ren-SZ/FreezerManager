// SPDX-License-Identifier: AGPL-3.0-or-later
import i18n from 'i18next';
import { initReactI18next } from 'react-i18next';
import common from '../../locales/en/common.json';
import ui from '../../locales/en/ui.json';

/**
 * i18next setup (G-arch 1, G-arch 11).
 *
 * Translations are bundled from `locales/<lng>/<namespace>.json` instead of
 * being fetched at runtime: the SPA is same-origin only and served with a
 * strict CSP (G0.3), so an HTTP backend would be one more moving part for no
 * gain. Every feature owns one namespace file (G-arch 11) and `ui` is the
 * shared kit's; a language is added by adding a directory.
 */
export const defaultNS = 'common';

export const resources = {
  en: { common, ui },
} as const;

void i18n.use(initReactI18next).init({
  lng: 'en',
  fallbackLng: 'en',
  ns: ['common', 'ui'],
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
