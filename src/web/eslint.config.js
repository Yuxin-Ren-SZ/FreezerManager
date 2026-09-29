// SPDX-License-Identifier: AGPL-3.0-or-later
import js from '@eslint/js';
import prettier from 'eslint-config-prettier';
import i18next from 'eslint-plugin-i18next';
import jsxA11y from 'eslint-plugin-jsx-a11y';
import reactHooks from 'eslint-plugin-react-hooks';
import globals from 'globals';
import tseslint from 'typescript-eslint';

// G-arch 2: typescript-eslint strict + react-hooks + jsx-a11y +
// i18next/no-literal-string, with Prettier owning formatting.
//
// `eslint-config-prettier` stays last: it switches off every stylistic rule
// that would fight the formatter.

// eslint-plugin-react-hooks ships its flat config under different keys
// depending on the major version; take whichever one exists. If neither does,
// fail the config load: spreading `undefined` would silently drop every
// react-hooks rule while lint still reported success.
const reactHooksRecommended =
  reactHooks.configs['recommended-latest'] ?? reactHooks.configs.flat?.recommended;

if (Object.keys(reactHooksRecommended?.rules ?? {}).length === 0) {
  throw new Error(
    'eslint.config.js: eslint-plugin-react-hooks exposes no flat recommended config ' +
      '(looked for configs["recommended-latest"] and configs.flat.recommended), so the ' +
      'react-hooks rules would be zero. Fix the lookup above before relying on this lint run.',
  );
}

const I18N_EXCLUDED_ATTRIBUTES = [
  // Technical, never user-visible text.
  'className',
  'class',
  'style',
  'data-testid',
  'id',
  'name',
  'htmlFor',
  'key',
  'role',
  'type',
  'to',
  'target',
  'rel',
  'aria-hidden',
];

export default tseslint.config(
  {
    ignores: ['dist/**', 'node_modules/**', 'coverage/**', 'src/gen/**'],
  },

  {
    files: ['**/*.{js,mjs,cjs,ts,tsx}'],
    extends: [js.configs.recommended, ...tseslint.configs.recommended],
    languageOptions: {
      ecmaVersion: 2023,
      sourceType: 'module',
    },
  },

  {
    // Type-aware "strict" linting. Scoped to `src/**` on purpose: those are the
    // files a tsconfig owns, so the project service can type them.
    files: ['src/**/*.{ts,tsx}'],
    extends: [...tseslint.configs.strictTypeChecked, ...tseslint.configs.stylisticTypeChecked],
    languageOptions: {
      globals: globals.browser,
      parserOptions: {
        projectService: true,
        tsconfigRootDir: import.meta.dirname,
      },
    },
    plugins: {
      'jsx-a11y': jsxA11y,
      'react-hooks': reactHooks,
      i18next,
    },
    rules: {
      ...jsxA11y.flatConfigs.recommended.rules,
      ...reactHooksRecommended?.rules,

      // Every user-visible string comes from locales/<lng>/<ns>.json.
      'i18next/no-literal-string': [
        'error',
        {
          mode: 'jsx-only',
          'jsx-attributes': { exclude: I18N_EXCLUDED_ATTRIBUTES },
        },
      ],

      // React 19 + the new JSX transform: no need to import React.
      '@typescript-eslint/no-unused-vars': [
        'error',
        { argsIgnorePattern: '^_', varsIgnorePattern: '^_' },
      ],
    },
  },

  {
    // Node-side files: build config and the scripts npm runs.
    files: ['*.config.{js,ts}', 'eslint.config.js', 'scripts/**/*.mjs'],
    languageOptions: {
      globals: globals.node,
    },
  },

  {
    // Tests assert on literal text on purpose: it is the fixture, not copy
    // that ships to a user. `i18next/no-literal-string` still applies to every
    // file under `src/` that is not a test, which is where it earns its keep.
    files: ['**/*.test.{ts,tsx}'],
    rules: {
      'i18next/no-literal-string': 'off',
    },
  },

  prettier,
);
