// SPDX-License-Identifier: AGPL-3.0-or-later
import 'i18next';
import type { defaultNS, resources } from './i18n';

/**
 * Makes `t()` keys type-checked against the bundled locale JSON, so a typo or a
 * key that was renamed in `locales/en/*.json` fails `npm run typecheck` instead
 * of rendering the raw key at runtime.
 *
 * The namespace list comes from `resources` in `i18n.ts` rather than from a
 * hand-maintained copy here: registering a namespace file there is enough, and
 * `useTranslation('<name>')` with a namespace that was never registered is a
 * type error.
 */
declare module 'i18next' {
  interface CustomTypeOptions {
    defaultNS: typeof defaultNS;
    resources: (typeof resources)['en'];
  }
}
