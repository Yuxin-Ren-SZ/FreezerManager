// SPDX-License-Identifier: AGPL-3.0-or-later
import { create } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import i18n from '../app/i18n';
import { SampleSchema, SampleStatus, SampleStatusSchema } from '../gen/fmgr/v1/sample_pb';
import { ApiError } from './errors';
import {
  apiErrorMessage,
  apiErrorMessageKey,
  dateToMicros,
  enumLabel,
  enumValueName,
  formatTimestamp,
  microsToDate,
} from './helpers';

/** A fixed instant: 2025-09-27T00:00:00Z, which is 2025-09-26 in New York. */
const SEPT_27_UTC_MICROS = 1_758_931_200_000_000n;

describe('micros <-> Date (G-arch 9: UTC micros on the wire, local only for display)', () => {
  it('converts UTC micros to a Date at that instant', () => {
    const date = microsToDate(SEPT_27_UTC_MICROS);

    expect(date?.toISOString()).toBe('2025-09-27T00:00:00.000Z');
  });

  it('Returns null for an unset timestamp instead of the epoch', () => {
    expect(microsToDate(null)).toBeNull();
    expect(microsToDate(undefined)).toBeNull();
  });

  it('truncates to millisecond resolution, since Date has no finer resolution', () => {
    expect(microsToDate(1_999n)?.getTime()).toBe(1);
  });

  it('round-trips a Date back to micros', () => {
    const date = microsToDate(SEPT_27_UTC_MICROS);
    if (date === null) {
      throw new Error('microsToDate returned null for a timestamp that is set');
    }
    expect(dateToMicros(date)).toBe(SEPT_27_UTC_MICROS);
  });

  it('reads the timestamp field of a generated message', () => {
    const sample = create(SampleSchema, { id: 's-1', lastModifiedAt: { unixMicros: 1_000n } });

    expect(microsToDate(sample.lastModifiedAt?.unixMicros)?.getTime()).toBe(1);
  });
});

describe('formatTimestamp', () => {
  it('renders in the requested zone, so the same instant differs by zone', () => {
    const utc = formatTimestamp(SEPT_27_UTC_MICROS, { locale: 'en-US', timeZone: 'UTC' });
    const newYork = formatTimestamp(SEPT_27_UTC_MICROS, {
      locale: 'en-US',
      timeZone: 'America/New_York',
    });

    expect(utc).toContain('Sep 27, 2025');
    expect(newYork).toContain('Sep 26, 2025');
  });

  it('returns null when the timestamp is unset', () => {
    expect(formatTimestamp(null, { locale: 'en-US', timeZone: 'UTC' })).toBeNull();
  });
});

describe('enum labels', () => {
  it('maps a numeric enum value to its proto name', () => {
    expect(enumValueName(SampleStatusSchema, SampleStatus.ACTIVE)).toBe('SAMPLE_STATUS_ACTIVE');
    expect(enumValueName(SampleStatusSchema, SampleStatus.CHECKED_OUT)).toBe(
      'SAMPLE_STATUS_CHECKED_OUT',
    );
  });

  it('returns null for a value this bundle does not know', () => {
    expect(enumValueName(SampleStatusSchema, 99)).toBeNull();
  });

  it('translates an enum value through the enums.<Enum>.<PROTO_NAME> key', () => {
    expect(enumLabel(i18n.t, SampleStatusSchema, SampleStatus.ACTIVE)).toBe('Active');
    expect(enumLabel(i18n.t, SampleStatusSchema, SampleStatus.TOMBSTONED)).toBe('Deleted');
  });

  it('falls back to the proto name when no translation exists', () => {
    expect(enumLabel(i18n.t, SampleStatusSchema, 99)).toBe('99');
  });
});

describe('apiErrorMessage', () => {
  it('maps every gRPC code to a translated sentence', () => {
    const codes = [
      'UNAUTHENTICATED',
      'PERMISSION_DENIED',
      'NOT_FOUND',
      'ALREADY_EXISTS',
      'FAILED_PRECONDITION',
      'INVALID_ARGUMENT',
      'UNAVAILABLE',
      'INTERNAL',
    ] as const;

    for (const code of codes) {
      const message = apiErrorMessage(i18n.t, new ApiError(code, 'raw server text'));
      // Resolved, not just returned as the key.
      expect(message, code).not.toBe(`errors.${code}`);
      expect(message, code).not.toContain('raw server text');
      expect(message.length, code).toBeGreaterThan(0);
    }
  });

  it('translates every code the gateway can emit, not only the common ones', async () => {
    const { GRPC_CODES } = await import('./errors');

    for (const code of GRPC_CODES) {
      if (code === 'OK') continue;
      expect(i18n.t(`errors.${code}`), code).not.toBe(`errors.${code}`);
    }
  });

  it('never shows the raw server message, which can carry internal detail', () => {
    const error = new ApiError('INTERNAL', 'sqlite3: /var/lib/fmgr/db.sqlite is locked');

    expect(apiErrorMessage(i18n.t, error)).not.toContain('sqlite3');
  });

  it('handles a non-ApiError value with the generic message', () => {
    expect(apiErrorMessage(i18n.t, new Error('boom'))).toBe(i18n.t('errors.UNKNOWN'));
    expect(apiErrorMessageKey(new Error('boom'))).toBe('errors.UNKNOWN');
  });

  it('exposes the message key so a screen can pick its own presentation', () => {
    expect(apiErrorMessageKey(new ApiError('PERMISSION_DENIED', 'no'))).toBe(
      'errors.PERMISSION_DENIED',
    );
  });
});
