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
#include <fmgr/v1/item_type.grpc.pb.h>
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
             ("fmgr-item-type-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Three principals across two labs:
    //   - admin   : SystemAdmin in lab1 (holds ItemTypeDefine + CustomFieldDefine)
    //   - member  : Member in lab1 (holds neither define permission)
    //   - outsider: SystemAdmin in lab2 only (holds nothing for lab1)
    //
    // ItemType/CFD define RPCs use `member` as the negative authz principal;
    // `outsider` exercises cross-lab isolation.
    class ItemTypeServiceTest : public ::testing::Test {
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
        item_type_stub_ = fmgr::v1::ItemTypeService::NewStub(channel_);
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

      // ---- entity creation helpers (as the lab-1 admin) ----

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      std::string create_item_type(const std::string& token, const std::string& lab,
                                   const std::string& name) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateItemTypeRequest req;
        req.set_lab_id(lab);
        req.set_name(name);
        fmgr::v1::CreateItemTypeResponse resp;
        EXPECT_TRUE(item_type_stub_->CreateItemType(&ctx, req, &resp).ok());
        return resp.item_type().id();
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      std::string create_cfd(const std::string& token, const std::string& lab,
                             const std::string& key) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateCfdRequest req;
        auto* const cfd = req.mutable_cfd();
        cfd->set_lab_id(lab);
        cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
        cfd->set_key(key);
        cfd->set_label(key + " label");
        cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
        fmgr::v1::CreateCfdResponse resp;
        EXPECT_TRUE(item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp).ok());
        return resp.cfd().id();
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
      std::unique_ptr<fmgr::v1::ItemTypeService::Stub> item_type_stub_;

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
    // ItemType
    // =====================================================================

    TEST_F(ItemTypeServiceTest, CreateItemTypeAsAdminSucceeds) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateItemTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_name("liquid");
      fmgr::v1::CreateItemTypeResponse resp;
      const auto status = item_type_stub_->CreateItemType(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.item_type().name(), "liquid");
      EXPECT_FALSE(resp.item_type().id().empty());
      EXPECT_FALSE(resp.item_type().has_parent_id());
    }

    TEST_F(ItemTypeServiceTest, CreateChildItemTypeRetainsParent) {
      const auto token = login(kAdminEmail, kPassword);
      const auto parent_id = create_item_type(token, kLab1, "liquid");

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateItemTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_parent_id(parent_id);
      req.set_name("blood");
      fmgr::v1::CreateItemTypeResponse resp;
      ASSERT_TRUE(item_type_stub_->CreateItemType(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.item_type().parent_id(), parent_id);
    }

    TEST_F(ItemTypeServiceTest, CreateItemTypeRejectsMemberWithoutItemTypeDefine) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateItemTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Sneaky");
      fmgr::v1::CreateItemTypeResponse resp;
      const auto status = item_type_stub_->CreateItemType(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ItemTypeServiceTest, CreateItemTypeWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::CreateItemTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Anon");
      fmgr::v1::CreateItemTypeResponse resp;
      const auto status = item_type_stub_->CreateItemType(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(ItemTypeServiceTest, CreateItemTypeRejectsEmptyName) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateItemTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_name("");
      fmgr::v1::CreateItemTypeResponse resp;
      const auto status = item_type_stub_->CreateItemType(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(ItemTypeServiceTest, ListItemTypesReturnsLabRows) {
      const auto token = login(kAdminEmail, kPassword);
      create_item_type(token, kLab1, "liquid");
      create_item_type(token, kLab1, "solid");

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListItemTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListItemTypesResponse resp;
      ASSERT_TRUE(item_type_stub_->ListItemTypes(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.item_types_size(), 2);
    }

    TEST_F(ItemTypeServiceTest, ListItemTypesRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListItemTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListItemTypesResponse resp;
      const auto status = item_type_stub_->ListItemTypes(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ItemTypeServiceTest, GetItemTypeAsAdminSucceeds) {
      const auto token = login(kAdminEmail, kPassword);
      const auto id = create_item_type(token, kLab1, "liquid");

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetItemTypeRequest req;
      req.set_item_type_id(id);
      fmgr::v1::GetItemTypeResponse resp;
      ASSERT_TRUE(item_type_stub_->GetItemType(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.item_type().id(), id);
      EXPECT_EQ(resp.item_type().name(), "liquid");
    }

    TEST_F(ItemTypeServiceTest, GetItemTypeRejectsOutsiderCrossLab) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto id = create_item_type(admin, kLab1, "liquid");

      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
      fmgr::v1::GetItemTypeRequest req;
      req.set_item_type_id(id);
      fmgr::v1::GetItemTypeResponse resp;
      const auto status = item_type_stub_->GetItemType(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ItemTypeServiceTest, UpdateItemTypeRenames) {
      const auto token = login(kAdminEmail, kPassword);
      const auto id = create_item_type(token, kLab1, "liquid");

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateItemTypeRequest req;
      auto* const it = req.mutable_item_type();
      it->set_id(id);
      it->set_lab_id(kLab1);
      it->set_name("liquid-renamed");
      fmgr::v1::UpdateItemTypeResponse resp;
      ASSERT_TRUE(item_type_stub_->UpdateItemType(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.item_type().name(), "liquid-renamed");
    }

    TEST_F(ItemTypeServiceTest, UpdateItemTypeRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto id = create_item_type(admin, kLab1, "liquid");

      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::UpdateItemTypeRequest req;
      auto* const it = req.mutable_item_type();
      it->set_id(id);
      it->set_lab_id(kLab1);
      it->set_name("hijack");
      fmgr::v1::UpdateItemTypeResponse resp;
      const auto status = item_type_stub_->UpdateItemType(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ItemTypeServiceTest, ArchiveItemTypeHidesFromGetAndList) {
      const auto token = login(kAdminEmail, kPassword);
      const auto id = create_item_type(token, kLab1, "liquid");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ArchiveItemTypeRequest req;
        req.set_item_type_id(id);
        fmgr::v1::ArchiveItemTypeResponse resp;
        ASSERT_TRUE(item_type_stub_->ArchiveItemType(&ctx, req, &resp).ok());
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GetItemTypeRequest req;
        req.set_item_type_id(id);
        fmgr::v1::GetItemTypeResponse resp;
        const auto status = item_type_stub_->GetItemType(&ctx, req, &resp);
        EXPECT_FALSE(status.ok());
        EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListItemTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListItemTypesResponse resp;
      ASSERT_TRUE(item_type_stub_->ListItemTypes(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.item_types_size(), 0);
    }

    TEST_F(ItemTypeServiceTest, ArchiveItemTypeRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto id = create_item_type(admin, kLab1, "liquid");

      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ArchiveItemTypeRequest req;
      req.set_item_type_id(id);
      fmgr::v1::ArchiveItemTypeResponse resp;
      const auto status = item_type_stub_->ArchiveItemType(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // CustomFieldDefinition
    // =====================================================================

    TEST_F(ItemTypeServiceTest, CreateCfdAsAdminSucceeds) {
      const auto token = login(kAdminEmail, kPassword);

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_key("patient_id");
      cfd->set_label("Patient ID");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
      fmgr::v1::CreateCfdResponse resp;
      const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.cfd().key(), "patient_id");
      EXPECT_EQ(resp.cfd().data_type(), fmgr::v1::FIELD_DATA_TYPE_TEXT);
      EXPECT_EQ(resp.cfd().scope_kind(), fmgr::v1::SCOPE_KIND_SAMPLE);
      EXPECT_FALSE(resp.cfd().id().empty());
    }

    TEST_F(ItemTypeServiceTest, CreateCfdPreservesIntDataType) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_BOX);
      cfd->set_key("count");
      cfd->set_label("Count");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_INT);
      fmgr::v1::CreateCfdResponse resp;
      ASSERT_TRUE(item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp).ok());
      // Lossless round-trip: INT must not collapse to FLOAT.
      EXPECT_EQ(resp.cfd().data_type(), fmgr::v1::FIELD_DATA_TYPE_INT);
    }

    TEST_F(ItemTypeServiceTest, CreateIndexedPhiCfdRejected) {
      // A PHI field must never be indexed (PRD §4.1): indexing would leak plaintext
      // PHI into the index. The combination is rejected at definition time.
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_key("mrn");
      cfd->set_label("MRN");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
      cfd->set_is_phi(true);
      cfd->set_indexed(true);
      fmgr::v1::CreateCfdResponse resp;
      const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(ItemTypeServiceTest, CreateCfdRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_key("k");
      cfd->set_label("L");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
      fmgr::v1::CreateCfdResponse resp;
      const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ItemTypeServiceTest, CreateCfdRejectsEmptyKey) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_key("");
      cfd->set_label("Label");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
      fmgr::v1::CreateCfdResponse resp;
      const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(ItemTypeServiceTest, CreateCfdRejectsUnspecifiedDataType) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_key("k");
      cfd->set_label("L");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_UNSPECIFIED);
      fmgr::v1::CreateCfdResponse resp;
      const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(ItemTypeServiceTest, CreateCfdRejectsPhiIndexed) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_key("ssn");
      cfd->set_label("SSN");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
      cfd->set_is_phi(true);
      cfd->set_indexed(true);
      fmgr::v1::CreateCfdResponse resp;
      const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(ItemTypeServiceTest, CreateCfdScopedToItemTypeSucceeds) {
      const auto token = login(kAdminEmail, kPassword);
      const auto item_type_id = create_item_type(token, kLab1, "blood");

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_item_type_id(item_type_id);
      cfd->set_key("hematocrit");
      cfd->set_label("Hematocrit");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_FLOAT);
      fmgr::v1::CreateCfdResponse resp;
      ASSERT_TRUE(item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.cfd().item_type_id(), item_type_id);
    }

    TEST_F(ItemTypeServiceTest, CreateCfdRejectsForeignItemType) {
      const auto admin = login(kAdminEmail, kPassword);
      // An item type that does not exist in lab1.
      const std::string bogus_item_type = "30000000-0000-0000-0000-0000000000ff";

      grpc::ClientContext ctx;
      set_bearer(ctx, admin);
      fmgr::v1::CreateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_item_type_id(bogus_item_type);
      cfd->set_key("k");
      cfd->set_label("L");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
      fmgr::v1::CreateCfdResponse resp;
      const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(ItemTypeServiceTest, ListCfdsFiltersByItemType) {
      const auto token = login(kAdminEmail, kPassword);
      const auto item_type_id = create_item_type(token, kLab1, "blood");
      // One global CFD, one scoped to the item type.
      create_cfd(token, kLab1, "global_key");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateCfdRequest req;
        auto* const cfd = req.mutable_cfd();
        cfd->set_lab_id(kLab1);
        cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
        cfd->set_item_type_id(item_type_id);
        cfd->set_key("scoped_key");
        cfd->set_label("Scoped");
        cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
        fmgr::v1::CreateCfdResponse resp;
        ASSERT_TRUE(item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp).ok());
      }

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListCfdsRequest req;
      req.set_lab_id(kLab1);
      req.set_item_type_id(item_type_id);
      fmgr::v1::ListCfdsResponse resp;
      ASSERT_TRUE(item_type_stub_->ListCustomFieldDefinitions(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.cfds_size(), 1);
      EXPECT_EQ(resp.cfds(0).key(), "scoped_key");
    }

    TEST_F(ItemTypeServiceTest, ListCfdsRejectsOutsiderCrossLab) {
      const auto admin = login(kAdminEmail, kPassword);
      create_cfd(admin, kLab1, "k1");

      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
      fmgr::v1::ListCfdsRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListCfdsResponse resp;
      const auto status = item_type_stub_->ListCustomFieldDefinitions(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ItemTypeServiceTest, UpdateCfdChangesLabel) {
      const auto token = login(kAdminEmail, kPassword);
      const auto id = create_cfd(token, kLab1, "patient_id");

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_id(id);
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_key("patient_id");
      cfd->set_label("Updated Label");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
      fmgr::v1::UpdateCfdResponse resp;
      ASSERT_TRUE(item_type_stub_->UpdateCustomFieldDefinition(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.cfd().label(), "Updated Label");
    }

    TEST_F(ItemTypeServiceTest, UpdateCfdRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto id = create_cfd(admin, kLab1, "patient_id");

      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::UpdateCfdRequest req;
      auto* const cfd = req.mutable_cfd();
      cfd->set_id(id);
      cfd->set_lab_id(kLab1);
      cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
      cfd->set_key("patient_id");
      cfd->set_label("hijack");
      cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
      fmgr::v1::UpdateCfdResponse resp;
      const auto status = item_type_stub_->UpdateCustomFieldDefinition(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(ItemTypeServiceTest, ArchiveCfdHidesFromList) {
      const auto token = login(kAdminEmail, kPassword);
      const auto id = create_cfd(token, kLab1, "patient_id");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ArchiveCfdRequest req;
        req.set_cfd_id(id);
        fmgr::v1::ArchiveCfdResponse resp;
        ASSERT_TRUE(item_type_stub_->ArchiveCustomFieldDefinition(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListCfdsRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListCfdsResponse resp;
      ASSERT_TRUE(item_type_stub_->ListCustomFieldDefinitions(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.cfds_size(), 0);
    }

    TEST_F(ItemTypeServiceTest, ArchiveCfdRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto id = create_cfd(admin, kLab1, "patient_id");

      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ArchiveCfdRequest req;
      req.set_cfd_id(id);
      fmgr::v1::ArchiveCfdResponse resp;
      const auto status = item_type_stub_->ArchiveCustomFieldDefinition(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

  } // namespace
} // namespace fmgr::test
