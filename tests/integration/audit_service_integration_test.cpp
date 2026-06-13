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

#include <fmgr/v1/audit.grpc.pb.h>
#include <fmgr/v1/auth.grpc.pb.h>
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
             ("fmgr-audit-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Three principals:
    //   - admin   : SystemAdmin in lab1 — holds AuditRead/AuditExport for lab1 AND
    //               the global LabProvision marker that identifies a deployment
    //               system administrator.
    //   - member  : Member in lab1 — holds neither AuditRead nor the system-admin
    //               marker (negative principal).
    //   - outsider: SystemAdmin in lab2 (cross-lab principal).
    class AuditServiceTest : public ::testing::Test {
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
        audit_stub_ = fmgr::v1::AuditService::NewStub(channel_);
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

      const std::string kAdminEmail{"admin@example.com"};
      const std::string kMemberEmail{"member@example.com"};
      const std::string kOutsiderEmail{"outsider@example.com"};
      const std::string kPassword{"hunter22"};
      const std::string kLab1{"20000000-0000-0000-0000-000000000001"};
      const std::string kLab2{"20000000-0000-0000-0000-000000000002"};

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::AuditService::Stub> audit_stub_;

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
        storage::register_box_repositories(b);
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
    // ListAuditEvents
    // =====================================================================

    TEST_F(AuditServiceTest, ListDeploymentWideReturnsSeedEventsForSystemAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListAuditEventsRequest req; // no lab_id → deployment-wide
      fmgr::v1::ListAuditEventsResponse resp;
      const auto status = audit_stub_->ListAuditEvents(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      // Seeding inserted labs/users/memberships — each appended an audit row.
      EXPECT_GT(resp.events_size(), 0);
    }

    TEST_F(AuditServiceTest, ListDeploymentWideRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListAuditEventsRequest req;
      fmgr::v1::ListAuditEventsResponse resp;
      const auto status = audit_stub_->ListAuditEvents(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(AuditServiceTest, ListWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::ListAuditEventsRequest req;
      fmgr::v1::ListAuditEventsResponse resp;
      const auto status = audit_stub_->ListAuditEvents(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(AuditServiceTest, ListLabScopedSucceedsForLabAuditReader) {
      // admin holds AuditRead for lab1, so a lab-scoped list is authorised even
      // though no seeded row is attributed to lab1 (audit lab_id is null for the
      // global identity entities created during seeding).
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListAuditEventsRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListAuditEventsResponse resp;
      const auto status = audit_stub_->ListAuditEvents(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
    }

    TEST_F(AuditServiceTest, ListLabScopedRejectsOutsider) {
      // outsider is a SystemAdmin of lab2 but holds nothing for lab1.
      const auto token = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListAuditEventsRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListAuditEventsResponse resp;
      const auto status = audit_stub_->ListAuditEvents(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(AuditServiceTest, ListFiltersByEntityKind) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListAuditEventsRequest req;
      req.set_entity_kind("lab");
      fmgr::v1::ListAuditEventsResponse resp;
      ASSERT_TRUE(audit_stub_->ListAuditEvents(&ctx, req, &resp).ok());
      ASSERT_GT(resp.events_size(), 0);
      for (const auto& event : resp.events()) {
        EXPECT_EQ(event.entity_kind(), "lab");
      }
    }

    // =====================================================================
    // GetAuditEvent
    // =====================================================================

    TEST_F(AuditServiceTest, GetReturnsEventForSystemAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      std::string event_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListAuditEventsRequest req;
        fmgr::v1::ListAuditEventsResponse resp;
        ASSERT_TRUE(audit_stub_->ListAuditEvents(&ctx, req, &resp).ok());
        ASSERT_GT(resp.events_size(), 0);
        event_id = resp.events(0).id();
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetAuditEventRequest req;
      req.set_audit_event_id(event_id);
      fmgr::v1::GetAuditEventResponse resp;
      ASSERT_TRUE(audit_stub_->GetAuditEvent(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.event().id(), event_id);
      EXPECT_FALSE(resp.event().this_hash().empty());
    }

    TEST_F(AuditServiceTest, GetGlobalEventRejectsMember) {
      // Seeded events carry no lab_id; only a system admin may read them.
      const auto admin = login(kAdminEmail, kPassword);
      std::string event_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::ListAuditEventsRequest req;
        fmgr::v1::ListAuditEventsResponse resp;
        ASSERT_TRUE(audit_stub_->ListAuditEvents(&ctx, req, &resp).ok());
        ASSERT_GT(resp.events_size(), 0);
        event_id = resp.events(0).id();
      }
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::GetAuditEventRequest req;
      req.set_audit_event_id(event_id);
      fmgr::v1::GetAuditEventResponse resp;
      const auto status = audit_stub_->GetAuditEvent(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(AuditServiceTest, GetUnknownEventIsNotFound) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetAuditEventRequest req;
      req.set_audit_event_id("99999999-9999-9999-9999-999999999999");
      fmgr::v1::GetAuditEventResponse resp;
      const auto status = audit_stub_->GetAuditEvent(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    }

    // =====================================================================
    // VerifyAuditChain
    // =====================================================================

    TEST_F(AuditServiceTest, VerifyChainIntactForSystemAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::VerifyAuditChainRequest req;
      fmgr::v1::VerifyAuditChainResponse resp;
      ASSERT_TRUE(audit_stub_->VerifyAuditChain(&ctx, req, &resp).ok());
      EXPECT_TRUE(resp.chain_intact());
      EXPECT_GT(resp.events_verified(), 0);
    }

    TEST_F(AuditServiceTest, VerifyChainRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::VerifyAuditChainRequest req;
      fmgr::v1::VerifyAuditChainResponse resp;
      const auto status = audit_stub_->VerifyAuditChain(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // ExportAuditLog
    // =====================================================================

    TEST_F(AuditServiceTest, ExportReturnsCsvForSystemAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ExportAuditLogRequest req;
      fmgr::v1::ExportAuditLogResponse resp;
      ASSERT_TRUE(audit_stub_->ExportAuditLog(&ctx, req, &resp).ok());
      EXPECT_NE(resp.csv_content().find("this_hash"), std::string::npos);
    }

    TEST_F(AuditServiceTest, ExportRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ExportAuditLogRequest req;
      fmgr::v1::ExportAuditLogResponse resp;
      const auto status = audit_stub_->ExportAuditLog(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

  } // namespace
} // namespace fmgr::test
