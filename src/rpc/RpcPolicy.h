// SPDX-License-Identifier: AGPL-3.0-or-later

// Explicit authorization policy for a gRPC method (review C9). Previously every
// RPC was registered with a bare core::Permission, which forced pre-auth and
// self-service endpoints (Login, Logout, ListSessions, ...) to borrow an
// unrelated permission (SessionRevoke) as a placeholder — misleading, and it
// hid the fact that those endpoints do not run the standard permission gate.
//
// RpcPolicy models the four real categories:
//   Public            — no bearer required (Login, SubmitMfa establish one).
//   SelfService       — valid, MFA-complete bearer; no specific permission
//                       (a caller acting on their own session/tokens).
//   LabPermission     — a lab-scoped permission checked against a target lab.
//   GlobalPermission  — a deployment-wide permission (global-only actions).
#ifndef FMGR_RPC_RPCPOLICY_H
#define FMGR_RPC_RPCPOLICY_H

#include "core/permissions.h"

#include <cstdint>
#include <optional>

namespace fmgr::rpc {

  enum class RpcPolicyKind : std::uint8_t {
    Public,
    SelfService,
    LabPermission,
    GlobalPermission,
  };

  struct RpcPolicy {
    RpcPolicyKind kind{RpcPolicyKind::LabPermission};
    // Set iff kind is LabPermission or GlobalPermission.
    std::optional<core::Permission> permission{};

    [[nodiscard]] static RpcPolicy public_() { return {RpcPolicyKind::Public, std::nullopt}; }
    [[nodiscard]] static RpcPolicy self_service() {
      return {RpcPolicyKind::SelfService, std::nullopt};
    }
    [[nodiscard]] static RpcPolicy lab(core::Permission perm) {
      return {RpcPolicyKind::LabPermission, perm};
    }
    [[nodiscard]] static RpcPolicy global(core::Permission perm) {
      return {RpcPolicyKind::GlobalPermission, perm};
    }

    // Classify a bare permission into its scope: global-only permissions (e.g.
    // BackupRun, KeyRotate, LabProvision, SampleDeleteHard) are GlobalPermission,
    // everything else is LabPermission. Keeps the invariant that GlobalPermission
    // policies use global-only perms and LabPermission policies do not.
    [[nodiscard]] static RpcPolicy for_permission(core::Permission perm) {
      return core::is_global_only_permission(perm) ? global(perm) : lab(perm);
    }

    [[nodiscard]] bool requires_permission() const {
      return kind == RpcPolicyKind::LabPermission || kind == RpcPolicyKind::GlobalPermission;
    }

    bool operator==(const RpcPolicy&) const = default;
  };

} // namespace fmgr::rpc

#endif // FMGR_RPC_RPCPOLICY_H
