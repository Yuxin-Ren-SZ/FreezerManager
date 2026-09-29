// SPDX-License-Identifier: AGPL-3.0-or-later
import {
  ExportAuditLogRequestSchema,
  ExportAuditLogResponseSchema,
  GetAuditEventRequestSchema,
  GetAuditEventResponseSchema,
  ListAuditEventsRequestSchema,
  ListAuditEventsResponseSchema,
  VerifyAuditChainRequestSchema,
  VerifyAuditChainResponseSchema,
} from '../gen/fmgr/v1/audit_pb';
import {
  CreateApiTokenRequestSchema,
  CreateApiTokenResponseSchema,
  ListApiTokensRequestSchema,
  ListApiTokensResponseSchema,
  LoginRequestSchema,
  LoginResponseSchema,
  LogoutRequestSchema,
  LogoutResponseSchema,
  RevokeApiTokenRequestSchema,
  RevokeApiTokenResponseSchema,
  SubmitMfaRequestSchema,
  SubmitMfaResponseSchema,
} from '../gen/fmgr/v1/auth_pb';
import {
  ArchiveBoxRequestSchema,
  ArchiveBoxResponseSchema,
  ArchiveFreezerRequestSchema,
  ArchiveFreezerResponseSchema,
  ArchiveStorageContainerRequestSchema,
  ArchiveStorageContainerResponseSchema,
  CreateBoxRequestSchema,
  CreateBoxResponseSchema,
  CreateBoxTypeRequestSchema,
  CreateBoxTypeResponseSchema,
  CreateContainerTypeRequestSchema,
  CreateContainerTypeResponseSchema,
  CreateFreezerRequestSchema,
  CreateFreezerResponseSchema,
  CreateStorageContainerRequestSchema,
  CreateStorageContainerResponseSchema,
  GetBoxRequestSchema,
  GetBoxResponseSchema,
  GetFreezerRequestSchema,
  GetFreezerResponseSchema,
  ListBoxTypesRequestSchema,
  ListBoxTypesResponseSchema,
  ListBoxesRequestSchema,
  ListBoxesResponseSchema,
  ListContainerTypesRequestSchema,
  ListContainerTypesResponseSchema,
  ListFreezersRequestSchema,
  ListFreezersResponseSchema,
  ListStorageContainersRequestSchema,
  ListStorageContainersResponseSchema,
  UpdateBoxRequestSchema,
  UpdateBoxResponseSchema,
  UpdateFreezerRequestSchema,
  UpdateFreezerResponseSchema,
  UpdateStorageContainerRequestSchema,
  UpdateStorageContainerResponseSchema,
} from '../gen/fmgr/v1/box_pb';
import {
  ArchiveCfdRequestSchema,
  ArchiveCfdResponseSchema,
  ArchiveItemTypeRequestSchema,
  ArchiveItemTypeResponseSchema,
  CreateCfdRequestSchema,
  CreateCfdResponseSchema,
  CreateItemTypeRequestSchema,
  CreateItemTypeResponseSchema,
  GetItemTypeRequestSchema,
  GetItemTypeResponseSchema,
  ListCfdsRequestSchema,
  ListCfdsResponseSchema,
  ListItemTypesRequestSchema,
  ListItemTypesResponseSchema,
  UpdateCfdRequestSchema,
  UpdateCfdResponseSchema,
  UpdateItemTypeRequestSchema,
  UpdateItemTypeResponseSchema,
} from '../gen/fmgr/v1/item_type_pb';
import {
  CreateLabRequestSchema,
  CreateLabResponseSchema,
  EnablePhiRequestSchema,
  EnablePhiResponseSchema,
  GetLabRequestSchema,
  GetLabResponseSchema,
  InviteMemberRequestSchema,
  InviteMemberResponseSchema,
  ListLabsRequestSchema,
  ListLabsResponseSchema,
  ListMembersRequestSchema,
  ListMembersResponseSchema,
  RevokeMembershipRequestSchema,
  RevokeMembershipResponseSchema,
  UpdateLabRequestSchema,
  UpdateLabResponseSchema,
} from '../gen/fmgr/v1/lab_pb';
import {
  ArchiveRoleRequestSchema,
  ArchiveRoleResponseSchema,
  CreateRoleRequestSchema,
  CreateRoleResponseSchema,
  GetRoleRequestSchema,
  GetRoleResponseSchema,
  GrantPermissionRequestSchema,
  GrantPermissionResponseSchema,
  ListRolePermissionsRequestSchema,
  ListRolePermissionsResponseSchema,
  ListRolesRequestSchema,
  ListRolesResponseSchema,
  RevokePermissionRequestSchema,
  RevokePermissionResponseSchema,
  UpdateRoleRequestSchema,
  UpdateRoleResponseSchema,
} from '../gen/fmgr/v1/role_pb';
import {
  CheckoutSampleRequestSchema,
  CheckoutSampleResponseSchema,
  CreateSampleRequestSchema,
  CreateSampleResponseSchema,
  ExportSamplesCsvRequestSchema,
  ExportSamplesCsvResponseSchema,
  GetSampleRequestSchema,
  GetSampleResponseSchema,
  ImportSamplesRequestSchema,
  ImportSamplesResponseSchema,
  ListSamplesRequestSchema,
  ListSamplesResponseSchema,
  MoveSampleRequestSchema,
  MoveSampleResponseSchema,
  SoftDeleteSampleRequestSchema,
  SoftDeleteSampleResponseSchema,
  UpdateSampleRequestSchema,
  UpdateSampleResponseSchema,
} from '../gen/fmgr/v1/sample_pb';
import {
  ListSessionsRequestSchema,
  ListSessionsResponseSchema,
  RevokeSessionRequestSchema,
  RevokeSessionResponseSchema,
} from '../gen/fmgr/v1/session_pb';
import {
  ApproveShareRequestRequestSchema,
  ApproveShareRequestResponseSchema,
  CreateShareRequestRequestSchema,
  CreateShareRequestResponseSchema,
  GetShareRequestRequestSchema,
  GetShareRequestResponseSchema,
  ListShareRequestsRequestSchema,
  ListShareRequestsResponseSchema,
  RejectShareRequestRequestSchema,
  RejectShareRequestResponseSchema,
  RevokeShareRequestRequestSchema,
  RevokeShareRequestResponseSchema,
} from '../gen/fmgr/v1/share_pb';
import type { MessageInitShape, MessageShape } from '@bufbuild/protobuf';
import type { UnaryRoute } from './route-types';

/**
 * The REST route table (TODO.md G1.2, G-arch 5): one entry per unary RPC the
 * gateway serves, keyed by `<noun>/<verb>` — the path with the `/api/v1/`
 * prefix removed, so `/api/v1/sample/list` is `'sample/list'`. That key is what
 * `src/test/fakeApi.ts` uses for per-RPC error injection
 * (`fakeApi({ fail: { 'sample/list': 'PERMISSION_DENIED' } })`, G-arch 10).
 *
 * `rpc` is the canonical proto name; `path` and the method part of `rpc` are
 * cross-checked against the `FMGR_ROUTE(...)` lines in
 * `src/rest/RestGateway.cc` by `scripts/check-routes.mjs`, which fails when the
 * two sides disagree in either direction. A C++ PR that adds a route must add
 * its entry here in the same PR — which is why this table is written by hand
 * and not generated.
 *
 * `input`/`output` are the schemas from `src/gen/` (`npm run gen`); they are
 * what make `call()` in `client.ts` typed end to end.
 *
 * The `auth/browser/*` entries (G0.1) are the same three AuthService RPCs as
 * `auth/*` but for a client that cannot hold a bearer token. The gateway keeps
 * the token in an `HttpOnly` cookie and answers `auth/browser/login` with
 * `{session_id, user_id, mfa_required}` — never `session_token` — so a caller
 * that reads `session_token` from that route gets the empty default.
 * Cookie-authenticated mutations are CSRF-checked and `client.ts` already
 * echoes the `fmgr_csrf` cookie in `X-CSRF-Token` for every call.
 */
export const apiRoutes = {
  // ---- AuthService ----
  'auth/login': {
    path: '/api/v1/auth/login',
    rpc: 'fmgr.v1.AuthService/Login',
    input: LoginRequestSchema,
    output: LoginResponseSchema,
  },
  'auth/submit-mfa': {
    path: '/api/v1/auth/submit-mfa',
    rpc: 'fmgr.v1.AuthService/SubmitMfa',
    input: SubmitMfaRequestSchema,
    output: SubmitMfaResponseSchema,
  },
  'auth/logout': {
    path: '/api/v1/auth/logout',
    rpc: 'fmgr.v1.AuthService/Logout',
    input: LogoutRequestSchema,
    output: LogoutResponseSchema,
  },
  'auth/api-token/create': {
    path: '/api/v1/auth/api-token/create',
    rpc: 'fmgr.v1.AuthService/CreateApiToken',
    input: CreateApiTokenRequestSchema,
    output: CreateApiTokenResponseSchema,
  },
  'auth/api-token/list': {
    path: '/api/v1/auth/api-token/list',
    rpc: 'fmgr.v1.AuthService/ListApiTokens',
    input: ListApiTokensRequestSchema,
    output: ListApiTokensResponseSchema,
  },
  'auth/api-token/revoke': {
    path: '/api/v1/auth/api-token/revoke',
    rpc: 'fmgr.v1.AuthService/RevokeApiToken',
    input: RevokeApiTokenRequestSchema,
    output: RevokeApiTokenResponseSchema,
  },

  // ---- AuthService browser session (G0.1) ----
  //
  // Same three RPCs as the auth/* entries above but for a client that cannot
  // hold a bearer token. See the table comment at the top of this file.
  'auth/browser/login': {
    path: '/api/v1/auth/browser/login',
    rpc: 'fmgr.v1.AuthService/Login',
    input: LoginRequestSchema,
    output: LoginResponseSchema,
  },
  'auth/browser/submit-mfa': {
    path: '/api/v1/auth/browser/submit-mfa',
    rpc: 'fmgr.v1.AuthService/SubmitMfa',
    input: SubmitMfaRequestSchema,
    output: SubmitMfaResponseSchema,
  },
  'auth/browser/logout': {
    path: '/api/v1/auth/browser/logout',
    rpc: 'fmgr.v1.AuthService/Logout',
    input: LogoutRequestSchema,
    output: LogoutResponseSchema,
  },

  // ---- SessionService ----
  'session/list': {
    path: '/api/v1/session/list',
    rpc: 'fmgr.v1.SessionService/ListSessions',
    input: ListSessionsRequestSchema,
    output: ListSessionsResponseSchema,
  },
  'session/revoke': {
    path: '/api/v1/session/revoke',
    rpc: 'fmgr.v1.SessionService/RevokeSession',
    input: RevokeSessionRequestSchema,
    output: RevokeSessionResponseSchema,
  },

  // ---- LabService ----
  'lab/get': {
    path: '/api/v1/lab/get',
    rpc: 'fmgr.v1.LabService/GetLab',
    input: GetLabRequestSchema,
    output: GetLabResponseSchema,
  },
  'lab/list': {
    path: '/api/v1/lab/list',
    rpc: 'fmgr.v1.LabService/ListLabs',
    input: ListLabsRequestSchema,
    output: ListLabsResponseSchema,
  },
  'lab/create': {
    path: '/api/v1/lab/create',
    rpc: 'fmgr.v1.LabService/CreateLab',
    input: CreateLabRequestSchema,
    output: CreateLabResponseSchema,
  },
  'lab/update': {
    path: '/api/v1/lab/update',
    rpc: 'fmgr.v1.LabService/UpdateLab',
    input: UpdateLabRequestSchema,
    output: UpdateLabResponseSchema,
  },
  'lab/enable-phi': {
    path: '/api/v1/lab/enable-phi',
    rpc: 'fmgr.v1.LabService/EnablePhi',
    input: EnablePhiRequestSchema,
    output: EnablePhiResponseSchema,
  },
  'lab/members/list': {
    path: '/api/v1/lab/members/list',
    rpc: 'fmgr.v1.LabService/ListMembers',
    input: ListMembersRequestSchema,
    output: ListMembersResponseSchema,
  },
  'lab/members/invite': {
    path: '/api/v1/lab/members/invite',
    rpc: 'fmgr.v1.LabService/InviteMember',
    input: InviteMemberRequestSchema,
    output: InviteMemberResponseSchema,
  },
  'lab/members/revoke': {
    path: '/api/v1/lab/members/revoke',
    rpc: 'fmgr.v1.LabService/RevokeMembership',
    input: RevokeMembershipRequestSchema,
    output: RevokeMembershipResponseSchema,
  },

  // ---- SampleService ----
  'sample/list': {
    path: '/api/v1/sample/list',
    rpc: 'fmgr.v1.SampleService/ListSamples',
    input: ListSamplesRequestSchema,
    output: ListSamplesResponseSchema,
  },
  'sample/get': {
    path: '/api/v1/sample/get',
    rpc: 'fmgr.v1.SampleService/GetSample',
    input: GetSampleRequestSchema,
    output: GetSampleResponseSchema,
  },
  'sample/create': {
    path: '/api/v1/sample/create',
    rpc: 'fmgr.v1.SampleService/CreateSample',
    input: CreateSampleRequestSchema,
    output: CreateSampleResponseSchema,
  },
  'sample/update': {
    path: '/api/v1/sample/update',
    rpc: 'fmgr.v1.SampleService/UpdateSample',
    input: UpdateSampleRequestSchema,
    output: UpdateSampleResponseSchema,
  },
  'sample/delete': {
    path: '/api/v1/sample/delete',
    rpc: 'fmgr.v1.SampleService/SoftDeleteSample',
    input: SoftDeleteSampleRequestSchema,
    output: SoftDeleteSampleResponseSchema,
  },
  'sample/move': {
    path: '/api/v1/sample/move',
    rpc: 'fmgr.v1.SampleService/MoveSample',
    input: MoveSampleRequestSchema,
    output: MoveSampleResponseSchema,
  },
  'sample/checkout': {
    path: '/api/v1/sample/checkout',
    rpc: 'fmgr.v1.SampleService/CheckoutSample',
    input: CheckoutSampleRequestSchema,
    output: CheckoutSampleResponseSchema,
  },
  'sample/export': {
    path: '/api/v1/sample/export',
    rpc: 'fmgr.v1.SampleService/ExportSamplesCsv',
    input: ExportSamplesCsvRequestSchema,
    output: ExportSamplesCsvResponseSchema,
  },
  'sample/import': {
    path: '/api/v1/sample/import',
    rpc: 'fmgr.v1.SampleService/ImportSamples',
    input: ImportSamplesRequestSchema,
    output: ImportSamplesResponseSchema,
  },

  // ---- BoxService ----
  'freezer/list': {
    path: '/api/v1/freezer/list',
    rpc: 'fmgr.v1.BoxService/ListFreezers',
    input: ListFreezersRequestSchema,
    output: ListFreezersResponseSchema,
  },
  'freezer/get': {
    path: '/api/v1/freezer/get',
    rpc: 'fmgr.v1.BoxService/GetFreezer',
    input: GetFreezerRequestSchema,
    output: GetFreezerResponseSchema,
  },
  'freezer/create': {
    path: '/api/v1/freezer/create',
    rpc: 'fmgr.v1.BoxService/CreateFreezer',
    input: CreateFreezerRequestSchema,
    output: CreateFreezerResponseSchema,
  },
  'freezer/update': {
    path: '/api/v1/freezer/update',
    rpc: 'fmgr.v1.BoxService/UpdateFreezer',
    input: UpdateFreezerRequestSchema,
    output: UpdateFreezerResponseSchema,
  },
  'freezer/archive': {
    path: '/api/v1/freezer/archive',
    rpc: 'fmgr.v1.BoxService/ArchiveFreezer',
    input: ArchiveFreezerRequestSchema,
    output: ArchiveFreezerResponseSchema,
  },
  'storage-container/list': {
    path: '/api/v1/storage-container/list',
    rpc: 'fmgr.v1.BoxService/ListStorageContainers',
    input: ListStorageContainersRequestSchema,
    output: ListStorageContainersResponseSchema,
  },
  'storage-container/create': {
    path: '/api/v1/storage-container/create',
    rpc: 'fmgr.v1.BoxService/CreateStorageContainer',
    input: CreateStorageContainerRequestSchema,
    output: CreateStorageContainerResponseSchema,
  },
  'storage-container/update': {
    path: '/api/v1/storage-container/update',
    rpc: 'fmgr.v1.BoxService/UpdateStorageContainer',
    input: UpdateStorageContainerRequestSchema,
    output: UpdateStorageContainerResponseSchema,
  },
  'storage-container/archive': {
    path: '/api/v1/storage-container/archive',
    rpc: 'fmgr.v1.BoxService/ArchiveStorageContainer',
    input: ArchiveStorageContainerRequestSchema,
    output: ArchiveStorageContainerResponseSchema,
  },
  'container-type/list': {
    path: '/api/v1/container-type/list',
    rpc: 'fmgr.v1.BoxService/ListContainerTypes',
    input: ListContainerTypesRequestSchema,
    output: ListContainerTypesResponseSchema,
  },
  'container-type/create': {
    path: '/api/v1/container-type/create',
    rpc: 'fmgr.v1.BoxService/CreateContainerType',
    input: CreateContainerTypeRequestSchema,
    output: CreateContainerTypeResponseSchema,
  },
  'box-type/list': {
    path: '/api/v1/box-type/list',
    rpc: 'fmgr.v1.BoxService/ListBoxTypes',
    input: ListBoxTypesRequestSchema,
    output: ListBoxTypesResponseSchema,
  },
  'box-type/create': {
    path: '/api/v1/box-type/create',
    rpc: 'fmgr.v1.BoxService/CreateBoxType',
    input: CreateBoxTypeRequestSchema,
    output: CreateBoxTypeResponseSchema,
  },
  'box/list': {
    path: '/api/v1/box/list',
    rpc: 'fmgr.v1.BoxService/ListBoxes',
    input: ListBoxesRequestSchema,
    output: ListBoxesResponseSchema,
  },
  'box/get': {
    path: '/api/v1/box/get',
    rpc: 'fmgr.v1.BoxService/GetBox',
    input: GetBoxRequestSchema,
    output: GetBoxResponseSchema,
  },
  'box/create': {
    path: '/api/v1/box/create',
    rpc: 'fmgr.v1.BoxService/CreateBox',
    input: CreateBoxRequestSchema,
    output: CreateBoxResponseSchema,
  },
  'box/update': {
    path: '/api/v1/box/update',
    rpc: 'fmgr.v1.BoxService/UpdateBox',
    input: UpdateBoxRequestSchema,
    output: UpdateBoxResponseSchema,
  },
  'box/archive': {
    path: '/api/v1/box/archive',
    rpc: 'fmgr.v1.BoxService/ArchiveBox',
    input: ArchiveBoxRequestSchema,
    output: ArchiveBoxResponseSchema,
  },

  // ---- ItemTypeService ----
  'item-type/list': {
    path: '/api/v1/item-type/list',
    rpc: 'fmgr.v1.ItemTypeService/ListItemTypes',
    input: ListItemTypesRequestSchema,
    output: ListItemTypesResponseSchema,
  },
  'item-type/get': {
    path: '/api/v1/item-type/get',
    rpc: 'fmgr.v1.ItemTypeService/GetItemType',
    input: GetItemTypeRequestSchema,
    output: GetItemTypeResponseSchema,
  },
  'item-type/create': {
    path: '/api/v1/item-type/create',
    rpc: 'fmgr.v1.ItemTypeService/CreateItemType',
    input: CreateItemTypeRequestSchema,
    output: CreateItemTypeResponseSchema,
  },
  'item-type/update': {
    path: '/api/v1/item-type/update',
    rpc: 'fmgr.v1.ItemTypeService/UpdateItemType',
    input: UpdateItemTypeRequestSchema,
    output: UpdateItemTypeResponseSchema,
  },
  'item-type/archive': {
    path: '/api/v1/item-type/archive',
    rpc: 'fmgr.v1.ItemTypeService/ArchiveItemType',
    input: ArchiveItemTypeRequestSchema,
    output: ArchiveItemTypeResponseSchema,
  },
  'custom-field-def/list': {
    path: '/api/v1/custom-field-def/list',
    rpc: 'fmgr.v1.ItemTypeService/ListCustomFieldDefinitions',
    input: ListCfdsRequestSchema,
    output: ListCfdsResponseSchema,
  },
  'custom-field-def/create': {
    path: '/api/v1/custom-field-def/create',
    rpc: 'fmgr.v1.ItemTypeService/CreateCustomFieldDefinition',
    input: CreateCfdRequestSchema,
    output: CreateCfdResponseSchema,
  },
  'custom-field-def/update': {
    path: '/api/v1/custom-field-def/update',
    rpc: 'fmgr.v1.ItemTypeService/UpdateCustomFieldDefinition',
    input: UpdateCfdRequestSchema,
    output: UpdateCfdResponseSchema,
  },
  'custom-field-def/archive': {
    path: '/api/v1/custom-field-def/archive',
    rpc: 'fmgr.v1.ItemTypeService/ArchiveCustomFieldDefinition',
    input: ArchiveCfdRequestSchema,
    output: ArchiveCfdResponseSchema,
  },

  // ---- RoleService ----
  'role/list': {
    path: '/api/v1/role/list',
    rpc: 'fmgr.v1.RoleService/ListRoles',
    input: ListRolesRequestSchema,
    output: ListRolesResponseSchema,
  },
  'role/get': {
    path: '/api/v1/role/get',
    rpc: 'fmgr.v1.RoleService/GetRole',
    input: GetRoleRequestSchema,
    output: GetRoleResponseSchema,
  },
  'role/create': {
    path: '/api/v1/role/create',
    rpc: 'fmgr.v1.RoleService/CreateRole',
    input: CreateRoleRequestSchema,
    output: CreateRoleResponseSchema,
  },
  'role/update': {
    path: '/api/v1/role/update',
    rpc: 'fmgr.v1.RoleService/UpdateRole',
    input: UpdateRoleRequestSchema,
    output: UpdateRoleResponseSchema,
  },
  'role/archive': {
    path: '/api/v1/role/archive',
    rpc: 'fmgr.v1.RoleService/ArchiveRole',
    input: ArchiveRoleRequestSchema,
    output: ArchiveRoleResponseSchema,
  },
  'role/permissions/list': {
    path: '/api/v1/role/permissions/list',
    rpc: 'fmgr.v1.RoleService/ListRolePermissions',
    input: ListRolePermissionsRequestSchema,
    output: ListRolePermissionsResponseSchema,
  },
  'role/permissions/grant': {
    path: '/api/v1/role/permissions/grant',
    rpc: 'fmgr.v1.RoleService/GrantPermission',
    input: GrantPermissionRequestSchema,
    output: GrantPermissionResponseSchema,
  },
  'role/permissions/revoke': {
    path: '/api/v1/role/permissions/revoke',
    rpc: 'fmgr.v1.RoleService/RevokePermission',
    input: RevokePermissionRequestSchema,
    output: RevokePermissionResponseSchema,
  },

  // ---- AuditService ----
  'audit/list': {
    path: '/api/v1/audit/list',
    rpc: 'fmgr.v1.AuditService/ListAuditEvents',
    input: ListAuditEventsRequestSchema,
    output: ListAuditEventsResponseSchema,
  },
  'audit/get': {
    path: '/api/v1/audit/get',
    rpc: 'fmgr.v1.AuditService/GetAuditEvent',
    input: GetAuditEventRequestSchema,
    output: GetAuditEventResponseSchema,
  },
  'audit/verify': {
    path: '/api/v1/audit/verify',
    rpc: 'fmgr.v1.AuditService/VerifyAuditChain',
    input: VerifyAuditChainRequestSchema,
    output: VerifyAuditChainResponseSchema,
  },
  'audit/export': {
    path: '/api/v1/audit/export',
    rpc: 'fmgr.v1.AuditService/ExportAuditLog',
    input: ExportAuditLogRequestSchema,
    output: ExportAuditLogResponseSchema,
  },

  // ---- ShareService ----
  'share/list': {
    path: '/api/v1/share/list',
    rpc: 'fmgr.v1.ShareService/ListShareRequests',
    input: ListShareRequestsRequestSchema,
    output: ListShareRequestsResponseSchema,
  },
  'share/get': {
    path: '/api/v1/share/get',
    rpc: 'fmgr.v1.ShareService/GetShareRequest',
    input: GetShareRequestRequestSchema,
    output: GetShareRequestResponseSchema,
  },
  'share/create': {
    path: '/api/v1/share/create',
    rpc: 'fmgr.v1.ShareService/CreateShareRequest',
    input: CreateShareRequestRequestSchema,
    output: CreateShareRequestResponseSchema,
  },
  'share/approve': {
    path: '/api/v1/share/approve',
    rpc: 'fmgr.v1.ShareService/ApproveShareRequest',
    input: ApproveShareRequestRequestSchema,
    output: ApproveShareRequestResponseSchema,
  },
  'share/reject': {
    path: '/api/v1/share/reject',
    rpc: 'fmgr.v1.ShareService/RejectShareRequest',
    input: RejectShareRequestRequestSchema,
    output: RejectShareRequestResponseSchema,
  },
  'share/revoke': {
    path: '/api/v1/share/revoke',
    rpc: 'fmgr.v1.ShareService/RevokeShareRequest',
    input: RevokeShareRequestRequestSchema,
    output: RevokeShareRequestResponseSchema,
  },
} as const satisfies Record<string, UnaryRoute>;

/** The table's literal type, so keys and schemas both stay inferred. */
export type ApiRoutes = typeof apiRoutes;

/** Every route key, e.g. `'sample/list'`. */
export type RpcName = keyof ApiRoutes;

/** The request message type of one route, inferred from its generated schema. */
export type RequestOf<K extends RpcName> = MessageShape<ApiRoutes[K]['input']>;

/** The response message type of one route, inferred from its generated schema. */
export type ResponseOf<K extends RpcName> = MessageShape<ApiRoutes[K]['output']>;

/**
 * What `call()` accepts as a request: the init shape of that route's generated
 * request message, so a caller writes a plain object and still gets a
 * compile-time field check.
 */
export type RequestInitOf<K extends RpcName> = MessageInitShape<ApiRoutes[K]['input']>;

/** `/api/v1/sample/list` -> `'sample/list'`. The one rule the table keys follow. */
export function routeKey(path: UnaryRoute['path']): string {
  return path.replace(/^\/api\/v1\//, '');
}
