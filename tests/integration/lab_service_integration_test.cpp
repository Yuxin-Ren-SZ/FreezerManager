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

#include <fmgr/v1/audit.grpc.pb.h>
#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/lab.grpc.pb.h>
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
             ("fmgr-lab-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Two principals share lab1: an admin (built-in SystemAdmin role, holds
    // global lab.provision + per-lab lab.configure/lab.enable_phi/user.invite) and
    // a plain member (built-in Member role, holds none of those). Every RPC is
    // exercised with at least one principal who may call it and one who may not.
    class LabServiceTest : public ::testing::Test {
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
        lab_stub_ = fmgr::v1::LabService::NewStub(channel_);
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
      const std::string kPassword{"hunter22"};
      const std::string kLab1{"20000000-0000-0000-0000-000000000001"};

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::LabService::Stub> lab_stub_;
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
        storage::register_login_attempt_repositories(b);
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
        const core::LabId lab_id = core::LabId::parse(kLab1);
        const core::UserId admin_id = core::UserId::parse("10000000-0000-0000-0000-000000000001");
        const core::UserId member_id = core::UserId::parse("10000000-0000-0000-0000-000000000002");

        const core::Lab lab{
            .id = lab_id,
            .name = "Test Lab",
            .contact = "test@example.com",
            .created_at = core::Timestamp::from_unix_micros(1),
            .settings_json = nlohmann::json::object(),
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
        const auto make_membership = [&lab_id](const core::UserId& uid, core::RoleKind kind) {
          return core::LabMembership{
              .user_id = uid,
              .lab_id = lab_id,
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
        txn->repo<core::Lab>().insert(lab, ctx);
        txn->repo<core::User>().insert(make_user(admin_id, kAdminEmail), ctx);
        txn->repo<core::User>().insert(make_user(member_id, kMemberEmail), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(admin_id, core::RoleKind::SystemAdmin), ctx);
        txn->repo<core::LabMembership>().insert(make_membership(member_id, core::RoleKind::Member),
                                                ctx);
        txn->commit();
      }
    };

    const std::string kMemberRoleId{"00000000-0000-0000-0000-000000000003"};

    // ---- CreateLab ----

    TEST_F(LabServiceTest, CreateLabAsSystemAdminSucceedsAndIsManageable) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateLabRequest req;
      req.set_name("Genomics Lab");
      req.set_contact("pi@genomics.example");
      fmgr::v1::CreateLabResponse resp;
      const auto status = lab_stub_->CreateLab(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.lab().name(), "Genomics Lab");
      EXPECT_FALSE(resp.lab().id().empty());

      // The provisioner became the lab's admin, so they can GetLab the new lab.
      grpc::ClientContext ctx2;
      set_bearer(ctx2, token);
      fmgr::v1::GetLabRequest get_req;
      get_req.set_lab_id(resp.lab().id());
      fmgr::v1::GetLabResponse get_resp;
      const auto get_status = lab_stub_->GetLab(&ctx2, get_req, &get_resp);
      EXPECT_TRUE(get_status.ok()) << get_status.error_message();
      EXPECT_EQ(get_resp.lab().name(), "Genomics Lab");
    }

    // C-12 / PRD §17: a caller-supplied x-request-id is propagated through the
    // handler into the mutation's audit row, so a request can be correlated across
    // the log pipeline and the audit chain.
    TEST_F(LabServiceTest, CreateLabPropagatesRequestIdIntoAuditRow) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());

      const std::string marker = "req-c12-correlation-0001";
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      ctx.AddMetadata("x-request-id", marker);
      fmgr::v1::CreateLabRequest req;
      req.set_name("Correlated Lab");
      fmgr::v1::CreateLabResponse resp;
      ASSERT_TRUE(lab_stub_->CreateLab(&ctx, req, &resp).ok());

      // The system admin reads the deployment-wide audit feed and finds the lab
      // creation carrying the supplied request id.
      grpc::ClientContext audit_ctx;
      set_bearer(audit_ctx, token);
      fmgr::v1::ListAuditEventsRequest audit_req; // no lab_id → deployment-wide
      fmgr::v1::ListAuditEventsResponse audit_resp;
      ASSERT_TRUE(audit_stub_->ListAuditEvents(&audit_ctx, audit_req, &audit_resp).ok());

      bool found = false;
      for (const auto& event : audit_resp.events()) {
        if (event.request_id() == marker) {
          found = true;
          break;
        }
      }
      EXPECT_TRUE(found) << "no audit event carried the supplied x-request-id";
    }

    // With no x-request-id supplied, the server still stamps a non-empty
    // correlation id (a generated UUID) so every mutation is traceable.
    TEST_F(LabServiceTest, CreateLabStampsGeneratedRequestIdWhenNoneSupplied) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateLabRequest req;
      req.set_name("Uncorrelated Lab");
      fmgr::v1::CreateLabResponse resp;
      ASSERT_TRUE(lab_stub_->CreateLab(&ctx, req, &resp).ok());

      grpc::ClientContext audit_ctx;
      set_bearer(audit_ctx, token);
      fmgr::v1::ListAuditEventsRequest audit_req;
      fmgr::v1::ListAuditEventsResponse audit_resp;
      ASSERT_TRUE(audit_stub_->ListAuditEvents(&audit_ctx, audit_req, &audit_resp).ok());

      bool saw_lab_create_with_id = false;
      for (const auto& event : audit_resp.events()) {
        if (event.entity_kind() == "lab" && !event.request_id().empty()) {
          saw_lab_create_with_id = true;
          break;
        }
      }
      EXPECT_TRUE(saw_lab_create_with_id) << "lab mutation lacked a generated request_id";
    }

    TEST_F(LabServiceTest, CreateLabRejectsCallerWithoutLabProvision) {
      const auto token = login(kMemberEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateLabRequest req;
      req.set_name("Sneaky Lab");
      fmgr::v1::CreateLabResponse resp;
      const auto status = lab_stub_->CreateLab(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(LabServiceTest, CreateLabWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::CreateLabRequest req;
      req.set_name("Anon Lab");
      fmgr::v1::CreateLabResponse resp;
      const auto status = lab_stub_->CreateLab(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    // ---- GetLab ----

    TEST_F(LabServiceTest, GetLabReturnsSeededLabForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetLabRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::GetLabResponse resp;
      const auto status = lab_stub_->GetLab(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.lab().name(), "Test Lab");
    }

    TEST_F(LabServiceTest, GetLabRejectsMemberWithoutLabConfigure) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetLabRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::GetLabResponse resp;
      const auto status = lab_stub_->GetLab(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // ---- UpdateLab ----

    TEST_F(LabServiceTest, UpdateLabChangesNameForAdmin) {
      const auto token = login(kAdminEmail, kPassword);

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateLabRequest req;
      req.mutable_lab()->set_id(kLab1);
      req.mutable_lab()->set_name("Renamed Lab");
      req.mutable_lab()->set_contact("new@example.com");
      fmgr::v1::UpdateLabResponse resp;
      const auto status = lab_stub_->UpdateLab(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.lab().name(), "Renamed Lab");

      grpc::ClientContext ctx2;
      set_bearer(ctx2, token);
      fmgr::v1::GetLabRequest get_req;
      get_req.set_lab_id(kLab1);
      fmgr::v1::GetLabResponse get_resp;
      ASSERT_TRUE(lab_stub_->GetLab(&ctx2, get_req, &get_resp).ok());
      EXPECT_EQ(get_resp.lab().name(), "Renamed Lab");
    }

    TEST_F(LabServiceTest, UpdateLabRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateLabRequest req;
      req.mutable_lab()->set_id(kLab1);
      req.mutable_lab()->set_name("Hijacked");
      fmgr::v1::UpdateLabResponse resp;
      const auto status = lab_stub_->UpdateLab(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // ---- EnablePhi ----

    TEST_F(LabServiceTest, EnablePhiSetsFlagForAdmin) {
      const auto token = login(kAdminEmail, kPassword);

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::EnablePhiRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::EnablePhiResponse resp;
      ASSERT_TRUE(lab_stub_->EnablePhi(&ctx, req, &resp).ok());

      grpc::ClientContext ctx2;
      set_bearer(ctx2, token);
      fmgr::v1::GetLabRequest get_req;
      get_req.set_lab_id(kLab1);
      fmgr::v1::GetLabResponse get_resp;
      ASSERT_TRUE(lab_stub_->GetLab(&ctx2, get_req, &get_resp).ok());
      EXPECT_TRUE(get_resp.lab().is_phi_enabled());
    }

    TEST_F(LabServiceTest, EnablePhiRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::EnablePhiRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::EnablePhiResponse resp;
      const auto status = lab_stub_->EnablePhi(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // ---- ListMembers ----

    TEST_F(LabServiceTest, ListMembersReturnsSeededForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListMembersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListMembersResponse resp;
      ASSERT_TRUE(lab_stub_->ListMembers(&ctx, req, &resp).ok());
      EXPECT_GE(resp.members_size(), 2);
    }

    TEST_F(LabServiceTest, ListMembersRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListMembersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListMembersResponse resp;
      const auto status = lab_stub_->ListMembers(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // ---- InviteMember / RevokeMembership ----

    TEST_F(LabServiceTest, InviteMemberCreatesMembership) {
      const auto token = login(kAdminEmail, kPassword);

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::InviteMemberRequest req;
      req.set_lab_id(kLab1);
      req.set_email("newbie@example.com");
      req.set_role_id(kMemberRoleId);
      fmgr::v1::InviteMemberResponse resp;
      const auto status = lab_stub_->InviteMember(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_FALSE(resp.member().user_id().empty());

      grpc::ClientContext ctx2;
      set_bearer(ctx2, token);
      fmgr::v1::ListMembersRequest list_req;
      list_req.set_lab_id(kLab1);
      fmgr::v1::ListMembersResponse list_resp;
      ASSERT_TRUE(lab_stub_->ListMembers(&ctx2, list_req, &list_resp).ok());
      EXPECT_GE(list_resp.members_size(), 3);
    }

    TEST_F(LabServiceTest, InviteMemberDuplicateReturnsAlreadyExists) {
      const auto token = login(kAdminEmail, kPassword);

      const auto invite = [&]() {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::InviteMemberRequest req;
        req.set_lab_id(kLab1);
        req.set_email("dup@example.com");
        req.set_role_id(kMemberRoleId);
        fmgr::v1::InviteMemberResponse resp;
        return lab_stub_->InviteMember(&ctx, req, &resp);
      };
      ASSERT_TRUE(invite().ok());
      const auto second = invite();
      EXPECT_FALSE(second.ok());
      EXPECT_EQ(second.error_code(), grpc::StatusCode::ALREADY_EXISTS);
    }

    TEST_F(LabServiceTest, InviteMemberRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::InviteMemberRequest req;
      req.set_lab_id(kLab1);
      req.set_email("x@example.com");
      req.set_role_id(kMemberRoleId);
      fmgr::v1::InviteMemberResponse resp;
      const auto status = lab_stub_->InviteMember(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(LabServiceTest, RevokeMembershipHidesFromDefaultList) {
      const auto token = login(kAdminEmail, kPassword);

      // Invite, then revoke.
      std::string invited_user_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::InviteMemberRequest req;
        req.set_lab_id(kLab1);
        req.set_email("revokeme@example.com");
        req.set_role_id(kMemberRoleId);
        fmgr::v1::InviteMemberResponse resp;
        ASSERT_TRUE(lab_stub_->InviteMember(&ctx, req, &resp).ok());
        invited_user_id = resp.member().user_id();
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::RevokeMembershipRequest req;
        req.set_lab_id(kLab1);
        req.set_user_id(invited_user_id);
        fmgr::v1::RevokeMembershipResponse resp;
        ASSERT_TRUE(lab_stub_->RevokeMembership(&ctx, req, &resp).ok());
      }

      const auto count_with = [&](bool include_revoked) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListMembersRequest req;
        req.set_lab_id(kLab1);
        req.set_include_revoked(include_revoked);
        fmgr::v1::ListMembersResponse resp;
        EXPECT_TRUE(lab_stub_->ListMembers(&ctx, req, &resp).ok());
        int found = 0;
        for (const auto& m : resp.members()) {
          if (m.user_id() == invited_user_id) {
            ++found;
          }
        }
        return found;
      };
      EXPECT_EQ(count_with(false), 0);
      EXPECT_EQ(count_with(true), 1);
    }

    // ---- ListLabs (visibility-scoped, not permission-gated) ----

    TEST_F(LabServiceTest, ListLabsReturnsCallerLabs) {
      const auto member_token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member_token);
      fmgr::v1::ListLabsRequest req;
      fmgr::v1::ListLabsResponse resp;
      ASSERT_TRUE(lab_stub_->ListLabs(&ctx, req, &resp).ok());
      ASSERT_GE(resp.labs_size(), 1);
      bool found = false;
      for (const auto& lab : resp.labs()) {
        if (lab.id() == kLab1) {
          found = true;
        }
      }
      EXPECT_TRUE(found);
    }

    TEST_F(LabServiceTest, ListLabsWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::ListLabsRequest req;
      fmgr::v1::ListLabsResponse resp;
      const auto status = lab_stub_->ListLabs(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

  } // namespace
} // namespace fmgr::test
