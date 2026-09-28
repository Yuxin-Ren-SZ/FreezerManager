// SPDX-License-Identifier: AGPL-3.0-or-later
import 'i18next';
import type common from '../../locales/en/common.json';

/**
 * Makes `t()` keys type-checked against `locales/en/common.json`, so a typo or
 * a key that was renamed in the JSON fails `npm run typecheck` instead of
 * rendering the key at runtime.
 */
declare module 'i18next' {
  interface CustomTypeOptions {
    defaultNS: 'common';
    resources: {
      common: typeof common;
    };
  }
}
