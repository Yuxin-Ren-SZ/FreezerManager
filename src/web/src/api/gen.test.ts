// SPDX-License-Identifier: AGPL-3.0-or-later
import { create, toJson } from '@bufbuild/protobuf';
import { describe, expect, it } from 'vitest';
import * as audit from '../gen/fmgr/v1/audit_pb';
import * as auth from '../gen/fmgr/v1/auth_pb';
import { LoginRequestSchema, LoginResponseSchema } from '../gen/fmgr/v1/auth_pb';
import * as box from '../gen/fmgr/v1/box_pb';
import * as commonTypes from '../gen/fmgr/v1/common/types_pb';
import { TimestampSchema } from '../gen/fmgr/v1/common/types_pb';
import * as itemType from '../gen/fmgr/v1/item_type_pb';
import * as lab from '../gen/fmgr/v1/lab_pb';
import * as role from '../gen/fmgr/v1/role_pb';
import * as sample from '../gen/fmgr/v1/sample_pb';
import { SampleSchema, SampleStatus } from '../gen/fmgr/v1/sample_pb';
import * as session from '../gen/fmgr/v1/session_pb';
import * as share from '../gen/fmgr/v1/share_pb';

/**
 * Guards for the generated code itself (TODO.md G1.2, G-arch 4).
 *
 * G1.1 shipped `npm run gen` as a stub that only created an empty `src/gen/`,
 * which is the dangerous shape: with no generated types, `build`, `test` and
 * `typecheck` still pass as long as nothing imports them yet, and the deception
 * only surfaces in a feature task much later. These tests import from `src/gen/`
 * eagerly, so an empty or stale `src/gen/` fails the suite at collection time,
 * and they pin the JSON wire contract the gateway actually speaks.
 */
describe('generated proto types', () => {
  it('generates a module for every proto service file (not an empty src/gen/)', () => {
    const modules = {
      audit,
      auth,
      box,
      commonTypes,
      itemType,
      lab,
      role,
      sample,
      session,
      share,
    };

    // A stub `gen` leaves `src/gen/` empty; these imports would then fail the
    // file at collection time, and the length check catches a module that
    // resolves but exports nothing.
    for (const [name, generated] of Object.entries(modules)) {
      expect(Object.keys(generated).length, `${name}_pb should export schemas`).toBeGreaterThan(0);
    }
  });

  it('serializes with proto (snake_case) field names, as JsonProtoMapping.cc does', () => {
    const request = create(LoginRequestSchema, { email: 'a@example.test', password: 'x' });

    expect(toJson(LoginRequestSchema, request, { useProtoFieldName: true })).toEqual({
      email: 'a@example.test',
      password: 'x',
    });
  });

  it('emits int64 as a JSON string and enums as names', () => {
    const at = create(TimestampSchema, { unixMicros: 1_758_931_200_000_000n });
    expect(toJson(TimestampSchema, at, { useProtoFieldName: true })).toEqual({
      unix_micros: '1758931200000000',
    });

    // protoc-gen-es strips the enum-name prefix in TypeScript
    // (`SampleStatus.ACTIVE`), but the wire keeps the full proto name.
    expect(SampleStatus.ACTIVE).toBe(1);
    const sample = create(SampleSchema, { status: SampleStatus.ACTIVE });
    const json = toJson(SampleSchema, sample, { useProtoFieldName: true });
    expect(json).toMatchObject({ status: 'SAMPLE_STATUS_ACTIVE' });
    // Default values are omitted, exactly like the gateway's JsonPrintOptions.
    expect(json).not.toHaveProperty('name');
  });

  it('keeps proto camelCase field names available on the generated message type', () => {
    const response = create(LoginResponseSchema, {
      sessionToken: 'redacted',
      sessionId: 's-1',
      userId: 'u-1',
      mfaRequired: false,
    });

    expect(response.sessionId).toBe('s-1');
    expect(response.mfaRequired).toBe(false);
  });
});
