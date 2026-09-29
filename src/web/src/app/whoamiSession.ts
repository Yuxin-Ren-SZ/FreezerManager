// SPDX-License-Identifier: AGPL-3.0-or-later
import { isApiError, isMfaRequired } from '../api/errors';
import { isPermissionKey, type PermissionKey } from './permissions';
import {
  MfaPendingError,
  type CurrentUser,
  type LabMembership,
  type SessionLoader,
} from './session';

/**
 * The `AuthService.WhoAmI` response (G0.2), as proto3 JSON with `snake_case`
 * names and defaults omitted arrives through `src/api/client.ts`.
 *
 * It is declared here rather than imported from `src/gen/` on purpose: the
 * generated message does not exist until G0.2 lands, and this mapping is the
 * SPA's half of that contract — the part that has to be right for the shell to
 * render the user it just authenticated. The fields are optional where proto3
 * would omit a default, so a response that leaves `labs` out is not a crash.
 */
export interface WhoAmIMembershipWire {
  readonly labId: string;
  readonly labName: string;
  readonly roleId: string;
  readonly roleName: string;
  readonly permissions?: readonly string[];
  readonly isPhiEnabled?: boolean;
}

export interface WhoAmIWire {
  readonly userId: string;
  readonly email: string;
  readonly displayName: string;
  readonly isSystemAdmin?: boolean;
  /** Deployment-wide grants, outside any lab membership. */
  readonly permissions?: readonly string[];
  readonly labs?: readonly WhoAmIMembershipWire[];
}

/** How the SPA asks `auth/whoami`. A parameter so the mapping is testable alone. */
export type WhoAmIFetch = () => Promise<WhoAmIWire>;

/** Keeps only the permissions this bundle knows how to reason about. */
function knownPermissions(keys: readonly string[] | undefined): readonly PermissionKey[] {
  return (keys ?? []).filter(isPermissionKey);
}

function toMembership(membership: WhoAmIMembershipWire): LabMembership {
  return {
    labId: membership.labId,
    labName: membership.labName,
    roleId: membership.roleId,
    roleName: membership.roleName,
    permissions: knownPermissions(membership.permissions),
    // Absent means PHI is off: proto3 omits a `false`, and a screen that gates a
    // PHI affordance must fail closed when the server did not say otherwise.
    isPhiEnabled: membership.isPhiEnabled ?? false,
  };
}

/** The wire response as the session context's `CurrentUser`. */
export function toCurrentUser(whoami: WhoAmIWire): CurrentUser {
  return {
    userId: whoami.userId,
    email: whoami.email,
    displayName: whoami.displayName,
    isSystemAdmin: whoami.isSystemAdmin ?? false,
    permissions: knownPermissions(whoami.permissions),
    labs: (whoami.labs ?? []).map(toMembership),
  };
}

/**
 * The real session loader: `auth/whoami` (G0.2), and nothing else.
 *
 * The three answers are kept apart, because collapsing any two of them is a
 * bug the user sees:
 *
 * - **A user** — the session is real and its second factor is done.
 * - **`null`** — `UNAUTHENTICATED`: no valid session, so the SPA shows the
 *   sign-in form.
 * - **`MfaPendingError`** — `UNAUTHENTICATED` *with* the `mfa_required: `
 *   prefix: a session exists but the TOTP code is still outstanding (#62).
 *   A half-finished login would otherwise be indistinguishable from a dead one
 *   and the user would be sent back to the password form for nothing.
 * - **Anything else propagates** — a dropped connection must reach
 *   `RequireSession`'s retry, never the login page.
 */
export function createWhoAmISessionLoader(fetchWhoAmI: WhoAmIFetch): SessionLoader {
  return async () => {
    try {
      return toCurrentUser(await fetchWhoAmI());
    } catch (cause) {
      if (isMfaRequired(cause)) {
        throw new MfaPendingError();
      }
      if (isApiError(cause) && cause.code === 'UNAUTHENTICATED') {
        return null;
      }
      throw cause;
    }
  };
}
