// SPDX-License-Identifier: AGPL-3.0-or-later
import { describe, expect, it } from 'vitest';
import tokensCss from './tokens.css?raw';

// The `?raw` import above returns the real stylesheet only because
// `vite.config.ts` sets `test.css = true`; under Vitest's default CSS stubbing
// it resolves to `''` and every assertion below would pass vacuously.

/**
 * WCAG AA contrast is an acceptance criterion of G1.3 (TODO.md §Section G →
 * G1.3: "light and dark themes via `prefers-color-scheme` and WCAG AA
 * contrast"). Asserting it in prose is how it silently rots the first time
 * somebody nudges a colour, so this file reads `tokens.css` back and computes
 * the real ratios.
 *
 * Every pair below is checked in **both** themes. Thresholds follow WCAG 2.2:
 * 1.4.3 Contrast (Minimum) for text — 4.5:1 — and 1.4.11 Non-text Contrast for
 * borders, focus rings and other UI affordances — 3:1.
 */

type Theme = Record<string, string>;

/** Pulls `--fmgr-*: value;` declarations out of the light `:root` block and the `prefers-color-scheme: dark` one. */
export function parseTokenThemes(css: string): { light: Theme; dark: Theme } {
  const stripComments = (text: string) => text.replace(/\/\*[\s\S]*?\*\//g, '');
  const clean = stripComments(css);

  const darkBlock = /@media\s*\(prefers-color-scheme:\s*dark\)\s*\{([\s\S]*)\}\s*$/.exec(clean);
  if (!darkBlock) {
    throw new Error('tokens.css must define a @media (prefers-color-scheme: dark) block');
  }
  const light = readDeclarations(clean.slice(0, darkBlock.index));
  const darkOverrides = readDeclarations(darkBlock[1]);
  const dark = { ...light, ...darkOverrides };

  if (Object.keys(darkOverrides).length === 0) {
    throw new Error(
      'the dark block declares no token: a dark theme that reuses every light value is not a dark theme',
    );
  }
  return { light, dark };
}

function readDeclarations(block: string): Theme {
  const theme: Theme = {};
  for (const match of block.matchAll(/--fmgr-([a-z0-9-]+)\s*:\s*([^;]+);/g)) {
    theme[match[1]] = match[2].trim();
  }
  return theme;
}

function channels(color: string): [number, number, number] {
  const value = color.trim().toLowerCase();
  const short = /^#([0-9a-f])([0-9a-f])([0-9a-f])$/.exec(value);
  const long = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/.exec(value);
  let hex: string[];
  if (long) {
    hex = [long[1], long[2], long[3]];
  } else if (short) {
    hex = [`${short[1]}${short[1]}`, `${short[2]}${short[2]}`, `${short[3]}${short[3]}`];
  } else {
    throw new Error(`not an opaque hex colour: ${color}`);
  }
  return hex.map((pair) => Number.parseInt(pair, 16) / 255) as [number, number, number];
}

function relativeLuminance(color: string): number {
  const [r, g, b] = channels(color).map((c) =>
    c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4,
  );
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrastRatio(a: string, b: string): number {
  const [lighter, darker] = [relativeLuminance(a), relativeLuminance(b)].sort((x, y) => y - x);
  return (lighter + 0.05) / (darker + 0.05);
}

interface Requirement {
  /** Foreground token name without the `--fmgr-` prefix. */
  fg: string;
  /** Background token name without the `--fmgr-` prefix. */
  bg: string;
  min: number;
  why: string;
}

const REQUIREMENTS: Requirement[] = [
  { fg: 'color-text', bg: 'color-bg', min: 4.5, why: 'body text on the page background' },
  { fg: 'color-text', bg: 'color-surface', min: 4.5, why: 'body text on a raised surface' },
  {
    fg: 'color-text-muted',
    bg: 'color-bg',
    min: 4.5,
    why: 'secondary text on the page background',
  },
  {
    fg: 'color-text-muted',
    bg: 'color-surface',
    min: 4.5,
    why: 'secondary text on a raised surface',
  },
  { fg: 'color-accent-text', bg: 'color-accent', min: 4.5, why: 'primary button label' },
  {
    fg: 'color-accent-text',
    bg: 'color-accent-hover',
    min: 4.5,
    why: 'primary button label while hovered',
  },
  { fg: 'color-accent', bg: 'color-bg', min: 4.5, why: 'links and accent text' },
  { fg: 'color-danger-text', bg: 'color-danger', min: 4.5, why: 'danger button label' },
  { fg: 'color-danger', bg: 'color-bg', min: 4.5, why: 'destructive text and inline errors' },
  { fg: 'color-success', bg: 'color-success-subtle', min: 4.5, why: 'success badge' },
  { fg: 'color-warning', bg: 'color-warning-subtle', min: 4.5, why: 'warning badge' },
  { fg: 'color-info', bg: 'color-info-subtle', min: 4.5, why: 'info badge' },
  { fg: 'color-neutral', bg: 'color-neutral-subtle', min: 4.5, why: 'neutral badge' },
  { fg: 'color-border', bg: 'color-bg', min: 3, why: 'control borders (WCAG 1.4.11)' },
  {
    fg: 'color-border',
    bg: 'color-surface',
    min: 3,
    why: 'control borders on a surface (WCAG 1.4.11)',
  },
  {
    fg: 'color-focus-ring',
    bg: 'color-bg',
    min: 3,
    why: 'focus ring on the page background (WCAG 1.4.11)',
  },
  {
    fg: 'color-focus-ring',
    bg: 'color-surface',
    min: 3,
    why: 'focus ring on a surface (WCAG 1.4.11)',
  },
];

describe('design tokens', () => {
  const themes = parseTokenThemes(tokensCss);

  it.each([
    ['light', 'light'],
    ['dark', 'dark'],
  ] as const)(
    'defines every token the contrast table needs in the %s theme',
    (_label, themeName) => {
      const theme = themes[themeName];

      for (const { fg, bg } of REQUIREMENTS) {
        expect(theme[fg], `--fmgr-${fg} is missing from the ${themeName} theme`).toBeDefined();
        expect(theme[bg], `--fmgr-${bg} is missing from the ${themeName} theme`).toBeDefined();
      }
    },
  );

  describe.each([
    ['light', 'light'],
    ['dark', 'dark'],
  ] as const)('%s theme', (_label, themeName) => {
    const theme = themes[themeName];

    it.each(REQUIREMENTS)('meets WCAG AA for $why ($fg on $bg)', ({ fg, bg, min }) => {
      const ratio = contrastRatio(theme[fg], theme[bg]);

      expect(
        ratio,
        `${themeName}: ${theme[fg]} on ${theme[bg]} is ${ratio.toFixed(2)}:1, needs ${String(min)}:1`,
      ).toBeGreaterThanOrEqual(min);
    });
  });

  it('uses prefers-color-scheme rather than a JS theme switch (G-arch 1)', () => {
    expect(tokensCss).toMatch(/@media\s*\(prefers-color-scheme:\s*dark\)/);
    expect(tokensCss).toMatch(/color-scheme:\s*light dark/);
  });

  it('keeps the dark theme genuinely different from the light one', () => {
    expect(themes.dark['color-bg']).not.toBe(themes.light['color-bg']);
    expect(themes.dark['color-text']).not.toBe(themes.light['color-text']);
  });
});
