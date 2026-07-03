// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "support/FastAuth.h"
#include "support/RegisterRepositories.h"
#include "support/TempSqliteDb.h"
#include "core/identity.h"
#include "core/role.h"
#include "server/FreezerServer.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SessionTraits.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/BoxGeometryRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/ItemTypeRepositories.h"
#include "storage/sqlite/LayoutRepositories.h"
#include "storage/sqlite/LoginAttemptRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/role.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>

namespace fmgr::test {
  namespace {



    // Three principals across two labs:
    //   - admin   : SystemAdmin in lab1 (holds UserManageRoles)
    //   - member  : Member in lab1 (does NOT hold UserManageRoles)
    //   - outsider: SystemAdmin in lab2 only (holds nothing for lab1)
    //
    // `member` is the negative principal for the UserManageRoles gate; `outsider`
    // doubles as the cross-lab isolation principal.
    class RoleServiceTest : public ::testing::Test {
    protected:
      void SetUp() override {
        db_ = std::make_unique<TempSqliteDb>("fmgr-role-test");

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_->string()});
        register_all_sqlite_repositories(*backend_);
        backend_->migrate_to_latest();

        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_auth_config());
        seed();

        server_opts_.listen_address = "localhost:0";
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, server_opts_);
        server_->build();
        server_thread_ = std::thread([this] { server_->wait(); });

        const std::string addr = "localhost:" + std::to_string(server_->bound_port());
        channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        auth_stub_ = fmgr::v1::AuthService::NewStub(channel_);
        role_stub_ = fmgr::v1::RoleService::NewStub(channel_);
      }

      void TearDown() override {
        if (server_) {
          server_->shutdown();
        }
        if (server_thread_.joinable()) {
          server_thread_.join();
        }
        server_.reset();
        provider_.reset();
        backend_.reset();
        db_.reset();
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [[nodiscard]] std::string login(const std::string& email, const std::string& password) {
        grpc::ClientContext ctx;
        fmgr::v1::LoginRequest req;
        req.set_email(email);
        req.set_password(password);
        fmgr::v1::LoginResponse resp;
        if (!auth_stub_->Login(&ctx, req, &resp).ok()) {
          return {};
        }
        return resp.session_token();
      }

      static void set_bearer(grpc::ClientContext& ctx, const std::string& token) {
        ctx.AddMetadata("authorization", "Bearer " + token);
      }

      // Create a custom role in the given lab as the bearer; returns its id.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      std::string create_role(const std::string& token, const std::string& lab,
                              const std::string& name) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateRoleRequest req;
        req.set_lab_id(lab);
        req.set_kind(fmgr::v1::ROLE_KIND_MEMBER);
        req.set_name(name);
        req.set_description("custom role");
        fmgr::v1::CreateRoleResponse resp;
        EXPECT_TRUE(role_stub_->CreateRole(&ctx, req, &resp).ok());
        return resp.role().id();
      }

      const std::string kAdminEmail{"admin@example.com"};
      const std::string kMemberEmail{"member@example.com"};
      const std::string kOutsiderEmail{"outsider@example.com"};
      const std::string kPassword{"hunter22"};
      const std::string kLab1{"20000000-0000-0000-0000-000000000001"};
      const std::string kLab2{"20000000-0000-0000-0000-000000000002"};
      // Built-in (global, lab_id NULL) Member role — see core::builtin_role_id.
      const std::string kBuiltinMemberRoleId{"00000000-0000-0000-0000-000000000003"};

      std::unique_ptr<TempSqliteDb> db_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::RoleService::Stub> role_stub_;

    private:


      void seed() {
        const auto hash = provider_->hash_password(kPassword);
        const core::LabId lab1 = core::LabId::parse(kLab1);
        const core::LabId lab2 = core::LabId::parse(kLab2);
        const core::UserId admin_id = core::UserId::parse("10000000-0000-0000-0000-000000000001");
        const core::UserId member_id = core::UserId::parse("10000000-0000-0000-0000-000000000002");
        const core::UserId outsider_id =
            core::UserId::parse("10000000-0000-0000-0000-000000000003");

        const auto make_lab = [](const core::LabId& id, const std::string& name) {
          return core::Lab{
              .id = id,
              .name = name,
              .contact = "test@example.com",
              .created_at = core::Timestamp::from_unix_micros(1),
              .settings_json = nlohmann::json::object(),
          };
        };
        const auto make_user = [&hash](const core::UserId& id, const std::string& email) {
          return core::User{
              .id = id,
              .primary_email = email,
              .display_name = email,
              .status = core::UserStatus::Active,
              .created_at = core::Timestamp::from_unix_micros(1),
              .auth_bindings = nlohmann::json::array({
                  nlohmann::json::object({{"provider", "local"}, {"hash", hash}}),
              }),
          };
        };
        const auto make_membership = [](const core::UserId& uid, const core::LabId& lab,
                                        core::RoleKind kind) {
          return core::LabMembership{
              .user_id = uid,
              .lab_id = lab,
              .role_id = core::builtin_role_id(kind),
              .joined_at = core::Timestamp::from_unix_micros(1),
          };
        };
        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(lab1, "Lab One"), ctx);
        txn->repo<core::Lab>().insert(make_lab(lab2, "Lab Two"), ctx);
        txn->repo<core::User>().insert(make_user(admin_id, kAdminEmail), ctx);
        txn->repo<core::User>().insert(make_user(member_id, kMemberEmail), ctx);
        txn->repo<core::User>().insert(make_user(outsider_id, kOutsiderEmail), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(admin_id, lab1, core::RoleKind::SystemAdmin), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(member_id, lab1, core::RoleKind::Member), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(outsider_id, lab2, core::RoleKind::SystemAdmin), ctx);
        txn->commit();
      }
    };

    // =====================================================================
    // CreateRole
    // =====================================================================

    TEST_F(RoleServiceTest, CreateRoleAsAdminSucceeds) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateRoleRequest req;
      req.set_lab_id(kLab1);
      req.set_kind(fmgr::v1::ROLE_KIND_MEMBER);
      req.set_name("Technician");
      req.set_description("bench staff");
      fmgr::v1::CreateRoleResponse resp;
      const auto status = role_stub_->CreateRole(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.role().name(), "Technician");
      EXPECT_EQ(resp.role().lab_id(), kLab1);
      EXPECT_FALSE(resp.role().is_builtin());
      EXPECT_FALSE(resp.role().id().empty());
    }

    TEST_F(RoleServiceTest, CreateRoleRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateRoleRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Sneaky");
      fmgr::v1::CreateRoleResponse resp;
      const auto status = role_stub_->CreateRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(RoleServiceTest, CreateRoleWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::CreateRoleRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Anon");
      fmgr::v1::CreateRoleResponse resp;
      const auto status = role_stub_->CreateRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(RoleServiceTest, OutsiderCannotCreateRoleInForeignLab) {
      const auto token = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateRoleRequest req;
      req.set_lab_id(kLab1); // outsider only belongs to lab2
      req.set_name("Trespass");
      fmgr::v1::CreateRoleResponse resp;
      const auto status = role_stub_->CreateRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // GetRole
    // =====================================================================

    TEST_F(RoleServiceTest, GetRoleReturnsCustomRoleForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      const auto role_id = create_role(token, kLab1, "Technician");
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetRoleRequest req;
      req.set_role_id(role_id);
      fmgr::v1::GetRoleResponse resp;
      ASSERT_TRUE(role_stub_->GetRole(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.role().name(), "Technician");
    }

    TEST_F(RoleServiceTest, GetCustomRoleRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto role_id = create_role(admin, kLab1, "Technician");
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::GetRoleRequest req;
      req.set_role_id(role_id);
      fmgr::v1::GetRoleResponse resp;
      const auto status = role_stub_->GetRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(RoleServiceTest, GetBuiltinRoleSucceedsForMember) {
      // Global built-in role definitions are readable by any authenticated user.
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::GetRoleRequest req;
      req.set_role_id(kBuiltinMemberRoleId);
      fmgr::v1::GetRoleResponse resp;
      ASSERT_TRUE(role_stub_->GetRole(&ctx, req, &resp).ok());
      EXPECT_TRUE(resp.role().is_builtin());
      EXPECT_FALSE(resp.role().has_lab_id());
    }

    TEST_F(RoleServiceTest, OutsiderCannotGetForeignCustomRole) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto role_id = create_role(admin, kLab1, "Technician");
      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
      fmgr::v1::GetRoleRequest req;
      req.set_role_id(role_id);
      fmgr::v1::GetRoleResponse resp;
      const auto status = role_stub_->GetRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // ListRoles
    // =====================================================================

    TEST_F(RoleServiceTest, ListRolesReturnsCustomAndBuiltinForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      create_role(token, kLab1, "Technician");
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListRolesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListRolesResponse resp;
      ASSERT_TRUE(role_stub_->ListRoles(&ctx, req, &resp).ok());
      bool saw_custom = false;
      bool saw_builtin = false;
      for (const auto& role : resp.roles()) {
        if (role.name() == "Technician") {
          saw_custom = true;
        }
        if (role.is_builtin()) {
          saw_builtin = true;
        }
      }
      EXPECT_TRUE(saw_custom);
      EXPECT_TRUE(saw_builtin);
    }

    TEST_F(RoleServiceTest, ListRolesRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListRolesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListRolesResponse resp;
      const auto status = role_stub_->ListRoles(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(RoleServiceTest, ListRolesExcludesForeignLabCustomRole) {
      // outsider (admin of lab2) creates a custom role in lab2.
      const auto outsider = login(kOutsiderEmail, kPassword);
      create_role(outsider, kLab2, "Lab2Only");
      // admin lists lab1 roles; the lab2 custom role must not appear.
      const auto admin = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, admin);
      fmgr::v1::ListRolesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListRolesResponse resp;
      ASSERT_TRUE(role_stub_->ListRoles(&ctx, req, &resp).ok());
      for (const auto& role : resp.roles()) {
        EXPECT_NE(role.name(), "Lab2Only");
      }
    }

    // =====================================================================
    // UpdateRole
    // =====================================================================

    TEST_F(RoleServiceTest, UpdateRoleChangesNameForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      const auto role_id = create_role(token, kLab1, "Technician");
      fmgr::v1::Role created;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GetRoleRequest req;
        req.set_role_id(role_id);
        fmgr::v1::GetRoleResponse resp;
        ASSERT_TRUE(role_stub_->GetRole(&ctx, req, &resp).ok());
        created = resp.role();
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateRoleRequest req;
      *req.mutable_role() = created;
      req.mutable_role()->set_name("Senior Technician");
      fmgr::v1::UpdateRoleResponse resp;
      ASSERT_TRUE(role_stub_->UpdateRole(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.role().name(), "Senior Technician");
    }

    TEST_F(RoleServiceTest, UpdateRoleRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto role_id = create_role(admin, kLab1, "Technician");
      fmgr::v1::Role created;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::GetRoleRequest req;
        req.set_role_id(role_id);
        fmgr::v1::GetRoleResponse resp;
        ASSERT_TRUE(role_stub_->GetRole(&ctx, req, &resp).ok());
        created = resp.role();
      }
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::UpdateRoleRequest req;
      *req.mutable_role() = created;
      req.mutable_role()->set_name("Hijack");
      fmgr::v1::UpdateRoleResponse resp;
      const auto status = role_stub_->UpdateRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(RoleServiceTest, UpdateBuiltinRoleFailsPrecondition) {
      const auto admin = login(kAdminEmail, kPassword);
      fmgr::v1::Role builtin;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::GetRoleRequest req;
        req.set_role_id(kBuiltinMemberRoleId);
        fmgr::v1::GetRoleResponse resp;
        ASSERT_TRUE(role_stub_->GetRole(&ctx, req, &resp).ok());
        builtin = resp.role();
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, admin);
      fmgr::v1::UpdateRoleRequest req;
      *req.mutable_role() = builtin;
      req.mutable_role()->set_name("Tampered");
      fmgr::v1::UpdateRoleResponse resp;
      const auto status = role_stub_->UpdateRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    }

    // =====================================================================
    // ArchiveRole
    // =====================================================================

    TEST_F(RoleServiceTest, ArchiveRoleHidesFromGetForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      const auto role_id = create_role(token, kLab1, "Technician");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ArchiveRoleRequest req;
        req.set_role_id(role_id);
        fmgr::v1::ArchiveRoleResponse resp;
        ASSERT_TRUE(role_stub_->ArchiveRole(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetRoleRequest req;
      req.set_role_id(role_id);
      fmgr::v1::GetRoleResponse resp;
      const auto status = role_stub_->GetRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    }

    TEST_F(RoleServiceTest, ArchiveRoleRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto role_id = create_role(admin, kLab1, "Technician");
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ArchiveRoleRequest req;
      req.set_role_id(role_id);
      fmgr::v1::ArchiveRoleResponse resp;
      const auto status = role_stub_->ArchiveRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(RoleServiceTest, ArchiveBuiltinRoleFailsPrecondition) {
      const auto admin = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, admin);
      fmgr::v1::ArchiveRoleRequest req;
      req.set_role_id(kBuiltinMemberRoleId);
      fmgr::v1::ArchiveRoleResponse resp;
      const auto status = role_stub_->ArchiveRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    }

    // =====================================================================
    // GrantPermission / RevokePermission / ListRolePermissions
    // =====================================================================

    TEST_F(RoleServiceTest, GrantPermissionThenListShowsIt) {
      const auto token = login(kAdminEmail, kPassword);
      const auto role_id = create_role(token, kLab1, "Technician");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GrantPermissionRequest req;
        req.set_role_id(role_id);
        req.set_permission_key("sample.read");
        fmgr::v1::GrantPermissionResponse resp;
        ASSERT_TRUE(role_stub_->GrantPermission(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListRolePermissionsRequest req;
      req.set_role_id(role_id);
      fmgr::v1::ListRolePermissionsResponse resp;
      ASSERT_TRUE(role_stub_->ListRolePermissions(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.permission_keys_size(), 1);
      EXPECT_EQ(resp.permission_keys(0), "sample.read");
    }

    TEST_F(RoleServiceTest, GrantPermissionIsIdempotent) {
      const auto token = login(kAdminEmail, kPassword);
      const auto role_id = create_role(token, kLab1, "Technician");
      for (int i = 0; i < 2; ++i) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GrantPermissionRequest req;
        req.set_role_id(role_id);
        req.set_permission_key("sample.read");
        fmgr::v1::GrantPermissionResponse resp;
        ASSERT_TRUE(role_stub_->GrantPermission(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListRolePermissionsRequest req;
      req.set_role_id(role_id);
      fmgr::v1::ListRolePermissionsResponse resp;
      ASSERT_TRUE(role_stub_->ListRolePermissions(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.permission_keys_size(), 1);
    }

    TEST_F(RoleServiceTest, GrantPermissionRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto role_id = create_role(admin, kLab1, "Technician");
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::GrantPermissionRequest req;
      req.set_role_id(role_id);
      req.set_permission_key("sample.read");
      fmgr::v1::GrantPermissionResponse resp;
      const auto status = role_stub_->GrantPermission(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(RoleServiceTest, GrantPermissionWithBadKeyIsInvalidArgument) {
      const auto token = login(kAdminEmail, kPassword);
      const auto role_id = create_role(token, kLab1, "Technician");
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GrantPermissionRequest req;
      req.set_role_id(role_id);
      req.set_permission_key("bogus.permission");
      fmgr::v1::GrantPermissionResponse resp;
      const auto status = role_stub_->GrantPermission(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(RoleServiceTest, GrantPermissionOnBuiltinFailsPrecondition) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GrantPermissionRequest req;
      req.set_role_id(kBuiltinMemberRoleId);
      req.set_permission_key("sample.read");
      fmgr::v1::GrantPermissionResponse resp;
      const auto status = role_stub_->GrantPermission(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    }

    // F-1 escalation guard: a lab-owned custom role must never carry a global-only
    // permission. resolve_permissions promotes such a grant into global_permissions
    // for any SystemAdmin-kind role, so allowing it would be a path to deployment
    // SystemAdmin. See doc/CODE_REVIEW_2026-06-12.md.
    TEST_F(RoleServiceTest, GrantGlobalOnlyPermissionFailsPrecondition) {
      const auto token = login(kAdminEmail, kPassword);
      const auto role_id = create_role(token, kLab1, "Technician");
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GrantPermissionRequest req;
      req.set_role_id(role_id);
      req.set_permission_key("lab.provision");
      fmgr::v1::GrantPermissionResponse resp;
      const auto status = role_stub_->GrantPermission(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    }

    // F-1 escalation guard: a lab admin may not mint a SystemAdmin-kind custom role.
    TEST_F(RoleServiceTest, CreateRoleWithSystemAdminKindIsRejected) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateRoleRequest req;
      req.set_lab_id(kLab1);
      req.set_kind(fmgr::v1::ROLE_KIND_SYSTEM_ADMIN);
      req.set_name("Backdoor");
      req.set_description("escalation attempt");
      fmgr::v1::CreateRoleResponse resp;
      const auto status = role_stub_->CreateRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    // #10: an unspecified role kind is rejected rather than silently defaulting.
    TEST_F(RoleServiceTest, CreateRoleWithUnspecifiedKindIsRejected) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateRoleRequest req;
      req.set_lab_id(kLab1);
      req.set_kind(fmgr::v1::ROLE_KIND_UNSPECIFIED);
      req.set_name("Ambiguous");
      fmgr::v1::CreateRoleResponse resp;
      const auto status = role_stub_->CreateRole(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(RoleServiceTest, RevokePermissionRemovesIt) {
      const auto token = login(kAdminEmail, kPassword);
      const auto role_id = create_role(token, kLab1, "Technician");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GrantPermissionRequest req;
        req.set_role_id(role_id);
        req.set_permission_key("sample.read");
        fmgr::v1::GrantPermissionResponse resp;
        ASSERT_TRUE(role_stub_->GrantPermission(&ctx, req, &resp).ok());
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::RevokePermissionRequest req;
        req.set_role_id(role_id);
        req.set_permission_key("sample.read");
        fmgr::v1::RevokePermissionResponse resp;
        ASSERT_TRUE(role_stub_->RevokePermission(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListRolePermissionsRequest req;
      req.set_role_id(role_id);
      fmgr::v1::ListRolePermissionsResponse resp;
      ASSERT_TRUE(role_stub_->ListRolePermissions(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.permission_keys_size(), 0);
    }

    TEST_F(RoleServiceTest, RevokePermissionRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto role_id = create_role(admin, kLab1, "Technician");
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::RevokePermissionRequest req;
      req.set_role_id(role_id);
      req.set_permission_key("sample.read");
      fmgr::v1::RevokePermissionResponse resp;
      const auto status = role_stub_->RevokePermission(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(RoleServiceTest, ListRolePermissionsRejectsMemberForCustomRole) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto role_id = create_role(admin, kLab1, "Technician");
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ListRolePermissionsRequest req;
      req.set_role_id(role_id);
      fmgr::v1::ListRolePermissionsResponse resp;
      const auto status = role_stub_->ListRolePermissions(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

  } // namespace
} // namespace fmgr::test
