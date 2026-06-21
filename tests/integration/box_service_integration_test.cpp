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
#include <fmgr/v1/box.grpc.pb.h>
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
             ("fmgr-box-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Three principals across two labs:
    //   - admin   : SystemAdmin in lab1 (holds FreezerConfigure/BoxConfigure/SampleRead)
    //   - member  : Member in lab1 (holds SampleRead but NOT Freezer/BoxConfigure)
    //   - outsider: SystemAdmin in lab2 only (holds nothing for lab1)
    //
    // Freezer/Box-config RPCs use `member` as the negative principal; the
    // SampleRead-gated read RPCs (ListBoxes/GetBox) use `outsider`, which also
    // exercises cross-lab isolation.
    class BoxServiceTest : public ::testing::Test {
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
        box_stub_ = fmgr::v1::BoxService::NewStub(channel_);
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
      std::string create_container(const std::string& token, const std::string& lab,
                                   const std::string& name) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateStorageContainerRequest req;
        req.set_lab_id(lab);
        req.set_kind(fmgr::v1::CONTAINER_KIND_RACK);
        req.set_name(name);
        req.set_label(name);
        req.set_ordering_index(0);
        fmgr::v1::CreateStorageContainerResponse resp;
        EXPECT_TRUE(box_stub_->CreateStorageContainer(&ctx, req, &resp).ok());
        return resp.container().id();
      }

      // A box-type position's `accepts` whitelist must reference a live
      // ContainerType.size_class in the same lab, so seed one first.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      void create_container_type(const std::string& token, const std::string& lab,
                                 const std::string& size_class) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateContainerTypeRequest req;
        req.set_lab_id(lab);
        req.set_name(size_class + " vessel");
        req.set_size_class(size_class);
        req.set_material("polypropylene");
        req.set_supplier_sku("SKU-" + size_class);
        fmgr::v1::CreateContainerTypeResponse resp;
        EXPECT_TRUE(box_stub_->CreateContainerType(&ctx, req, &resp).ok());
      }

      std::string create_box_type(const std::string& token, const std::string& lab) {
        create_container_type(token, lab, "9x9");
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateBoxTypeRequest req;
        req.set_lab_id(lab);
        req.set_name("9x9 cryobox");
        req.set_manufacturer("Acme");
        req.set_sku("ACM-9X9");
        auto* const slot = req.add_positions();
        slot->set_label("A1");
        slot->set_row(0);
        slot->set_col(0);
        slot->add_accepts("9x9");
        fmgr::v1::CreateBoxTypeResponse resp;
        EXPECT_TRUE(box_stub_->CreateBoxType(&ctx, req, &resp).ok());
        return resp.box_type().id();
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      std::string create_box(const std::string& token, const std::string& lab,
                             const std::string& box_type_id, const std::string& container_id) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateBoxRequest req;
        req.set_lab_id(lab);
        req.set_box_type_id(box_type_id);
        req.set_storage_container_id(container_id);
        req.set_label("Box-1");
        fmgr::v1::CreateBoxResponse resp;
        EXPECT_TRUE(box_stub_->CreateBox(&ctx, req, &resp).ok());
        return resp.box().id();
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
      std::unique_ptr<fmgr::v1::BoxService::Stub> box_stub_;

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
    // Freezer
    // =====================================================================

    TEST_F(BoxServiceTest, CreateFreezerAsAdminSucceedsAndCreatesLayoutRoot) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateFreezerRequest req;
      req.set_lab_id(kLab1);
      req.set_name("ULT-80");
      req.set_location("Room 3");
      req.set_model("Thermo");
      req.set_temp_target_c(-80.0);
      fmgr::v1::CreateFreezerResponse resp;
      const auto status = box_stub_->CreateFreezer(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.freezer().name(), "ULT-80");
      EXPECT_FALSE(resp.freezer().id().empty());
      EXPECT_FALSE(resp.freezer().layout_root_id().empty());
    }

    TEST_F(BoxServiceTest, CreateFreezerRejectsMemberWithoutFreezerConfigure) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateFreezerRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Sneaky");
      fmgr::v1::CreateFreezerResponse resp;
      const auto status = box_stub_->CreateFreezer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, CreateFreezerWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::CreateFreezerRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Anon");
      fmgr::v1::CreateFreezerResponse resp;
      const auto status = box_stub_->CreateFreezer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(BoxServiceTest, GetFreezerReturnsCreatedForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      std::string freezer_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
        freezer_id = resp.freezer().id();
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetFreezerRequest req;
      req.set_freezer_id(freezer_id);
      fmgr::v1::GetFreezerResponse resp;
      ASSERT_TRUE(box_stub_->GetFreezer(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.freezer().name(), "ULT-80");
    }

    TEST_F(BoxServiceTest, GetFreezerRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string freezer_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
        freezer_id = resp.freezer().id();
      }
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::GetFreezerRequest req;
      req.set_freezer_id(freezer_id);
      fmgr::v1::GetFreezerResponse resp;
      const auto status = box_stub_->GetFreezer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, ListFreezersReturnsCreatedForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListFreezersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListFreezersResponse resp;
      ASSERT_TRUE(box_stub_->ListFreezers(&ctx, req, &resp).ok());
      EXPECT_GE(resp.freezers_size(), 1);
    }

    TEST_F(BoxServiceTest, ListFreezersRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListFreezersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListFreezersResponse resp;
      const auto status = box_stub_->ListFreezers(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, UpdateFreezerChangesNameForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      fmgr::v1::Freezer created;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
        created = resp.freezer();
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateFreezerRequest req;
      *req.mutable_freezer() = created;
      req.mutable_freezer()->set_name("Renamed");
      fmgr::v1::UpdateFreezerResponse resp;
      ASSERT_TRUE(box_stub_->UpdateFreezer(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.freezer().name(), "Renamed");
    }

    TEST_F(BoxServiceTest, UpdateFreezerRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      fmgr::v1::Freezer created;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
        created = resp.freezer();
      }
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::UpdateFreezerRequest req;
      *req.mutable_freezer() = created;
      req.mutable_freezer()->set_name("Hijack");
      fmgr::v1::UpdateFreezerResponse resp;
      const auto status = box_stub_->UpdateFreezer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, ArchiveFreezerHidesFromListForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      std::string freezer_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
        freezer_id = resp.freezer().id();
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ArchiveFreezerRequest req;
        req.set_freezer_id(freezer_id);
        fmgr::v1::ArchiveFreezerResponse resp;
        ASSERT_TRUE(box_stub_->ArchiveFreezer(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetFreezerRequest req;
      req.set_freezer_id(freezer_id);
      fmgr::v1::GetFreezerResponse resp;
      const auto status = box_stub_->GetFreezer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    }

    TEST_F(BoxServiceTest, ArchiveFreezerRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string freezer_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
        freezer_id = resp.freezer().id();
      }
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ArchiveFreezerRequest req;
      req.set_freezer_id(freezer_id);
      fmgr::v1::ArchiveFreezerResponse resp;
      const auto status = box_stub_->ArchiveFreezer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // StorageContainer
    // =====================================================================

    TEST_F(BoxServiceTest, CreateStorageContainerAsAdminRoundTripsKind) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateStorageContainerRequest req;
      req.set_lab_id(kLab1);
      req.set_kind(fmgr::v1::CONTAINER_KIND_RACK);
      req.set_name("Rack A");
      req.set_label("A");
      req.set_ordering_index(2);
      fmgr::v1::CreateStorageContainerResponse resp;
      ASSERT_TRUE(box_stub_->CreateStorageContainer(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.container().kind(), fmgr::v1::CONTAINER_KIND_RACK);
      EXPECT_EQ(resp.container().ordering_index(), 2);
    }

    TEST_F(BoxServiceTest, CreateStorageContainerRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateStorageContainerRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Rack A");
      fmgr::v1::CreateStorageContainerResponse resp;
      const auto status = box_stub_->CreateStorageContainer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, ListStorageContainersReturnsForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      create_container(token, kLab1, "Rack A");
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListStorageContainersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListStorageContainersResponse resp;
      ASSERT_TRUE(box_stub_->ListStorageContainers(&ctx, req, &resp).ok());
      EXPECT_GE(resp.containers_size(), 1);
    }

    TEST_F(BoxServiceTest, ListStorageContainersRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListStorageContainersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListStorageContainersResponse resp;
      const auto status = box_stub_->ListStorageContainers(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, UpdateStorageContainerChangesNameForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      fmgr::v1::StorageContainer created;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateStorageContainerRequest req;
        req.set_lab_id(kLab1);
        req.set_kind(fmgr::v1::CONTAINER_KIND_RACK);
        req.set_name("Rack A");
        fmgr::v1::CreateStorageContainerResponse resp;
        ASSERT_TRUE(box_stub_->CreateStorageContainer(&ctx, req, &resp).ok());
        created = resp.container();
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateStorageContainerRequest req;
      *req.mutable_container() = created;
      req.mutable_container()->set_name("Rack B");
      fmgr::v1::UpdateStorageContainerResponse resp;
      ASSERT_TRUE(box_stub_->UpdateStorageContainer(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.container().name(), "Rack B");
    }

    TEST_F(BoxServiceTest, UpdateStorageContainerRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      fmgr::v1::StorageContainer created;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::CreateStorageContainerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("Rack A");
        fmgr::v1::CreateStorageContainerResponse resp;
        ASSERT_TRUE(box_stub_->CreateStorageContainer(&ctx, req, &resp).ok());
        created = resp.container();
      }
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::UpdateStorageContainerRequest req;
      *req.mutable_container() = created;
      req.mutable_container()->set_name("Hijack");
      fmgr::v1::UpdateStorageContainerResponse resp;
      const auto status = box_stub_->UpdateStorageContainer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, ArchiveStorageContainerForAdminThenListExcludesIt) {
      const auto token = login(kAdminEmail, kPassword);
      const auto container_id = create_container(token, kLab1, "Rack A");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ArchiveStorageContainerRequest req;
        req.set_container_id(container_id);
        fmgr::v1::ArchiveStorageContainerResponse resp;
        ASSERT_TRUE(box_stub_->ArchiveStorageContainer(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListStorageContainersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListStorageContainersResponse resp;
      ASSERT_TRUE(box_stub_->ListStorageContainers(&ctx, req, &resp).ok());
      for (const auto& container : resp.containers()) {
        EXPECT_NE(container.id(), container_id);
      }
    }

    TEST_F(BoxServiceTest, ArchiveStorageContainerRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto container_id = create_container(admin, kLab1, "Rack A");
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ArchiveStorageContainerRequest req;
      req.set_container_id(container_id);
      fmgr::v1::ArchiveStorageContainerResponse resp;
      const auto status = box_stub_->ArchiveStorageContainer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // ContainerType
    // =====================================================================

    TEST_F(BoxServiceTest, CreateContainerTypeAsAdminSucceeds) {
      const auto token = login(kAdminEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateContainerTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Cryo");
      req.set_size_class("9x9");
      req.set_material("polypropylene");
      req.set_supplier_sku("SKU-1");
      fmgr::v1::CreateContainerTypeResponse resp;
      ASSERT_TRUE(box_stub_->CreateContainerType(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.container_type().name(), "Cryo");
    }

    TEST_F(BoxServiceTest, CreateContainerTypeRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateContainerTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Cryo");
      fmgr::v1::CreateContainerTypeResponse resp;
      const auto status = box_stub_->CreateContainerType(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, ListContainerTypesReturnsForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateContainerTypeRequest req;
        req.set_lab_id(kLab1);
        req.set_name("Cryo");
        req.set_size_class("9x9");
        fmgr::v1::CreateContainerTypeResponse resp;
        ASSERT_TRUE(box_stub_->CreateContainerType(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListContainerTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListContainerTypesResponse resp;
      ASSERT_TRUE(box_stub_->ListContainerTypes(&ctx, req, &resp).ok());
      EXPECT_GE(resp.container_types_size(), 1);
    }

    TEST_F(BoxServiceTest, ListContainerTypesRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListContainerTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListContainerTypesResponse resp;
      const auto status = box_stub_->ListContainerTypes(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // BoxType
    // =====================================================================

    TEST_F(BoxServiceTest, CreateBoxTypeAsAdminPersistsPositions) {
      const auto token = login(kAdminEmail, kPassword);
      create_container_type(token, kLab1, "dna"); // accepts whitelist references this
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateBoxTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_name("9x9");
      req.set_manufacturer("Acme");
      req.set_sku("ACM-9X9");
      auto* const slot = req.add_positions();
      slot->set_label("A1");
      slot->set_row(0);
      slot->set_col(0);
      slot->add_accepts("dna");
      fmgr::v1::CreateBoxTypeResponse resp;
      ASSERT_TRUE(box_stub_->CreateBoxType(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.box_type().positions_size(), 1);
      EXPECT_EQ(resp.box_type().positions(0).label(), "A1");
      ASSERT_EQ(resp.box_type().positions(0).accepts_size(), 1);
      EXPECT_EQ(resp.box_type().positions(0).accepts(0), "dna");
    }

    TEST_F(BoxServiceTest, CreateBoxTypeRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateBoxTypeRequest req;
      req.set_lab_id(kLab1);
      req.set_name("9x9");
      fmgr::v1::CreateBoxTypeResponse resp;
      const auto status = box_stub_->CreateBoxType(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, ListBoxTypesReturnsForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      create_box_type(token, kLab1);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListBoxTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListBoxTypesResponse resp;
      ASSERT_TRUE(box_stub_->ListBoxTypes(&ctx, req, &resp).ok());
      EXPECT_GE(resp.box_types_size(), 1);
    }

    TEST_F(BoxServiceTest, ListBoxTypesRejectsMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListBoxTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListBoxTypesResponse resp;
      const auto status = box_stub_->ListBoxTypes(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // Box
    // =====================================================================

    TEST_F(BoxServiceTest, CreateBoxAsAdminSucceeds) {
      const auto token = login(kAdminEmail, kPassword);
      const auto container_id = create_container(token, kLab1, "Rack A");
      const auto box_type_id = create_box_type(token, kLab1);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateBoxRequest req;
      req.set_lab_id(kLab1);
      req.set_box_type_id(box_type_id);
      req.set_storage_container_id(container_id);
      req.set_label("Box-1");
      req.set_serial("SN-1");
      fmgr::v1::CreateBoxResponse resp;
      const auto status = box_stub_->CreateBox(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.box().label(), "Box-1");
      EXPECT_EQ(resp.box().serial(), "SN-1");
    }

    TEST_F(BoxServiceTest, CreateBoxRejectsMemberWithoutBoxConfigure) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto container_id = create_container(admin, kLab1, "Rack A");
      const auto box_type_id = create_box_type(admin, kLab1);
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::CreateBoxRequest req;
      req.set_lab_id(kLab1);
      req.set_box_type_id(box_type_id);
      req.set_storage_container_id(container_id);
      req.set_label("Box-1");
      fmgr::v1::CreateBoxResponse resp;
      const auto status = box_stub_->CreateBox(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, GetBoxReturnsForMemberWithSampleRead) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto container_id = create_container(admin, kLab1, "Rack A");
      const auto box_type_id = create_box_type(admin, kLab1);
      const auto box_id = create_box(admin, kLab1, box_type_id, container_id);
      // A plain Member holds sample.read, which gates GetBox — so this succeeds.
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::GetBoxRequest req;
      req.set_box_id(box_id);
      fmgr::v1::GetBoxResponse resp;
      ASSERT_TRUE(box_stub_->GetBox(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.box().id(), box_id);
    }

    TEST_F(BoxServiceTest, GetBoxRejectsOutsiderWithoutSampleRead) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto container_id = create_container(admin, kLab1, "Rack A");
      const auto box_type_id = create_box_type(admin, kLab1);
      const auto box_id = create_box(admin, kLab1, box_type_id, container_id);
      // The outsider has no membership in lab1, so lacks sample.read there.
      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
      fmgr::v1::GetBoxRequest req;
      req.set_box_id(box_id);
      fmgr::v1::GetBoxResponse resp;
      const auto status = box_stub_->GetBox(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, ListBoxesReturnsForMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto container_id = create_container(admin, kLab1, "Rack A");
      const auto box_type_id = create_box_type(admin, kLab1);
      create_box(admin, kLab1, box_type_id, container_id);
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ListBoxesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListBoxesResponse resp;
      ASSERT_TRUE(box_stub_->ListBoxes(&ctx, req, &resp).ok());
      EXPECT_GE(resp.boxes_size(), 1);
    }

    TEST_F(BoxServiceTest, ListBoxesRejectsOutsider) {
      const auto token = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListBoxesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListBoxesResponse resp;
      const auto status = box_stub_->ListBoxes(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, UpdateBoxChangesLabelForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      const auto container_id = create_container(token, kLab1, "Rack A");
      const auto box_type_id = create_box_type(token, kLab1);
      fmgr::v1::Box created;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateBoxRequest req;
        req.set_lab_id(kLab1);
        req.set_box_type_id(box_type_id);
        req.set_storage_container_id(container_id);
        req.set_label("Box-1");
        fmgr::v1::CreateBoxResponse resp;
        ASSERT_TRUE(box_stub_->CreateBox(&ctx, req, &resp).ok());
        created = resp.box();
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateBoxRequest req;
      *req.mutable_box() = created;
      req.mutable_box()->set_label("Box-2");
      fmgr::v1::UpdateBoxResponse resp;
      ASSERT_TRUE(box_stub_->UpdateBox(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.box().label(), "Box-2");
    }

    TEST_F(BoxServiceTest, UpdateBoxRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto container_id = create_container(admin, kLab1, "Rack A");
      const auto box_type_id = create_box_type(admin, kLab1);
      fmgr::v1::Box created;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::CreateBoxRequest req;
        req.set_lab_id(kLab1);
        req.set_box_type_id(box_type_id);
        req.set_storage_container_id(container_id);
        req.set_label("Box-1");
        fmgr::v1::CreateBoxResponse resp;
        ASSERT_TRUE(box_stub_->CreateBox(&ctx, req, &resp).ok());
        created = resp.box();
      }
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::UpdateBoxRequest req;
      *req.mutable_box() = created;
      req.mutable_box()->set_label("Hijack");
      fmgr::v1::UpdateBoxResponse resp;
      const auto status = box_stub_->UpdateBox(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, ArchiveBoxHidesFromGetForAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      const auto container_id = create_container(token, kLab1, "Rack A");
      const auto box_type_id = create_box_type(token, kLab1);
      const auto box_id = create_box(token, kLab1, box_type_id, container_id);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ArchiveBoxRequest req;
        req.set_box_id(box_id);
        fmgr::v1::ArchiveBoxResponse resp;
        ASSERT_TRUE(box_stub_->ArchiveBox(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetBoxRequest req;
      req.set_box_id(box_id);
      fmgr::v1::GetBoxResponse resp;
      const auto status = box_stub_->GetBox(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    }

    TEST_F(BoxServiceTest, ArchiveBoxRejectsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto container_id = create_container(admin, kLab1, "Rack A");
      const auto box_type_id = create_box_type(admin, kLab1);
      const auto box_id = create_box(admin, kLab1, box_type_id, container_id);
      const auto member = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ArchiveBoxRequest req;
      req.set_box_id(box_id);
      fmgr::v1::ArchiveBoxResponse resp;
      const auto status = box_stub_->ArchiveBox(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // Cross-lab isolation
    // =====================================================================

    TEST_F(BoxServiceTest, OutsiderCannotCreateFreezerInForeignLab) {
      const auto token = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateFreezerRequest req;
      req.set_lab_id(kLab1); // outsider only belongs to lab2
      req.set_name("Trespass");
      fmgr::v1::CreateFreezerResponse resp;
      const auto status = box_stub_->CreateFreezer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxServiceTest, OutsiderCannotArchiveForeignLabFreezer) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string freezer_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
        freezer_id = resp.freezer().id();
      }
      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
      fmgr::v1::ArchiveFreezerRequest req;
      req.set_freezer_id(freezer_id);
      fmgr::v1::ArchiveFreezerResponse resp;
      const auto status = box_stub_->ArchiveFreezer(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

  } // namespace
} // namespace fmgr::test
