// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/LabServiceImpl.h"

#include "core/enums.h"
#include "core/identity.h"
#include "core/permissions.h"
#include "core/role.h"
#include "server/GrpcErrorTranslation.h"
#include "storage/IStorageBackend.h"
#include "storage/IdentityTraits.h"

#include <fmgr/v1/lab.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <string>

namespace fmgr::server {
  namespace {

    [[nodiscard]] storage::MutationContext make_ctx(const auth::SessionContext& sctx,
                                                    std::string_view reason) {
      return storage::MutationContext{
          .actor_user_id = sctx.user_id,
          .actor_session_id = sctx.session_id.to_string(),
          .request_id = "",
          .reason = std::string(reason),
      };
    }

    [[nodiscard]] core::Timestamp now_timestamp() {
      const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      return core::Timestamp::from_unix_micros(static_cast<std::int64_t>(micros));
    }

    // RFC 4122 version-4 UUID from a non-deterministic entropy source. IDs must be
    // unguessable (sequential IDs leak record counts), but need not be
    // cryptographically secret, so std::random_device is sufficient here.
    [[nodiscard]] std::string generate_uuid_v4() {
      std::random_device rng;
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const std::uint32_t word = rng();
        bytes[i] = static_cast<std::uint8_t>(word & 0xFFU);
        bytes[i + 1] = static_cast<std::uint8_t>((word >> 8U) & 0xFFU);
        bytes[i + 2] = static_cast<std::uint8_t>((word >> 16U) & 0xFFU);
        bytes[i + 3] = static_cast<std::uint8_t>((word >> 24U) & 0xFFU);
      }
      bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U); // version 4
      bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U); // variant
      std::array<char, 37> buf{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      std::snprintf(buf.data(), buf.size(),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                    bytes[15]);
      return {buf.data()};
    }

    void fill_lab(fmgr::v1::Lab* out, const core::Lab& lab) {
      out->set_id(lab.id.to_string());
      out->set_name(lab.name);
      out->set_contact(lab.contact);
      out->mutable_created_at()->set_unix_micros(lab.created_at.unix_micros());
      out->set_settings_json(lab.settings_json.dump());
      out->set_is_phi_enabled(lab.is_phi_enabled);
    }

    void fill_lab_member(fmgr::v1::LabMember* out, const core::LabMembership& membership) {
      out->set_user_id(membership.user_id.to_string());
      out->set_lab_id(membership.lab_id.to_string());
      if (membership.role_id.has_value()) {
        out->set_role_id(membership.role_id->to_string());
      }
      out->set_scope_filters_json(membership.scope_filters_json.dump());
      out->mutable_joined_at()->set_unix_micros(membership.joined_at.unix_micros());
      if (membership.revoked_at.has_value()) {
        out->mutable_revoked_at()->set_unix_micros(membership.revoked_at->unix_micros());
      }
    }

  } // namespace

  LabServiceImpl::LabServiceImpl(auth::IAuthProvider& auth, storage::IStorageBackend& backend)
      : auth_(auth), backend_(backend), middleware_(auth) {
    using P = core::Permission;
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.LabService/GetLab", P::LabConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.LabService/ListLabs", P::LabConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.LabService/CreateLab", P::LabProvision);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.LabService/UpdateLab", P::LabConfigure);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.LabService/EnablePhi", P::LabEnablePhi);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.LabService/ListMembers", P::UserInvite);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.LabService/InviteMember", P::UserInvite);
    rpc::AuthMiddleware::register_rpc("/fmgr.v1.LabService/RevokeMembership", P::UserInvite);
  }

  grpc::Status LabServiceImpl::GetLab(grpc::ServerContext* ctx, const fmgr::v1::GetLabRequest* req,
                                      fmgr::v1::GetLabResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::LabConfigure, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto lab = txn->repo<core::Lab>().find_by_id(lab_id);
      txn->commit();

      if (!lab.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "lab not found"};
      }
      fill_lab(resp->mutable_lab(), *lab);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status LabServiceImpl::ListLabs(grpc::ServerContext* ctx,
                                        const fmgr::v1::ListLabsRequest* /*req*/,
                                        fmgr::v1::ListLabsResponse* resp) {
    try {
      // Visibility-scoped, not permission-gated: a caller lists the labs they can
      // see. A deployment admin (global lab.provision) sees every lab; everyone
      // else sees the labs they hold a membership in. Token + MFA still required.
      const auto bearer = extract_bearer(*ctx);
      auto sctx = auth_.validate_token(bearer);
      if (!sctx.mfa_complete) {
        throw auth::MfaRequired("MFA required before this operation");
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);

      if (sctx.has_global(core::Permission::LabProvision)) {
        for (const auto& lab : txn->repo<core::Lab>().query(storage::Query<core::Lab>::all())) {
          fill_lab(resp->add_labs(), lab);
        }
      } else {
        for (const auto& [lab_id, perms] : sctx.permissions_by_lab) {
          const auto lab = txn->repo<core::Lab>().find_by_id(lab_id);
          if (lab.has_value()) {
            fill_lab(resp->add_labs(), *lab);
          }
        }
      }
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status LabServiceImpl::CreateLab(grpc::ServerContext* ctx,
                                         const fmgr::v1::CreateLabRequest* req,
                                         fmgr::v1::CreateLabResponse* resp) {
    try {
      // Provisioning a brand-new lab is a deployment-level (SystemAdmin) action:
      // no membership exists yet, so it is gated globally by lab.provision.
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::LabProvision, std::nullopt);

      const core::Lab lab{
          .id = core::LabId::parse(generate_uuid_v4()),
          .name = req->name(),
          .contact = req->contact(),
          .created_at = now_timestamp(),
          .settings_json = nlohmann::json::object(),
          .is_phi_enabled = false,
      };
      // The provisioner becomes the lab's first administrator so they can manage
      // what they created (per-lab RPCs need a membership-derived permission).
      const core::LabMembership membership{
          .user_id = sctx.user_id,
          .lab_id = lab.id,
          .role_id = core::builtin_role_id(core::RoleKind::SystemAdmin),
          .invited_by = sctx.user_id,
          .joined_at = lab.created_at,
      };

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto mut = make_ctx(sctx, "create_lab");
      txn->repo<core::Lab>().insert(lab, mut);
      txn->repo<core::LabMembership>().insert(membership, mut);
      txn->commit();

      fill_lab(resp->mutable_lab(), lab);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status LabServiceImpl::UpdateLab(grpc::ServerContext* ctx,
                                         const fmgr::v1::UpdateLabRequest* req,
                                         fmgr::v1::UpdateLabResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab().id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::LabConfigure, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);

      auto existing = txn->repo<core::Lab>().find_by_id(lab_id);
      if (!existing.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "lab not found"};
      }
      // Mutable fields only. is_phi_enabled is owned by EnablePhi (separate
      // permission); created_at/archived_at are not caller-editable.
      existing->name = req->lab().name();
      existing->contact = req->lab().contact();
      if (!req->lab().settings_json().empty()) {
        existing->settings_json = nlohmann::json::parse(req->lab().settings_json());
      }
      txn->repo<core::Lab>().update(*existing, make_ctx(sctx, "update_lab"));
      txn->commit();

      fill_lab(resp->mutable_lab(), *existing);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status LabServiceImpl::EnablePhi(grpc::ServerContext* ctx,
                                         const fmgr::v1::EnablePhiRequest* req,
                                         fmgr::v1::EnablePhiResponse* /*resp*/) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::LabEnablePhi, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);

      auto existing = txn->repo<core::Lab>().find_by_id(lab_id);
      if (!existing.has_value()) {
        return {grpc::StatusCode::NOT_FOUND, "lab not found"};
      }
      existing->is_phi_enabled = true;
      txn->repo<core::Lab>().update(*existing, make_ctx(sctx, "enable_phi"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status LabServiceImpl::ListMembers(grpc::ServerContext* ctx,
                                           const fmgr::v1::ListMembersRequest* req,
                                           fmgr::v1::ListMembersResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::UserInvite, lab_id);

      auto query = storage::Query<core::LabMembership>::where(
          storage::field<core::LabMembership, std::string>(core::LabMembership::Field::LabId) ==
          lab_id.to_string());
      if (req->include_revoked()) {
        query = query.include_tombstoned();
      }

      auto txn = backend_.begin(storage::IsolationLevel::ReadCommitted);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto members = txn->repo<core::LabMembership>().query(query);
      txn->commit();

      for (const auto& member : members) {
        fill_lab_member(resp->add_members(), member);
      }
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status LabServiceImpl::InviteMember(grpc::ServerContext* ctx,
                                            const fmgr::v1::InviteMemberRequest* req,
                                            fmgr::v1::InviteMemberResponse* resp) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::UserInvite, lab_id);
      const auto role_id = core::RoleId::parse(req->role_id());

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      const auto mut = make_ctx(sctx, "invite_member");

      // Reuse an existing global User if the email is already known (including
      // disabled/invited accounts), otherwise create a pending one. No credential
      // is set here: the invitee establishes auth via a later accept-invite flow.
      auto by_email =
          storage::Query<core::User>::where(storage::field<core::User, std::string>(
                                                core::User::Field::PrimaryEmail) == req->email())
              .include_tombstoned();
      const auto existing_users = txn->repo<core::User>().query(by_email);

      core::UserId user_id;
      if (!existing_users.empty()) {
        user_id = existing_users.front().id;
      } else {
        const core::User pending{
            .id = core::UserId::parse(generate_uuid_v4()),
            .primary_email = req->email(),
            .display_name = req->email(),
            .status = core::UserStatus::Disabled,
            .created_at = now_timestamp(),
            .auth_bindings = nlohmann::json::array(),
        };
        txn->repo<core::User>().insert(pending, mut);
        user_id = pending.id;
      }

      const core::LabMembership membership{
          .user_id = user_id,
          .lab_id = lab_id,
          .role_id = role_id,
          .invited_by = sctx.user_id,
          .joined_at = now_timestamp(),
      };
      txn->repo<core::LabMembership>().insert(membership, mut);
      txn->commit();

      fill_lab_member(resp->mutable_member(), membership);
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

  grpc::Status LabServiceImpl::RevokeMembership(grpc::ServerContext* ctx,
                                                const fmgr::v1::RevokeMembershipRequest* req,
                                                fmgr::v1::RevokeMembershipResponse* /*resp*/) {
    try {
      const auto lab_id = core::LabId::parse(req->lab_id());
      const auto user_id = core::UserId::parse(req->user_id());
      const auto sctx =
          middleware_.authorize(extract_bearer(*ctx), core::Permission::UserInvite, lab_id);

      auto txn = backend_.begin(storage::IsolationLevel::Serializable);
      rpc::AuthMiddleware::inject_rls_vars(*txn, sctx);
      txn->repo<core::LabMembership>().soft_delete(
          core::LabMembershipId{.user_id = user_id, .lab_id = lab_id},
          make_ctx(sctx, "revoke_membership"));
      txn->commit();
      return grpc::Status::OK;
    } catch (...) {
      return current_exception_to_grpc_status();
    }
  }

} // namespace fmgr::server
