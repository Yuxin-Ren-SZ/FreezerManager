# FreezerManager — F2/F3 gRPC Service Review

**Date:** 2026-06-12
**Reviewed range:** `main..dev` (tip `f30a944`) — F2/F3 gRPC server slices
**Scope:** Production handlers only — `RoleServiceImpl`, `ShareServiceImpl`, `AuditServiceImpl`. Tests not reviewed.
**Method:** Manual inspection of the new service handlers, cross-referenced against `LocalAuthProvider::resolve_permissions` and `core::permissions`.

---

## Executive Summary

The three new gRPC services are structurally sound: consistent auth gating, RLS
var injection on every transaction, `Serializable` isolation for writes, and a
uniform `catch (...) → current_exception_to_grpc_status()` funnel.

One **critical privilege-escalation chain** was found (F-1) that lets a lab admin
reach deployment-wide SystemAdmin. One **medium** governance gap (F-2, share
separation-of-duties) and several lower-severity correctness/UX issues round out
the list. All items below are fixed in the same commit as this document.

---

## F-1 — 🔴 CRITICAL: lab admin → deployment SystemAdmin escalation

**Files:** `src/server/RoleServiceImpl.cc` (`CreateRole`, `UpdateRole`,
`GrantPermission`), enabled by `src/auth/LocalAuthProvider.cc:144-152`.

### Chain

1. A lab admin holds `UserManageRoles` for their own lab — legitimately.
2. `CreateRole` accepted `kind = ROLE_KIND_SYSTEM_ADMIN` on a **custom**
   (`is_builtin = false`, lab-owned) role with no restriction.
3. `GrantPermission` accepted **any** permission key via `parse_permission_key`,
   including global-only permissions such as `LabProvision`.
4. `resolve_permissions` (`LocalAuthProvider.cc:145-148`) promotes a global-only
   permission into `global_permissions` **whenever the owning role's
   `kind == SystemAdmin`** — regardless of `is_builtin` or lab ownership.
5. The role is assigned via `LabMembership`; the holder's session now satisfies
   `has_global(LabProvision)`, which every service uses as the
   `is_system_admin()` marker → deployment-wide compromise.

Either gate (3) or (4) closing the door is sufficient; we close both for
defense-in-depth.

### Fix

- `CreateRole` / `UpdateRole`: reject `kind` of `SYSTEM_ADMIN` (and any other
  built-in-reserved kind) on custom roles. Custom roles may only be
  `Member` / `ReadOnly` / `LabAdmin` / `ApiClient`.
- `GrantPermission`: reject `core::is_global_only_permission(...)` keys — a
  lab-scoped custom role must never carry a deployment-global permission. Returns
  `FAILED_PRECONDITION`.

---

## F-2 — 🟡 Share approval: no separation of duties

**File:** `src/server/ShareServiceImpl.cc` (`ApproveShareRequest`).

`ApproveShareRequest` gates each call by `approver_role`, but nothing requires
the three required signatures (source / target / system) come from **distinct**
users. A principal holding `ShareApprove` on both source and target labs and who
is also a system admin can call `Approve` three times with different
`approver_role` values, self-satisfy `all_roles_signed`, and single-handedly
approve a cross-lab biospecimen transfer — defeating the multi-party custody
control.

### Fix

Before recording a signature, reject if the same `approver_user_id` has already
signed this request under any role (`FAILED_PRECONDITION`). One human, one
signature, per request.

---

## F-3 — 🟡 `ListShareRequests`: `include_decided` is mislabeled

**File:** `src/server/ShareServiceImpl.cc:284-285`.

`include_decided` mapped to `query.include_tombstoned()`. Only `Revoke`
tombstones a request; `Approved` / `Rejected` only flip `status` and are never
tombstoned, so they were **always** listed regardless of the flag. The flag's
real effect was toggling *revoked* visibility, contradicting its name.

### Fix

Default list returns only `Pending`; `include_decided` additionally surfaces
`Approved` / `Rejected` / `Revoked`. Implemented by filtering on `status` and
only calling `include_tombstoned()` when the flag is set.

---

## F-4 — 🟡 Page-token parse throws → wrong gRPC code

**Files:** `src/server/AuditServiceImpl.cc:135`,
`src/server/ShareServiceImpl.cc:303`.

`std::stoull(page_token)` throws `std::invalid_argument` / `std::out_of_range`
on a malformed client token, which the `catch (...)` funnel reports as
`INTERNAL`. A bad client-supplied token is a client error.

### Fix

Parse via a helper that translates failure into `storage::ConstraintViolation`
→ `INVALID_ARGUMENT`.

---

## F-5 — 🔵 `CreateShareRequest`: missing source≠target guard

**File:** `src/server/ShareServiceImpl.cc:192-221`.

No check that `source_lab_id != target_lab_id`; a self-share request was
acceptable. `scope_json` is also stored unvalidated (deferred — downstream
share-grant materialization does not yet exist).

### Fix

Reject `source_lab_id == target_lab_id` with `INVALID_ARGUMENT`.

---

## F-6 — 🔵 Unbounded audit reads (acknowledged, tracked)

**File:** `src/server/AuditServiceImpl.cc:211,271`.

`VerifyAuditChain` and `ExportAuditLog` load every audit row into memory. The
chain hash is global so verification cannot be paged, and export is bounded by
`since`. Acceptable for current deployment scale; left as a tracked TODO for a
streaming path. **Not changed in this pass** — recorded here for visibility.

---

## F-7 — 🔵 Duplicated service-local helpers

**Files:** `RoleServiceImpl.cc`, `ShareServiceImpl.cc` (and `BoxServiceImpl`).

`generate_uuid_v4`, `now_timestamp`, `make_ctx`, `is_system_admin` are copy-pasted
across service translation units (the comments even cross-reference each other).
Candidate for a shared `server/ServiceUtil.h`. **Not changed in this pass** —
noted as cleanup debt to avoid widening the diff during a security fix.

---

## Resolution

| ID | Severity | Status |
|----|----------|--------|
| F-1 | 🔴 Critical | ✅ Fixed |
| F-2 | 🟡 Medium | ✅ Fixed |
| F-3 | 🟡 Medium | ✅ Fixed |
| F-4 | 🟡 Medium | ✅ Fixed |
| F-5 | 🔵 Low | ✅ Fixed |
| F-6 | 🔵 Low | ⏳ Tracked (TODO) |
| F-7 | 🔵 Low | ⏳ Tracked (cleanup) |
