// SPDX-License-Identifier: AGPL-3.0-or-later
import type { DescEnum } from '@bufbuild/protobuf';
import type { TFunction } from 'i18next';
import { isApiError } from './errors';

/**
 * Small display helpers shared by every feature screen (TODO.md G1.2).
 *
 * Everything here is presentation only. G-arch 9: timestamps travel as UTC
 * micros and are converted to the browser's zone *for display*, but the
 * conversion must not happen in the data layer, because a screen that sorts or
 * compares has to keep working on the wire value.
 */

/** Anything the generated `int64` fields can hold. */
export type Micros = bigint | null | undefined;

/**
 * UTC micros to a `Date`, or `null` when the field is unset. `null` (not the
 * epoch) is deliberate: an absent `last_modified_at` and 1970-01-01 are very
 * different things on a sample detail page.
 *
 * `Date` only has millisecond resolution, so the sub-millisecond part is
 * truncated — micros are still the value to store and to send back.
 */
export function microsToDate(micros: Micros): Date | null {
  if (micros === null || micros === undefined) {
    return null;
  }
  return new Date(Number(micros / 1_000n));
}

/** A `Date` back to the wire representation. */
export function dateToMicros(date: Date): bigint {
  return BigInt(date.getTime()) * 1_000n;
}

export interface FormatTimestampOptions {
  readonly locale?: string;
  /** Defaults to the browser's zone, which is the point of G-arch 9. */
  readonly timeZone?: string;
}

/** Format UTC micros for display, or `null` when the field is unset. */
export function formatTimestamp(
  micros: Micros,
  options: FormatTimestampOptions = {},
): string | null {
  const date = microsToDate(micros);
  if (date === null) {
    return null;
  }
  return new Intl.DateTimeFormat(options.locale ?? undefined, {
    dateStyle: 'medium',
    timeStyle: 'short',
    timeZone: options.timeZone,
  }).format(date);
}

/**
 * The proto name of an enum value, e.g. `SAMPLE_STATUS_ACTIVE`.
 *
 * A generated TypeScript enum has prefix-stripped members
 * (`SampleStatus.ACTIVE`), but the JSON wire value — and therefore every
 * translation key and every stored value — uses the full name. This bridges
 * the two, and returns `null` for a number this bundle does not know about
 * (an older SPA against a newer server).
 */
export function enumValueName(schema: DescEnum, value: number): string | null {
  return schema.values.find((candidate) => candidate.number === value)?.name ?? null;
}

/** i18n key for one enum value, e.g. `enums.SampleStatus.SAMPLE_STATUS_ACTIVE`. */
export function enumLabelKey(schema: DescEnum, value: number): string {
  const name = enumValueName(schema, value);
  return name === null ? '' : `enums.${schema.name}.${name}`;
}

/**
 * Translate an enum value. Falls back to the proto name (or the number) rather
 * than showing a raw `enums.X.Y` key, and never throws on an unknown value.
 */
export function enumLabel(t: TFunction, schema: DescEnum, value: number): string {
  const key = enumLabelKey(schema, value);
  if (key !== '') {
    // The key is computed at runtime, so the literal-key inference from
    // src/app/i18next.d.ts cannot apply here.
    const translated = String(t(key as never));
    if (translated !== key) {
      return translated;
    }
  }
  return enumValueName(schema, value) ?? String(value);
}

/**
 * The i18n key describing a failure. Screens that render their own error state
 * use this; everything else uses `apiErrorMessage`.
 */
export function apiErrorMessageKey(error: unknown): string {
  return isApiError(error) ? `errors.${error.code}` : 'errors.UNKNOWN';
}

/**
 * A translated sentence for anything the API layer threw.
 *
 * Deliberately does *not* show `error.message`: that is the gateway's own text,
 * which can name internal paths and identifiers (AGENTS.md §5 — PHI never
 * appears in logs, errors or screenshots). The correlation id is available on
 * the `ApiError` for a support request.
 */
export function apiErrorMessage(t: TFunction, error: unknown): string {
  // Same reason as `enumLabel`: the key depends on the failure at runtime.
  return String(t(apiErrorMessageKey(error) as never));
}
