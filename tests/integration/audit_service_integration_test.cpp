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
#include "storage/sqlite/LoginAttemptRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"
#include "support/FastAuth.h"
#include "support/RegisterRepositories.h"
#include "support/TempSqliteDb.h"

#include <fmgr/v1/audit.grpc.pb.h>
#include <fmgr/v1/auth.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace fmgr::test {
  namespace {

    [[nodiscard]] std::int64_t now_micros() {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
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
        db_ = std::make_unique<TempSqliteDb>("fmgr-audit-test");

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

      // Append one audit row attributed to lab1 by inserting a throwaway Lab with
      // the mutation context's lab_id pinned to lab1 (audit lab_id is taken from
      // MutationContext.lab_id, not the entity). `marker` lands in the row's
      // request_id so a test can identify exactly which event it expects.
      void append_lab1_event(const std::string& marker) {
        static std::atomic<int> counter{100};
        const int idx = counter.fetch_add(1);
        std::ostringstream oss;
        oss << "30000000-0000-0000-0000-" << std::setw(12) << std::setfill('0') << idx;
        const core::Lab lab{
            .id = core::LabId::parse(oss.str()),
            .name = marker,
            .contact = "watch@example.com",
            .created_at = core::Timestamp::from_unix_micros(1),
            .settings_json = nlohmann::json::object(),
        };
        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "watch-test",
            .request_id = marker,
            .reason = "watch feed test mutation",
            .lab_id = kLab1,
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, ctx);
        txn->commit();
      }

      const std::string kAdminEmail{"admin@example.com"};
      const std::string kMemberEmail{"member@example.com"};
      const std::string kOutsiderEmail{"outsider@example.com"};
      const std::string kPassword{"hunter22"};
      const std::string kLab1{"20000000-0000-0000-0000-000000000001"};
      const std::string kLab2{"20000000-0000-0000-0000-000000000002"};

      std::unique_ptr<TempSqliteDb> db_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::AuditService::Stub> audit_stub_;

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

    // Regression: the `since` filter is a >= range predicate, not equality. The
    // audit repositories formerly rendered every predicate as `column = ?`, so
    // `since` silently matched nothing. A row appended after `before` must be
    // returned when since=before and excluded when since is past its timestamp.
    TEST_F(AuditServiceTest, ListSinceFiltersByTimestampRange) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());

      const std::int64_t before = now_micros();
      append_lab1_event("since-marker");
      const std::int64_t after = now_micros();

      const auto list_since = [&](std::int64_t since) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListAuditEventsRequest req;
        req.set_lab_id(kLab1);
        req.mutable_since()->set_unix_micros(since);
        fmgr::v1::ListAuditEventsResponse resp;
        EXPECT_TRUE(audit_stub_->ListAuditEvents(&ctx, req, &resp).ok());
        return resp;
      };

      const auto included = list_since(before);
      ASSERT_EQ(included.events_size(), 1);
      EXPECT_EQ(included.events(0).request_id(), "since-marker");

      const auto excluded = list_since(after + 1);
      EXPECT_EQ(excluded.events_size(), 0);
    }

    // =====================================================================
    // WatchAuditFeed (server-streaming live feed)
    // =====================================================================

    // The lab-scoped feed tails newly-appended rows. Open the stream from "now",
    // append a lab1-scoped audit row, and the reader must receive exactly that
    // event (poll cadence is ~1s, so a generous client deadline covers it).
    TEST_F(AuditServiceTest, WatchAuditFeedStreamsNewLabScopedEvent) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());

      const std::int64_t since = now_micros();
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));
      fmgr::v1::WatchAuditFeedRequest req;
      req.set_lab_id(kLab1);
      req.mutable_since()->set_unix_micros(since);
      auto reader = audit_stub_->WatchAuditFeed(&ctx, req);

      std::thread appender([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        append_lab1_event("watch-marker");
      });

      fmgr::v1::AuditEvent event;
      const bool got = reader->Read(&event);
      appender.join();

      ASSERT_TRUE(got) << "no event streamed within deadline";
      EXPECT_EQ(event.lab_id(), kLab1);
      EXPECT_EQ(event.request_id(), "watch-marker");

      ctx.TryCancel(); // end the open stream
      reader->Finish();
    }

    // A Member of lab1 holds no audit.read; the feed must be denied at open.
    TEST_F(AuditServiceTest, WatchAuditFeedRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      ASSERT_FALSE(token.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      fmgr::v1::WatchAuditFeedRequest req;
      req.set_lab_id(kLab1);
      auto reader = audit_stub_->WatchAuditFeed(&ctx, req);

      fmgr::v1::AuditEvent event;
      EXPECT_FALSE(reader->Read(&event));
      const auto status = reader->Finish();
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // A SystemAdmin of lab2 holds nothing for lab1; cross-lab feed is denied.
    TEST_F(AuditServiceTest, WatchAuditFeedLabScopedRejectsOutsider) {
      const auto token = login(kOutsiderEmail, kPassword);
      ASSERT_FALSE(token.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      fmgr::v1::WatchAuditFeedRequest req;
      req.set_lab_id(kLab1);
      auto reader = audit_stub_->WatchAuditFeed(&ctx, req);

      fmgr::v1::AuditEvent event;
      EXPECT_FALSE(reader->Read(&event));
      const auto status = reader->Finish();
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // No bearer → unauthenticated, even before any event would stream.
    TEST_F(AuditServiceTest, WatchAuditFeedWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      fmgr::v1::WatchAuditFeedRequest req;
      req.set_lab_id(kLab1);
      auto reader = audit_stub_->WatchAuditFeed(&ctx, req);

      fmgr::v1::AuditEvent event;
      EXPECT_FALSE(reader->Read(&event));
      const auto status = reader->Finish();
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

  } // namespace
} // namespace fmgr::test
