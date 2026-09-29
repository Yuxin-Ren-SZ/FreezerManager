// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
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
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/role.grpc.pb.h>
#include <fmgr/v1/session.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace fmgr::test {
  namespace {

    [[nodiscard]] auth::LocalAuthProviderConfig fast_config() {
      auth::LocalAuthProviderConfig cfg;
      cfg.pwhash_memlimit = 8192;
      cfg.pwhash_opslimit = 1;
      return cfg;
    }

    [[nodiscard]] std::filesystem::path unique_db_path() {
      static std::atomic<int> counter{0};
      return std::filesystem::temp_directory_path() /
             ("fmgr-session-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Principals across two labs:
    //   - admin     : SystemAdmin in lab1 (holds session.revoke deployment-wide)
    //   - alice     : Member in lab1 (holds no session.revoke)
    //   - bob       : Member in lab1 (holds no session.revoke)
    //   - lab_admin : LabAdmin in lab1 (holds user.manage_roles there, and is
    //                 the principal that could mint a custom role carrying
    //                 session.revoke while the permission was lab-grantable)
    //   - supervisor: lab1 membership whose *custom* role carries session.revoke.
    //                 Seeded directly, because that grant is exactly what the
    //                 API used to allow: a deployment upgraded from the
    //                 vulnerable version can still hold the row.
    //   - carol     : Member in lab2 only -- a session the lab1 principals have
    //                 no membership relationship with.
    //
    // `bob`/`supervisor` are the attackers in the cross-user tests and
    // `alice`/`carol` the targets; `admin` pins the one cross-user path the API
    // documents as intended, and `lab_admin` pins the lab scope of the grant.
    class SessionServiceTest : public ::testing::Test {
    protected:
      void SetUp() override {
        db_path_ = unique_db_path();
        remove_sqlite_files(db_path_);

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_path_.string()});
        register_all_repositories(*backend_);
        backend_->migrate_to_latest();

        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_config());
        seed();

        server_opts_.listen_address = "localhost:0";
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, server_opts_);
        server_->build();
        server_thread_ = std::thread([this] { server_->wait(); });

        const std::string addr = "localhost:" + std::to_string(server_->bound_port());
        channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
        auth_stub_ = fmgr::v1::AuthService::NewStub(channel_);
        session_stub_ = fmgr::v1::SessionService::NewStub(channel_);
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
        remove_sqlite_files(db_path_);
      }

      // A logged-in session: the bearer token plus the id the RPC revokes.
      struct SignedIn {
        std::string token;
        std::string session_id;
      };

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [[nodiscard]] SignedIn login(const std::string& email, const std::string& password) {
        grpc::ClientContext ctx;
        fmgr::v1::LoginRequest req;
        req.set_email(email);
        req.set_password(password);
        fmgr::v1::LoginResponse resp;
        const auto status = auth_stub_->Login(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
        return SignedIn{.token = resp.session_token(), .session_id = resp.session_id()};
      }

      static void set_bearer(grpc::ClientContext& ctx, const std::string& token) {
        ctx.AddMetadata("authorization", "Bearer " + token);
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [[nodiscard]] grpc::Status revoke(const std::string& token, const std::string& session_id) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::RevokeSessionRequest req;
        req.set_session_id(session_id);
        fmgr::v1::RevokeSessionResponse resp;
        return session_stub_->RevokeSession(&ctx, req, &resp);
      }

      // Create a custom (lab-owned) role as the bearer; returns its id, or an
      // empty string when the call failed.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [[nodiscard]] std::string create_role(const std::string& token, const std::string& lab,
                                            const std::string& name) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateRoleRequest req;
        req.set_lab_id(lab);
        req.set_kind(fmgr::v1::ROLE_KIND_MEMBER);
        req.set_name(name);
        req.set_description("custom role");
        fmgr::v1::CreateRoleResponse resp;
        const auto status = role_stub_->CreateRole(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
        return resp.role().id();
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [[nodiscard]] grpc::Status grant_permission(const std::string& token,
                                                  const std::string& role_id,
                                                  const std::string& permission_key) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GrantPermissionRequest req;
        req.set_role_id(role_id);
        req.set_permission_key(permission_key);
        fmgr::v1::GrantPermissionResponse resp;
        return role_stub_->GrantPermission(&ctx, req, &resp);
      }

      // "Is this token still usable?" — the observable effect a forced logout
      // has on the target. ListSessions is the cheapest authenticated call that
      // needs no seeded reference data.
      [[nodiscard]] grpc::Status list_own_sessions(const std::string& token) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSessionsRequest req;
        fmgr::v1::ListSessionsResponse resp;
        return session_stub_->ListSessions(&ctx, req, &resp);
      }

      [[nodiscard]] bool token_is_usable(const std::string& token) {
        return list_own_sessions(token).ok();
      }

      // A well-formed session id that was never issued.
      static std::string unknown_session_id() {
        return "30000000-0000-0000-0000-0000000000ff";
      }

      const std::string kAdminEmail{"admin@example.com"};
      const std::string kAliceEmail{"alice@example.com"};
      const std::string kBobEmail{"bob@example.com"};
      const std::string kLabAdminEmail{"labadmin@example.com"};
      const std::string kSupervisorEmail{"supervisor@example.com"};
      const std::string kCarolEmail{"carol@example.com"};
      const std::string kPassword{"hunter22"};
      const std::string kLab1{"20000000-0000-0000-0000-000000000001"};
      const std::string kLab2{"20000000-0000-0000-0000-000000000002"};
      // Lab-owned custom role carrying session.revoke, as a deployment running
      // the vulnerable classification could have created through the API.
      const std::string kLegacyRevokerRoleId{"40000000-0000-0000-0000-000000000001"};

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::SessionService::Stub> session_stub_;
      std::unique_ptr<fmgr::v1::RoleService::Stub> role_stub_;

    private:
      static void remove_sqlite_files(const std::filesystem::path& path) {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(std::filesystem::path(path.string() + "-wal"), error);
        std::filesystem::remove(std::filesystem::path(path.string() + "-shm"), error);
      }

      static void register_all_repositories(storage::SqliteBackend& b) {
        storage::register_identity_repositories(b);
        storage::register_role_repositories(b);
        storage::register_session_repositories(b);
        storage::register_audit_repositories(b);
        storage::register_box_geometry_repositories(b);
        storage::register_item_type_repositories(b);
        storage::register_layout_repositories(b);
        storage::register_sample_repositories(b);
        storage::register_share_request_repositories(b);
      }

      void seed() {
        const auto hash = provider_->hash_password(kPassword);
        const core::LabId lab1 = core::LabId::parse(kLab1);
        const core::LabId lab2 = core::LabId::parse(kLab2);
        const core::UserId admin_id = core::UserId::parse("10000000-0000-0000-0000-000000000001");
        const core::UserId alice_id = core::UserId::parse("10000000-0000-0000-0000-000000000002");
        const core::UserId bob_id = core::UserId::parse("10000000-0000-0000-0000-000000000003");
        const core::UserId lab_admin_id =
            core::UserId::parse("10000000-0000-0000-0000-000000000004");
        const core::UserId supervisor_id =
            core::UserId::parse("10000000-0000-0000-0000-000000000005");
        const core::UserId carol_id = core::UserId::parse("10000000-0000-0000-0000-000000000006");
        const core::RoleId legacy_role_id = core::RoleId::parse(kLegacyRevokerRoleId);

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
                                        const core::RoleId& role_id) {
          return core::LabMembership{
              .user_id = uid,
              .lab_id = lab,
              .role_id = role_id,
              .joined_at = core::Timestamp::from_unix_micros(1),
          };
        };
        const auto builtin = [](core::RoleKind kind) { return core::builtin_role_id(kind); };
        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(lab1, "Session Lab One"), ctx);
        txn->repo<core::Lab>().insert(make_lab(lab2, "Session Lab Two"), ctx);
        txn->repo<core::User>().insert(make_user(admin_id, kAdminEmail), ctx);
        txn->repo<core::User>().insert(make_user(alice_id, kAliceEmail), ctx);
        txn->repo<core::User>().insert(make_user(bob_id, kBobEmail), ctx);
        txn->repo<core::User>().insert(make_user(lab_admin_id, kLabAdminEmail), ctx);
        txn->repo<core::User>().insert(make_user(supervisor_id, kSupervisorEmail), ctx);
        txn->repo<core::User>().insert(make_user(carol_id, kCarolEmail), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(admin_id, lab1, builtin(core::RoleKind::SystemAdmin)), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(alice_id, lab1, builtin(core::RoleKind::Member)), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(bob_id, lab1, builtin(core::RoleKind::Member)), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(lab_admin_id, lab1, builtin(core::RoleKind::LabAdmin)), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(carol_id, lab2, builtin(core::RoleKind::Member)), ctx);
        // The custom lab role and the supervisor's membership in it. Seeded at
        // the repository layer rather than through CreateRole/GrantPermission
        // because the point of the test that reads it is that such a row must
        // stay inert even when it exists.
        txn->repo<core::Role>().insert(
            core::Role{
                .id = legacy_role_id,
                .lab_id = lab1,
                .kind = core::RoleKind::Member,
                .name = "session-revoker",
                .description = "lab-owned role with session.revoke",
                .is_builtin = false,
                .created_at = core::Timestamp::from_unix_micros(1),
            },
            ctx);
        txn->repo<core::RolePermission>().insert(
            core::RolePermission{.role_id = legacy_role_id,
                                 .permission = core::Permission::SessionRevoke},
            ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(supervisor_id, lab1, legacy_role_id), ctx);
        txn->commit();
      }
    };

    // ---- The vulnerability (#77): cross-user revoke ----

    TEST_F(SessionServiceTest, MemberCannotRevokeAnotherUsersSession) {
      const auto alice = login(kAliceEmail, kPassword);
      const auto bob = login(kBobEmail, kPassword);
      ASSERT_TRUE(token_is_usable(alice.token));

      const auto status = revoke(bob.token, alice.session_id);

      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED) << status.error_message();
      // The decisive effect: a forced logout is what the bug delivered.
      EXPECT_TRUE(token_is_usable(alice.token));
    }

    TEST_F(SessionServiceTest, MemberCannotRevokeUnknownSessionId) {
      const auto bob = login(kBobEmail, kPassword);

      EXPECT_EQ(revoke(bob.token, unknown_session_id()).error_code(),
                grpc::StatusCode::PERMISSION_DENIED);
    }

    // A denied cross-user revoke must not become an existence oracle: the
    // status for someone else's real session and for a never-issued id is the
    // same byte for byte.
    TEST_F(SessionServiceTest, DeniedRevokeDoesNotRevealWhetherTheSessionExists) {
      const auto alice = login(kAliceEmail, kPassword);
      const auto bob = login(kBobEmail, kPassword);

      const auto foreign = revoke(bob.token, alice.session_id);
      const auto missing = revoke(bob.token, unknown_session_id());

      EXPECT_EQ(foreign.error_code(), missing.error_code());
      EXPECT_EQ(foreign.error_message(), missing.error_message());
    }

    // ---- The exception must not be reachable from lab scope ----

    // session.revoke is deployment-level: the catalog describes it as "revoke
    // another user's sessions" and the proto allows it only to a SystemAdmin.
    // A LabAdmin holds user.manage_roles for its own lab, so if the permission
    // is lab-grantable it can mint itself a custom role carrying it -- and,
    // because RevokeSessionRequest carries no lab, spend it deployment-wide.
    TEST_F(SessionServiceTest, LabAdminCannotGrantSessionRevokeToALabRole) {
      const auto lab_admin = login(kLabAdminEmail, kPassword);
      const auto role_id = create_role(lab_admin.token, kLab1, "session-revoker-attempt");
      ASSERT_FALSE(role_id.empty());

      const auto status = grant_permission(lab_admin.token, role_id, "session.revoke");

      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION)
          << status.error_message();
    }

    // The regression test for the classification: even when a lab-owned role
    // carrying session.revoke exists -- the row the vulnerable version could
    // create, which an upgraded deployment still holds -- holding it must not
    // unlock a cross-user revoke. The grant is inert because a global-only
    // permission reaches global_permissions only through a SystemAdmin-kind
    // role, and a lab admin cannot mint one of those.
    TEST_F(SessionServiceTest, LabScopedSessionRevokeGrantCannotRevokeAnotherUsersSession) {
      const auto supervisor = login(kSupervisorEmail, kPassword);
      const auto carol = login(kCarolEmail, kPassword);
      ASSERT_TRUE(token_is_usable(supervisor.token));
      ASSERT_TRUE(token_is_usable(carol.token));

      const auto status = revoke(supervisor.token, carol.session_id);

      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED) << status.error_message();
      EXPECT_TRUE(token_is_usable(carol.token));
    }

    // ---- The normal logout path must not regress ----

    TEST_F(SessionServiceTest, MemberCanRevokeOwnSession) {
      const auto alice = login(kAliceEmail, kPassword);

      const auto status = revoke(alice.token, alice.session_id);

      EXPECT_TRUE(status.ok()) << status.error_message();
      EXPECT_FALSE(token_is_usable(alice.token));
    }

    // Revoking a *different* session of one's own is the "log out my other
    // device" case, so the check must be ownership, not "is it my current
    // session id".
    TEST_F(SessionServiceTest, MemberCanRevokeOwnOtherSession) {
      const auto first = login(kAliceEmail, kPassword);
      const auto second = login(kAliceEmail, kPassword);
      ASSERT_NE(first.session_id, second.session_id);

      const auto status = revoke(first.token, second.session_id);

      EXPECT_TRUE(status.ok()) << status.error_message();
      EXPECT_FALSE(token_is_usable(second.token));
      EXPECT_TRUE(token_is_usable(first.token));
    }

    TEST_F(SessionServiceTest, RevokingAnAlreadyRevokedOwnSessionStaysIdempotent) {
      const auto first = login(kAliceEmail, kPassword);
      const auto second = login(kAliceEmail, kPassword);

      EXPECT_TRUE(revoke(first.token, second.session_id).ok());
      EXPECT_TRUE(revoke(first.token, second.session_id).ok());
    }

    // ---- The one cross-user path the API documents as intended ----

    TEST_F(SessionServiceTest, SystemAdminCanRevokeAnotherUsersSession) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto bob = login(kBobEmail, kPassword);
      ASSERT_TRUE(token_is_usable(bob.token));

      const auto status = revoke(admin.token, bob.session_id);

      EXPECT_TRUE(status.ok()) << status.error_message();
      EXPECT_FALSE(token_is_usable(bob.token));
    }

  } // namespace
} // namespace fmgr::test
