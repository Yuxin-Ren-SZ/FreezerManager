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
#include <fmgr/v1/share.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

namespace fmgr::test {
  namespace {



    // Principals spread across three labs:
    //   - src_admin : LabAdmin in lab1 — ShareRequest + ShareApprove for lab1.
    //   - tgt_admin : LabAdmin in lab2 — ShareRequest + ShareApprove for lab2.
    //   - sys_admin : SystemAdmin in lab3 — deployment system-admin marker, signs
    //                 the SystemAdmin leg of the three-signature approval.
    //   - member    : Member in lab1 — holds ShareRequest but NOT ShareApprove.
    //   - stranger  : ReadOnly in lab1 — holds neither ShareRequest nor approve.
    class ShareServiceTest : public ::testing::Test {
    protected:
      void SetUp() override {
        db_ = std::make_unique<TempSqliteDb>("fmgr-share-test");

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
        share_stub_ = fmgr::v1::ShareService::NewStub(channel_);
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

      // Create a lab1 -> lab2 share request as the given bearer; returns its id.
      std::string create_request(const std::string& token) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateShareRequestRequest req;
        req.set_source_lab_id(kLab1);
        req.set_target_lab_id(kLab2);
        req.set_scope_json(R"({"projects":["p1"]})");
        fmgr::v1::CreateShareRequestResponse resp;
        EXPECT_TRUE(share_stub_->CreateShareRequest(&ctx, req, &resp).ok());
        return resp.request().id();
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      grpc::Status approve(const std::string& token, const std::string& id,
                           fmgr::v1::ShareApprovalRole role) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ApproveShareRequestRequest req;
        req.set_share_request_id(id);
        req.set_approver_role(role);
        fmgr::v1::ApproveShareRequestResponse resp;
        return share_stub_->ApproveShareRequest(&ctx, req, &resp);
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      fmgr::v1::ShareRequestStatus get_status(const std::string& token, const std::string& id) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GetShareRequestRequest req;
        req.set_share_request_id(id);
        fmgr::v1::GetShareRequestResponse resp;
        EXPECT_TRUE(share_stub_->GetShareRequest(&ctx, req, &resp).ok());
        return resp.request().status();
      }

      const std::string kSrcAdminEmail{"src@example.com"};
      const std::string kTgtAdminEmail{"tgt@example.com"};
      const std::string kSysAdminEmail{"sys@example.com"};
      const std::string kMemberEmail{"member@example.com"};
      const std::string kStrangerEmail{"stranger@example.com"};
      const std::string kPassword{"hunter22"};
      const std::string kLab1{"20000000-0000-0000-0000-000000000001"};
      const std::string kLab2{"20000000-0000-0000-0000-000000000002"};
      const std::string kLab3{"20000000-0000-0000-0000-000000000003"};

      std::unique_ptr<TempSqliteDb> db_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::ShareService::Stub> share_stub_;

    private:


      void seed() {
        const auto hash = provider_->hash_password(kPassword);
        const core::LabId lab1 = core::LabId::parse(kLab1);
        const core::LabId lab2 = core::LabId::parse(kLab2);
        const core::LabId lab3 = core::LabId::parse(kLab3);

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

        const core::UserId src_id = core::UserId::parse("10000000-0000-0000-0000-000000000001");
        const core::UserId tgt_id = core::UserId::parse("10000000-0000-0000-0000-000000000002");
        const core::UserId sys_id = core::UserId::parse("10000000-0000-0000-0000-000000000003");
        const core::UserId mem_id = core::UserId::parse("10000000-0000-0000-0000-000000000004");
        const core::UserId str_id = core::UserId::parse("10000000-0000-0000-0000-000000000005");

        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(make_lab(lab1, "Lab One"), ctx);
        txn->repo<core::Lab>().insert(make_lab(lab2, "Lab Two"), ctx);
        txn->repo<core::Lab>().insert(make_lab(lab3, "Lab Three"), ctx);
        txn->repo<core::User>().insert(make_user(src_id, kSrcAdminEmail), ctx);
        txn->repo<core::User>().insert(make_user(tgt_id, kTgtAdminEmail), ctx);
        txn->repo<core::User>().insert(make_user(sys_id, kSysAdminEmail), ctx);
        txn->repo<core::User>().insert(make_user(mem_id, kMemberEmail), ctx);
        txn->repo<core::User>().insert(make_user(str_id, kStrangerEmail), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(src_id, lab1, core::RoleKind::LabAdmin), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(tgt_id, lab2, core::RoleKind::LabAdmin), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(sys_id, lab3, core::RoleKind::SystemAdmin), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(mem_id, lab1, core::RoleKind::Member), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(str_id, lab1, core::RoleKind::ReadOnly), ctx);
        txn->commit();
      }
    };

    // =====================================================================
    // CreateShareRequest
    // =====================================================================

    TEST_F(ShareServiceTest, CreateAsSourceAdminSucceeds) {
      const auto token = login(kSrcAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateShareRequestRequest req;
      req.set_source_lab_id(kLab1);
      req.set_target_lab_id(kLab2);
      req.set_scope_json(R"({"projects":["p1"]})");
      fmgr::v1::CreateShareRequestResponse resp;
      const auto status = share_stub_->CreateShareRequest(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.request().source_lab_id(), kLab1);
      EXPECT_EQ(resp.request().target_lab_id(), kLab2);
      EXPECT_EQ(resp.request().status(), fmgr::v1::SHARE_REQUEST_STATUS_PENDING);
      EXPECT_FALSE(resp.request().id().empty());
    }

    TEST_F(ShareServiceTest, CreateRejectsPrincipalWithoutSourceShareRequest) {
      // stranger is ReadOnly in lab1 and lacks the ShareRequest permission.
      const auto token = login(kStrangerEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateShareRequestRequest req;
      req.set_source_lab_id(kLab1);
      req.set_target_lab_id(kLab2);
      fmgr::v1::CreateShareRequestResponse resp;
      const auto status = share_stub_->CreateShareRequest(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ShareServiceTest, CreateFromForeignLabIsRejected) {
      // tgt_admin administers lab2 only; it cannot open a request sourced from lab1.
      const auto token = login(kTgtAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateShareRequestRequest req;
      req.set_source_lab_id(kLab1);
      req.set_target_lab_id(kLab2);
      fmgr::v1::CreateShareRequestResponse resp;
      const auto status = share_stub_->CreateShareRequest(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ShareServiceTest, CreateRejectsSameSourceAndTarget) {
      // A self-share is meaningless; source and target labs must differ (F-5).
      const auto token = login(kSrcAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateShareRequestRequest req;
      req.set_source_lab_id(kLab1);
      req.set_target_lab_id(kLab1);
      fmgr::v1::CreateShareRequestResponse resp;
      const auto status = share_stub_->CreateShareRequest(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(ShareServiceTest, CreateRejectsNonObjectScopeJson) {
      // scope_json must be a JSON object (security-audit-2026-06-12 #6).
      const auto token = login(kSrcAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateShareRequestRequest req;
      req.set_source_lab_id(kLab1);
      req.set_target_lab_id(kLab2);
      req.set_scope_json("[1,2,3]"); // valid JSON, but an array not an object
      fmgr::v1::CreateShareRequestResponse resp;
      const auto status = share_stub_->CreateShareRequest(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(ShareServiceTest, CreateWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::CreateShareRequestRequest req;
      req.set_source_lab_id(kLab1);
      req.set_target_lab_id(kLab2);
      fmgr::v1::CreateShareRequestResponse resp;
      const auto status = share_stub_->CreateShareRequest(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    // =====================================================================
    // GetShareRequest / ListShareRequests
    // =====================================================================

    TEST_F(ShareServiceTest, GetReadableByTargetParticipant) {
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto id = create_request(src);
      const auto tgt = login(kTgtAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, tgt);
      fmgr::v1::GetShareRequestRequest req;
      req.set_share_request_id(id);
      fmgr::v1::GetShareRequestResponse resp;
      ASSERT_TRUE(share_stub_->GetShareRequest(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.request().id(), id);
    }

    TEST_F(ShareServiceTest, GetRejectsStranger) {
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto id = create_request(src);
      const auto stranger = login(kStrangerEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, stranger);
      fmgr::v1::GetShareRequestRequest req;
      req.set_share_request_id(id);
      fmgr::v1::GetShareRequestResponse resp;
      const auto status = share_stub_->GetShareRequest(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ShareServiceTest, ListPendingForSourceAdmin) {
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto id = create_request(src);
      grpc::ClientContext ctx;
      set_bearer(ctx, src);
      fmgr::v1::ListShareRequestsRequest req;
      req.set_source_lab_id(kLab1);
      fmgr::v1::ListShareRequestsResponse resp;
      ASSERT_TRUE(share_stub_->ListShareRequests(&ctx, req, &resp).ok());
      bool found = false;
      for (const auto& request : resp.requests()) {
        if (request.id() == id) {
          found = true;
        }
      }
      EXPECT_TRUE(found);
    }

    TEST_F(ShareServiceTest, ListUnscopedRejectsNonSystemAdmin) {
      const auto src = login(kSrcAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, src);
      fmgr::v1::ListShareRequestsRequest req; // no lab filter
      fmgr::v1::ListShareRequestsResponse resp;
      const auto status = share_stub_->ListShareRequests(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // Approve — three-signature state machine
    // =====================================================================

    TEST_F(ShareServiceTest, ThreeSignaturesReachApproved) {
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto tgt = login(kTgtAdminEmail, kPassword);
      const auto sys = login(kSysAdminEmail, kPassword);
      const auto id = create_request(src);

      ASSERT_TRUE(approve(src, id, fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB).ok());
      // One signature is not enough — still pending.
      EXPECT_EQ(get_status(src, id), fmgr::v1::SHARE_REQUEST_STATUS_PENDING);

      ASSERT_TRUE(approve(tgt, id, fmgr::v1::SHARE_APPROVAL_ROLE_TARGET_LAB).ok());
      EXPECT_EQ(get_status(src, id), fmgr::v1::SHARE_REQUEST_STATUS_PENDING);

      ASSERT_TRUE(approve(sys, id, fmgr::v1::SHARE_APPROVAL_ROLE_SYSTEM_ADMIN).ok());
      EXPECT_EQ(get_status(src, id), fmgr::v1::SHARE_REQUEST_STATUS_APPROVED);
    }

    TEST_F(ShareServiceTest, ApproveRejectsForeignLabSignature) {
      // tgt_admin may not sign as the SOURCE lab — that would forge lab1's vote.
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto tgt = login(kTgtAdminEmail, kPassword);
      const auto id = create_request(src);
      const auto status = approve(tgt, id, fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ShareServiceTest, ApproveRejectsMemberWithoutApprovePermission) {
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto member = login(kMemberEmail, kPassword);
      const auto id = create_request(src);
      const auto status = approve(member, id, fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ShareServiceTest, DuplicateSignatureForSameRoleIsRejected) {
      // Separation of duties catches a repeat signer before the composite-PK path,
      // so a same-role duplicate now surfaces as FAILED_PRECONDITION (see F-2).
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto id = create_request(src);
      ASSERT_TRUE(approve(src, id, fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB).ok());
      const auto status = approve(src, id, fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    }

    TEST_F(ShareServiceTest, OneSignerCannotSignTwoRoles) {
      // Separation of duties: a principal entitled to sign as more than one role
      // (here a system admin who also holds source-lab approve) may still sign only
      // once. Without this, one human could single-handedly approve a transfer.
      const auto sys = login(kSysAdminEmail, kPassword);
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto id = create_request(src);
      ASSERT_TRUE(approve(sys, id, fmgr::v1::SHARE_APPROVAL_ROLE_SYSTEM_ADMIN).ok());
      const auto status = approve(sys, id, fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    }

    // F-3: a decided (rejected) request is hidden from the default list and only
    // surfaces with include_decided. Approved/Rejected are not tombstoned, so this
    // requires an explicit pending-only default filter, not include_tombstoned.
    TEST_F(ShareServiceTest, ListHidesDecidedUnlessRequested) {
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto id = create_request(src);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, src);
        fmgr::v1::RejectShareRequestRequest req;
        req.set_share_request_id(id);
        req.set_approver_role(fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB);
        fmgr::v1::RejectShareRequestResponse resp;
        ASSERT_TRUE(share_stub_->RejectShareRequest(&ctx, req, &resp).ok());
      }

      const auto contains_id = [&](bool include_decided) {
        grpc::ClientContext ctx;
        set_bearer(ctx, src);
        fmgr::v1::ListShareRequestsRequest req;
        req.set_source_lab_id(kLab1);
        req.set_include_decided(include_decided);
        fmgr::v1::ListShareRequestsResponse resp;
        EXPECT_TRUE(share_stub_->ListShareRequests(&ctx, req, &resp).ok());
        return std::ranges::any_of(resp.requests(),
                                   [&](const auto& request) { return request.id() == id; });
      };

      EXPECT_FALSE(contains_id(false)); // default: pending only
      EXPECT_TRUE(contains_id(true));   // include_decided: surfaces the rejected row
    }

    // F-4: a malformed client page_token is a client error, not INTERNAL.
    TEST_F(ShareServiceTest, ListRejectsMalformedPageToken) {
      const auto src = login(kSrcAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, src);
      fmgr::v1::ListShareRequestsRequest req;
      req.set_source_lab_id(kLab1);
      req.mutable_page()->set_page_token("not-a-number");
      fmgr::v1::ListShareRequestsResponse resp;
      const auto status = share_stub_->ListShareRequests(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    // =====================================================================
    // Reject / Revoke
    // =====================================================================

    TEST_F(ShareServiceTest, RejectIsTerminal) {
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto tgt = login(kTgtAdminEmail, kPassword);
      const auto id = create_request(src);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, tgt);
        fmgr::v1::RejectShareRequestRequest req;
        req.set_share_request_id(id);
        req.set_approver_role(fmgr::v1::SHARE_APPROVAL_ROLE_TARGET_LAB);
        fmgr::v1::RejectShareRequestResponse resp;
        ASSERT_TRUE(share_stub_->RejectShareRequest(&ctx, req, &resp).ok());
      }
      EXPECT_EQ(get_status(src, id), fmgr::v1::SHARE_REQUEST_STATUS_REJECTED);
      // A subsequent approval on a decided request is refused.
      const auto status = approve(src, id, fmgr::v1::SHARE_APPROVAL_ROLE_SOURCE_LAB);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    }

    TEST_F(ShareServiceTest, RevokeBySourceAdminMovesToRevoked) {
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto id = create_request(src);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, src);
        fmgr::v1::RevokeShareRequestRequest req;
        req.set_share_request_id(id);
        fmgr::v1::RevokeShareRequestResponse resp;
        ASSERT_TRUE(share_stub_->RevokeShareRequest(&ctx, req, &resp).ok());
      }
      EXPECT_EQ(get_status(src, id), fmgr::v1::SHARE_REQUEST_STATUS_REVOKED);
    }

    TEST_F(ShareServiceTest, RevokeRejectsTargetAdmin) {
      // Only the originating (source) lab may withdraw its own request.
      const auto src = login(kSrcAdminEmail, kPassword);
      const auto tgt = login(kTgtAdminEmail, kPassword);
      const auto id = create_request(src);
      grpc::ClientContext ctx;
      set_bearer(ctx, tgt);
      fmgr::v1::RevokeShareRequestRequest req;
      req.set_share_request_id(id);
      fmgr::v1::RevokeShareRequestResponse resp;
      const auto status = share_stub_->RevokeShareRequest(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

  } // namespace
} // namespace fmgr::test
