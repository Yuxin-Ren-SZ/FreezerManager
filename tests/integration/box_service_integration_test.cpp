// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/role.h"
#include "core/sample.h"
#include "core/uuid.h"
#include "server/FreezerServer.h"
#include "storage/AuditTraits.h"
#include "storage/IdentityTraits.h"
#include "storage/ItemTypeTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SampleTraits.h"
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

#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
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

    // Four principals across two labs:
    //   - admin   : SystemAdmin in lab1 (holds FreezerConfigure/BoxConfigure/SampleRead)
    //   - member  : Member in lab1 (holds SampleRead but NOT Freezer/BoxConfigure)
    //   - readonly: ReadOnly in lab1 (holds SampleRead + AuditRead only)
    //   - outsider: SystemAdmin in lab2 only (holds nothing for lab1)
    //
    // Mutating Freezer/Box-config RPCs use `member` as the negative principal;
    // the SampleRead-gated read RPCs (ListBoxes/GetBox) use `outsider`, which
    // also exercises cross-lab isolation.
    //
    // The layout read RPCs (ListFreezers/ListStorageContainers/ListBoxTypes) are
    // gated on SampleRead so a Member can interpret a sample's location (#54);
    // `readonly` asserts that this reaches the read-only role too, and
    // `ListContainerTypesStillRejectsMember` pins the boundary deliberately NOT
    // relaxed.
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
      const std::string kReadOnlyEmail{"readonly@example.com"};
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
        const core::UserId readonly_id =
            core::UserId::parse("10000000-0000-0000-0000-000000000004");
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
        txn->repo<core::User>().insert(make_user(readonly_id, kReadOnlyEmail), ctx);
        txn->repo<core::User>().insert(make_user(outsider_id, kOutsiderEmail), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(admin_id, lab1, core::RoleKind::SystemAdmin), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(member_id, lab1, core::RoleKind::Member), ctx);
        txn->repo<core::LabMembership>().insert(
            make_membership(readonly_id, lab1, core::RoleKind::ReadOnly), ctx);
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

    // #54: the layout read RPCs are gated on sample.read, the permission the
    // REST route and the SPA `layout` route already declare, so a Member can
    // interpret a sample's location. This member holds no configure permission.
    TEST_F(BoxServiceTest, ListFreezersAllowsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
      }
      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ListFreezersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListFreezersResponse resp;
      const auto status = box_stub_->ListFreezers(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_GE(resp.freezers_size(), 1);
    }

    TEST_F(BoxServiceTest, ListFreezersAllowsReadOnly) {
      const auto admin = login(kAdminEmail, kPassword);
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name("ULT-80");
        fmgr::v1::CreateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->CreateFreezer(&ctx, req, &resp).ok());
      }
      const auto readonly_user = login(kReadOnlyEmail, kPassword);
      ASSERT_FALSE(readonly_user.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, readonly_user);
      fmgr::v1::ListFreezersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListFreezersResponse resp;
      const auto status = box_stub_->ListFreezers(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_GE(resp.freezers_size(), 1);
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

    TEST_F(BoxServiceTest, ListStorageContainersAllowsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      create_container(admin, kLab1, "Rack A");
      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ListStorageContainersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListStorageContainersResponse resp;
      const auto status = box_stub_->ListStorageContainers(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_GE(resp.containers_size(), 1);
    }

    TEST_F(BoxServiceTest, ListStorageContainersAllowsReadOnly) {
      const auto admin = login(kAdminEmail, kPassword);
      create_container(admin, kLab1, "Rack A");
      const auto readonly_user = login(kReadOnlyEmail, kPassword);
      ASSERT_FALSE(readonly_user.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, readonly_user);
      fmgr::v1::ListStorageContainersRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListStorageContainersResponse resp;
      const auto status = box_stub_->ListStorageContainers(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_GE(resp.containers_size(), 1);
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

    TEST_F(BoxServiceTest, ListContainerTypesToleratesMalformedOuterDimensionsJson) {
      const auto token = login(kAdminEmail, kPassword);
      std::string planted_id;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateContainerTypeRequest req;
        req.set_lab_id(kLab1);
        req.set_name("Cryo");
        req.set_size_class("9x9");
        fmgr::v1::CreateContainerTypeResponse resp;
        ASSERT_TRUE(box_stub_->CreateContainerType(&ctx, req, &resp).ok());
        planted_id = resp.container_type().id();
      }

      // Plant the state the API cannot produce: the demo seed wrote `{}` where
      // the write path stores the text `null` (dimensions_dump). The plant is
      // the point of this test — without it the defect only shows on rows
      // created outside the API, which is why CI never saw it.
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        auto& sqlite = dynamic_cast<storage::SqliteTransaction&>(*txn);
        char* error = nullptr;
        const std::string sql =
            "UPDATE container_types SET outer_dimensions_json = '{}' WHERE id = '" + planted_id +
            "'";
        ASSERT_EQ(sqlite3_exec(sqlite.handle(), sql.c_str(), nullptr, nullptr, &error), SQLITE_OK)
            << (error == nullptr ? "" : error);
        sqlite3_free(error);
        txn->commit();
      }

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListContainerTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListContainerTypesResponse resp;
      const auto status = box_stub_->ListContainerTypes(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();

      bool found = false;
      for (const auto& type : resp.container_types()) {
        if (type.id() != planted_id) {
          continue;
        }
        found = true;
        EXPECT_EQ(type.name(), "Cryo");
        // "Absent" on the wire is the empty proto3 string field — the same shape
        // a row created by the API with no dimensions reads back with.
        EXPECT_EQ(type.outer_dimensions_json(), "");
      }
      EXPECT_TRUE(found);
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

    TEST_F(BoxServiceTest, ListBoxTypesAllowsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      create_box_type(admin, kLab1);
      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ListBoxTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListBoxTypesResponse resp;
      const auto status = box_stub_->ListBoxTypes(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_GE(resp.box_types_size(), 1);
    }

    TEST_F(BoxServiceTest, ListBoxTypesAllowsReadOnly) {
      const auto admin = login(kAdminEmail, kPassword);
      create_box_type(admin, kLab1);
      const auto readonly_user = login(kReadOnlyEmail, kPassword);
      ASSERT_FALSE(readonly_user.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, readonly_user);
      fmgr::v1::ListBoxTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListBoxTypesResponse resp;
      const auto status = box_stub_->ListBoxTypes(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_GE(resp.box_types_size(), 1);
    }

    // The boundary this change deliberately does NOT move: container *type*
    // definitions stay behind box.configure, because they are not part of the
    // layout read set the Qt client and the SPA layout screen consume. Pinned so
    // a later "relax all the reads" commit has to argue with a red test.
    TEST_F(BoxServiceTest, ListContainerTypesStillRejectsMember) {
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

    // The acceptance criterion that a positive-only test would miss: relaxing
    // the reads must not relax the writes. A Member reads the layout and is then
    // still refused when archiving a freezer.
    TEST_F(BoxServiceTest, MemberCanReadLayoutButStillCannotArchiveFreezer) {
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
      ASSERT_FALSE(member.empty());
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::ListFreezersRequest req;
        req.set_lab_id(kLab1);
        fmgr::v1::ListFreezersResponse resp;
        EXPECT_TRUE(box_stub_->ListFreezers(&ctx, req, &resp).ok());
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::ArchiveFreezerRequest req;
        req.set_freezer_id(freezer_id);
        fmgr::v1::ArchiveFreezerResponse resp;
        const auto status = box_stub_->ArchiveFreezer(&ctx, req, &resp);
        EXPECT_FALSE(status.ok());
        EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      }
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

    // =====================================================================
    // 3D layout (ADR 0003): catalogue, placements, update semantics, scene
    // =====================================================================

    class BoxLayout3dTest : public BoxServiceTest {
    protected:
      static constexpr const char* kF740 = "eppendorf-cryocube-f740hi";
      static constexpr const char* kTsx = "thermo-tsx40086a";

      struct CreatedFreezer {
        std::string id;
        std::string root_id;
      };

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      CreatedFreezer create_freezer(const std::string& token, const std::string& model_id,
                                    const std::string& name = "ULT") {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateFreezerRequest req;
        req.set_lab_id(kLab1);
        req.set_name(name);
        req.set_freezer_model_id(model_id);
        fmgr::v1::CreateFreezerResponse resp;
        const auto status = box_stub_->CreateFreezer(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << status.error_message();
        return {.id = resp.freezer().id(), .root_id = resp.freezer().layout_root_id()};
      }

      struct ContainerSpec {
        std::string parent_id;
        std::string name;
        std::optional<std::array<int, 3>> placement;
        std::optional<std::array<int, 3>> grid;
      };

      grpc::Status create_placed_container(const std::string& token, const ContainerSpec& spec,
                                           std::string* id_out = nullptr) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateStorageContainerRequest req;
        req.set_lab_id(kLab1);
        req.set_parent_id(spec.parent_id);
        req.set_kind(fmgr::v1::CONTAINER_KIND_RACK);
        req.set_name(spec.name);
        req.set_label(spec.name);
        if (spec.placement.has_value()) {
          set_placement(req.mutable_placement(), *spec.placement);
        }
        if (spec.grid.has_value()) {
          req.mutable_grid()->set_levels((*spec.grid)[0]);
          req.mutable_grid()->set_columns((*spec.grid)[1]);
          req.mutable_grid()->set_depths((*spec.grid)[2]);
        }
        fmgr::v1::CreateStorageContainerResponse resp;
        auto status = box_stub_->CreateStorageContainer(&ctx, req, &resp);
        if (id_out != nullptr) {
          *id_out = resp.container().id();
        }
        return status;
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      grpc::Status create_placed_box(const std::string& token, const std::string& box_type_id,
                                     const std::string& container_id, const std::string& label,
                                     std::optional<std::array<int, 3>> placement,
                                     std::string* id_out = nullptr) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateBoxRequest req;
        req.set_lab_id(kLab1);
        req.set_box_type_id(box_type_id);
        req.set_storage_container_id(container_id);
        req.set_label(label);
        if (placement.has_value()) {
          set_placement(req.mutable_placement(), *placement);
        }
        fmgr::v1::CreateBoxResponse resp;
        auto status = box_stub_->CreateBox(&ctx, req, &resp);
        if (id_out != nullptr) {
          *id_out = resp.box().id();
        }
        return status;
      }

      static void set_placement(fmgr::v1::Placement* out, const std::array<int, 3>& cell) {
        out->set_level(cell[0]);
        out->set_column(cell[1]);
        out->set_depth(cell[2]);
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      fmgr::v1::StorageContainer get_container(const std::string& token, const std::string& id) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListStorageContainersRequest req;
        req.set_lab_id(kLab1);
        fmgr::v1::ListStorageContainersResponse resp;
        EXPECT_TRUE(box_stub_->ListStorageContainers(&ctx, req, &resp).ok());
        for (const auto& container : resp.containers()) {
          if (container.id() == id) {
            return container;
          }
        }
        ADD_FAILURE() << "container " << id << " not listed";
        return {};
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      fmgr::v1::Freezer get_freezer(const std::string& token, const std::string& id) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GetFreezerRequest req;
        req.set_freezer_id(id);
        fmgr::v1::GetFreezerResponse resp;
        EXPECT_TRUE(box_stub_->GetFreezer(&ctx, req, &resp).ok());
        return resp.freezer();
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      grpc::Status get_scene(const std::string& token, const std::string& freezer_id,
                             fmgr::v1::GetFreezerSceneResponse* resp) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GetFreezerSceneRequest req;
        req.set_freezer_id(freezer_id);
        return box_stub_->GetFreezerScene(&ctx, req, resp);
      }

      // Samples are seeded straight through the backend: the scene only counts
      // them, and SampleService's own rules are covered by its own suite.
      void seed_samples_in_box(const std::string& box_id, int count) {
        const storage::MutationContext mctx{
            .actor_user_id = core::UserId::parse("10000000-0000-0000-0000-000000000001"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "scene occupancy",
        };
        const auto lab = core::LabId::parse(kLab1);
        const auto user = core::UserId::parse("10000000-0000-0000-0000-000000000001");
        const core::ItemType item_type{
            .id = core::ItemTypeId::parse(core::generate_uuid_v4()),
            .lab_id = lab,
            .parent_id = std::nullopt,
            .name = "tube-" + box_id.substr(0, 8),
            .created_at = core::Timestamp::from_unix_micros(10),
        };
        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::ItemType>().insert(item_type, mctx);
          txn->commit();
        }
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        for (int index = 0; index < count; ++index) {
          txn->repo<core::Sample>().insert(
              core::Sample{
                  .id = core::SampleId::parse(core::generate_uuid_v4()),
                  .lab_id = lab,
                  .item_type_id = item_type.id,
                  .name = "occupant " + std::to_string(index),
                  .box_id = core::BoxId::parse(box_id),
                  .position_label = "A" + std::to_string(index + 1),
                  .created_by = user,
                  .created_at = core::Timestamp::from_unix_micros(20),
                  .last_modified_by = user,
                  .last_modified_at = core::Timestamp::from_unix_micros(20),
              },
              mctx);
        }
        txn->commit();
      }

      // A box type with `positions` cells A1..An.
      std::string create_box_type_with_positions(const std::string& token, int positions) {
        create_container_type(token, kLab1, "2in");
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateBoxTypeRequest req;
        req.set_lab_id(kLab1);
        req.set_name("2in box");
        req.set_manufacturer("Acme");
        req.set_sku("ACM-2IN");
        for (int index = 0; index < positions; ++index) {
          auto* const slot = req.add_positions();
          slot->set_label("A" + std::to_string(index + 1));
          slot->set_row(0);
          slot->set_col(index);
          slot->add_accepts("2in");
        }
        fmgr::v1::CreateBoxTypeResponse resp;
        EXPECT_TRUE(box_stub_->CreateBoxType(&ctx, req, &resp).ok());
        return resp.box_type().id();
      }
    };

    TEST_F(BoxLayout3dTest, ListFreezerModelsReturnsTheBuiltinCatalogueToAMember) {
      const auto token = login(kMemberEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListFreezerModelsRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListFreezerModelsResponse resp;
      const auto status = box_stub_->ListFreezerModels(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      ASSERT_EQ(resp.models_size(), 2);
      const auto& f740 = resp.models(0);
      EXPECT_EQ(f740.id(), kF740);
      EXPECT_EQ(f740.render_key(), "cryocube-f740");
      EXPECT_EQ(f740.hinge(), fmgr::v1::HINGE_SIDE_RIGHT);
      EXPECT_EQ(f740.form(), fmgr::v1::FREEZER_FORM_UPRIGHT);
      EXPECT_EQ(f740.exterior_mm().height_mm(), 1973);
      ASSERT_EQ(f740.compartments_size(), 3);
      EXPECT_EQ(f740.compartments(1).rack_slots(), 6);
      EXPECT_EQ(f740.compartments(1).rack_slot_mm().width_mm(), 139);
      const auto& tsx = resp.models(1);
      ASSERT_EQ(tsx.inner_doors_size(), 2);
      EXPECT_EQ(tsx.inner_doors(1).first_level(), 2);
      EXPECT_EQ(tsx.inner_doors(1).last_level(), 3);
    }

    TEST_F(BoxLayout3dTest, ListFreezerModelsRejectsAnOutsider) {
      const auto token = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListFreezerModelsRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListFreezerModelsResponse resp;
      EXPECT_EQ(box_stub_->ListFreezerModels(&ctx, req, &resp).error_code(),
                grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(BoxLayout3dTest, CreateFreezerStoresAKnownModelAndRefusesAnUnknownOne) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);
      EXPECT_EQ(get_freezer(token, freezer.id).freezer_model_id(), kF740);

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CreateFreezerRequest req;
      req.set_lab_id(kLab1);
      req.set_name("Mystery");
      req.set_freezer_model_id("no-such-model");
      fmgr::v1::CreateFreezerResponse resp;
      EXPECT_EQ(box_stub_->CreateFreezer(&ctx, req, &resp).error_code(),
                grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(BoxLayout3dTest, UpdateFreezerKeepsTheModelWhenTheFieldIsAbsentAndClearsItOnEmpty) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);

      auto current = get_freezer(token, freezer.id);
      current.clear_freezer_model_id(); // a client that predates the field
      current.set_name("Renamed");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::UpdateFreezerRequest req;
        *req.mutable_freezer() = current;
        fmgr::v1::UpdateFreezerResponse resp;
        ASSERT_TRUE(box_stub_->UpdateFreezer(&ctx, req, &resp).ok());
        EXPECT_EQ(resp.freezer().freezer_model_id(), kF740);
        EXPECT_EQ(resp.freezer().name(), "Renamed");
      }

      current.set_freezer_model_id("");
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateFreezerRequest req;
      *req.mutable_freezer() = current;
      fmgr::v1::UpdateFreezerResponse resp;
      ASSERT_TRUE(box_stub_->UpdateFreezer(&ctx, req, &resp).ok());
      EXPECT_EQ(get_freezer(token, freezer.id).freezer_model_id(), "");
    }

    TEST_F(BoxLayout3dTest, RackPlacementUnderACabinetFollowsTheCompartmentsRackSlots) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);

      EXPECT_TRUE(create_placed_container(token, {.parent_id = freezer.root_id,
                                                  .name = "Rack 6",
                                                  .placement = std::array{2, 5, 0}})
                      .ok());
      // The F740 has 6 rack slots per compartment and 3 compartments.
      EXPECT_EQ(
          create_placed_container(
              token, {.parent_id = freezer.root_id, .name = "R7", .placement = std::array{0, 6, 0}})
              .error_code(),
          grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_EQ(create_placed_container(token, {.parent_id = freezer.root_id,
                                                .name = "R-top",
                                                .placement = std::array{3, 0, 0}})
                    .error_code(),
                grpc::StatusCode::INVALID_ARGUMENT);
      // Racks stand side by side: there is no second rack behind the first.
      EXPECT_EQ(create_placed_container(token, {.parent_id = freezer.root_id,
                                                .name = "R-back",
                                                .placement = std::array{0, 0, 1}})
                    .error_code(),
                grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(BoxLayout3dTest, PlacementUnderAParentWithoutGridIsRefused) {
      const auto token = login(kAdminEmail, kPassword);
      const auto modelless = create_freezer(token, "");
      EXPECT_EQ(create_placed_container(
                    token,
                    {.parent_id = modelless.root_id, .name = "R", .placement = std::array{0, 0, 0}})
                    .error_code(),
                grpc::StatusCode::INVALID_ARGUMENT);

      std::string rack_id;
      ASSERT_TRUE(
          create_placed_container(token, {.parent_id = modelless.root_id, .name = "Rack"}, &rack_id)
              .ok());
      const auto box_type = create_box_type_with_positions(token, 1);
      EXPECT_EQ(
          create_placed_box(token, box_type, rack_id, "Box", std::array{0, 0, 0}).error_code(),
          grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(BoxLayout3dTest, TwoLiveSiblingsCannotShareACell) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);
      ASSERT_TRUE(
          create_placed_container(
              token, {.parent_id = freezer.root_id, .name = "A", .placement = std::array{1, 1, 0}})
              .ok());
      EXPECT_EQ(
          create_placed_container(
              token, {.parent_id = freezer.root_id, .name = "B", .placement = std::array{1, 1, 0}})
              .error_code(),
          grpc::StatusCode::ALREADY_EXISTS);
    }

    TEST_F(BoxLayout3dTest, BoxPlacementFollowsTheRackGridAndAnInvalidGridIsRefused) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);
      EXPECT_EQ(create_placed_container(token, {.parent_id = freezer.root_id,
                                                .name = "Bad",
                                                .placement = std::array{0, 0, 0},
                                                .grid = std::array{0, 1, 4}})
                    .error_code(),
                grpc::StatusCode::INVALID_ARGUMENT);

      std::string rack_id;
      ASSERT_TRUE(create_placed_container(token,
                                          {.parent_id = freezer.root_id,
                                           .name = "Rack",
                                           .placement = std::array{0, 0, 0},
                                           .grid = std::array{8, 1, 4}},
                                          &rack_id)
                      .ok());
      const auto box_type = create_box_type_with_positions(token, 1);
      EXPECT_TRUE(
          create_placed_box(token, box_type, rack_id, "Tier 8 back", std::array{7, 0, 3}).ok());
      EXPECT_EQ(
          create_placed_box(token, box_type, rack_id, "Too deep", std::array{0, 0, 4}).error_code(),
          grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_EQ(
          create_placed_box(token, box_type, rack_id, "Twin", std::array{7, 0, 3}).error_code(),
          grpc::StatusCode::ALREADY_EXISTS);
    }

    TEST_F(BoxLayout3dTest, UpdateContainerKeepsAbsentLayoutFieldsAndClearsOnRequest) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);
      std::string rack_id;
      ASSERT_TRUE(create_placed_container(token,
                                          {.parent_id = freezer.root_id,
                                           .name = "Rack",
                                           .placement = std::array{1, 2, 0},
                                           .grid = std::array{8, 1, 4}},
                                          &rack_id)
                      .ok());

      auto current = get_container(token, rack_id);
      current.clear_placement();
      current.clear_grid();
      current.set_name("Renamed rack");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::UpdateStorageContainerRequest req;
        *req.mutable_container() = current;
        fmgr::v1::UpdateStorageContainerResponse resp;
        ASSERT_TRUE(box_stub_->UpdateStorageContainer(&ctx, req, &resp).ok());
        EXPECT_TRUE(resp.container().has_placement());
        EXPECT_TRUE(resp.container().has_grid());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateStorageContainerRequest req;
      *req.mutable_container() = current;
      req.set_clear_placement(true);
      req.set_clear_grid(true);
      fmgr::v1::UpdateStorageContainerResponse resp;
      ASSERT_TRUE(box_stub_->UpdateStorageContainer(&ctx, req, &resp).ok());
      const auto stored = get_container(token, rack_id);
      EXPECT_FALSE(stored.has_placement());
      EXPECT_FALSE(stored.has_grid());
      EXPECT_EQ(stored.name(), "Renamed rack");
    }

    TEST_F(BoxLayout3dTest, ShrinkingAGridUnderAPlacedBoxIsAFailedPrecondition) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);
      std::string rack_id;
      ASSERT_TRUE(create_placed_container(token,
                                          {.parent_id = freezer.root_id,
                                           .name = "Rack",
                                           .placement = std::array{0, 0, 0},
                                           .grid = std::array{8, 1, 4}},
                                          &rack_id)
                      .ok());
      const auto box_type = create_box_type_with_positions(token, 1);
      ASSERT_TRUE(create_placed_box(token, box_type, rack_id, "Top", std::array{7, 0, 0}).ok());

      auto current = get_container(token, rack_id);
      current.mutable_grid()->set_levels(4);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateStorageContainerRequest req;
      *req.mutable_container() = current;
      fmgr::v1::UpdateStorageContainerResponse resp;
      EXPECT_EQ(box_stub_->UpdateStorageContainer(&ctx, req, &resp).error_code(),
                grpc::StatusCode::FAILED_PRECONDITION);
    }

    TEST_F(BoxLayout3dTest, ChangingTheModelUnderAStrandedRackIsAFailedPrecondition) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);
      // Rack slot 6 exists in the F740 (6 slots) but not in the TSX (4 slots).
      ASSERT_TRUE(
          create_placed_container(
              token, {.parent_id = freezer.root_id, .name = "R6", .placement = std::array{0, 5, 0}})
              .ok());
      auto current = get_freezer(token, freezer.id);
      current.set_freezer_model_id(kTsx);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateFreezerRequest req;
      *req.mutable_freezer() = current;
      fmgr::v1::UpdateFreezerResponse resp;
      EXPECT_EQ(box_stub_->UpdateFreezer(&ctx, req, &resp).error_code(),
                grpc::StatusCode::FAILED_PRECONDITION);
    }

    TEST_F(BoxLayout3dTest, UpdateBoxKeepsAnAbsentPlacementAndClearsOnRequest) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);
      std::string rack_id;
      ASSERT_TRUE(create_placed_container(token,
                                          {.parent_id = freezer.root_id,
                                           .name = "Rack",
                                           .placement = std::array{0, 0, 0},
                                           .grid = std::array{8, 1, 4}},
                                          &rack_id)
                      .ok());
      const auto box_type = create_box_type_with_positions(token, 1);
      std::string box_id;
      ASSERT_TRUE(
          create_placed_box(token, box_type, rack_id, "Box", std::array{3, 0, 1}, &box_id).ok());

      fmgr::v1::Box current;
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GetBoxRequest req;
        req.set_box_id(box_id);
        fmgr::v1::GetBoxResponse resp;
        ASSERT_TRUE(box_stub_->GetBox(&ctx, req, &resp).ok());
        ASSERT_TRUE(resp.box().has_placement());
        EXPECT_EQ(resp.box().placement().level(), 3);
        current = resp.box();
      }
      current.clear_placement();
      current.set_label("Relabelled");
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::UpdateBoxRequest req;
        *req.mutable_box() = current;
        fmgr::v1::UpdateBoxResponse resp;
        ASSERT_TRUE(box_stub_->UpdateBox(&ctx, req, &resp).ok());
        EXPECT_TRUE(resp.box().has_placement());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateBoxRequest req;
      *req.mutable_box() = current;
      req.set_clear_placement(true);
      fmgr::v1::UpdateBoxResponse resp;
      ASSERT_TRUE(box_stub_->UpdateBox(&ctx, req, &resp).ok());
      EXPECT_FALSE(resp.box().has_placement());
    }

    TEST_F(BoxLayout3dTest, PlacementWritesAreAuditedWithTheNewFields) {
      const auto token = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(token, kF740);
      std::string rack_id;
      ASSERT_TRUE(create_placed_container(
                      token,
                      {.parent_id = freezer.root_id, .name = "R", .placement = std::array{2, 4, 0}},
                      &rack_id)
                      .ok());

      auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
      const auto events =
          txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      txn->commit();
      bool found = false;
      for (const auto& event : events) {
        if (event.entity_id == rack_id) {
          const auto after = nlohmann::json::parse(event.after_json);
          EXPECT_EQ(after.at("placement").at("column"), 4);
          found = true;
        }
      }
      EXPECT_TRUE(found) << "no audit row for the placed rack";
    }

    TEST_F(BoxLayout3dTest, MemberReadsTheSceneParentsFirstWithOccupancyAndNoArchivedNodes) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(admin, kTsx, "TSX #1");
      std::string rack_id;
      ASSERT_TRUE(create_placed_container(admin,
                                          {.parent_id = freezer.root_id,
                                           .name = "L4 R2",
                                           .placement = std::array{3, 1, 0},
                                           .grid = std::array{5, 1, 5}},
                                          &rack_id)
                      .ok());
      std::string archived_rack;
      ASSERT_TRUE(create_placed_container(admin, {.parent_id = freezer.root_id, .name = "Gone"},
                                          &archived_rack)
                      .ok());
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, admin);
        fmgr::v1::ArchiveStorageContainerRequest req;
        req.set_container_id(archived_rack);
        fmgr::v1::ArchiveStorageContainerResponse resp;
        ASSERT_TRUE(box_stub_->ArchiveStorageContainer(&ctx, req, &resp).ok());
      }
      const auto box_type = create_box_type_with_positions(admin, 4);
      std::string box_id;
      ASSERT_TRUE(create_placed_box(admin, box_type, rack_id, "Drawer 2 box 3", std::array{1, 0, 2},
                                    &box_id)
                      .ok());
      seed_samples_in_box(box_id, 3);

      const auto member = login(kMemberEmail, kPassword);
      fmgr::v1::GetFreezerSceneResponse scene;
      const auto status = get_scene(member, freezer.id, &scene);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(scene.freezer().id(), freezer.id);
      ASSERT_TRUE(scene.has_model());
      EXPECT_EQ(scene.model().id(), kTsx);

      ASSERT_EQ(scene.nodes_size(), 3) << "root, rack and box; the archived rack is left out";
      EXPECT_EQ(scene.nodes(0).id(), freezer.root_id);
      EXPECT_FALSE(scene.nodes(0).has_parent_id());
      EXPECT_EQ(scene.nodes(1).id(), rack_id);
      EXPECT_EQ(scene.nodes(1).kind(), fmgr::v1::SCENE_NODE_KIND_CONTAINER);
      EXPECT_EQ(scene.nodes(1).parent_id(), freezer.root_id);
      EXPECT_EQ(scene.nodes(1).placement().level(), 3);
      EXPECT_EQ(scene.nodes(1).grid().depths(), 5);
      const auto& box = scene.nodes(2);
      EXPECT_EQ(box.id(), box_id);
      EXPECT_EQ(box.kind(), fmgr::v1::SCENE_NODE_KIND_BOX);
      EXPECT_EQ(box.parent_id(), rack_id);
      EXPECT_EQ(box.label(), "Drawer 2 box 3");
      EXPECT_EQ(box.occupied_positions(), 3);
      EXPECT_EQ(box.total_positions(), 4);
    }

    TEST_F(BoxLayout3dTest, SceneOfAFreezerWithoutModelHasNoModel) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(admin, "");
      fmgr::v1::GetFreezerSceneResponse scene;
      ASSERT_TRUE(get_scene(admin, freezer.id, &scene).ok());
      EXPECT_FALSE(scene.has_model());
      ASSERT_EQ(scene.nodes_size(), 1);
    }

    TEST_F(BoxLayout3dTest, SceneRejectsAnOutsiderAndHidesUnknownAndArchivedFreezers) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(admin, kF740);

      fmgr::v1::GetFreezerSceneResponse scene;
      EXPECT_EQ(get_scene(login(kOutsiderEmail, kPassword), freezer.id, &scene).error_code(),
                grpc::StatusCode::PERMISSION_DENIED);
      EXPECT_EQ(get_scene(admin, "30000000-0000-0000-0000-00000000dead", &scene).error_code(),
                grpc::StatusCode::NOT_FOUND);

      grpc::ClientContext ctx;
      set_bearer(ctx, admin);
      fmgr::v1::ArchiveFreezerRequest req;
      req.set_freezer_id(freezer.id);
      fmgr::v1::ArchiveFreezerResponse resp;
      ASSERT_TRUE(box_stub_->ArchiveFreezer(&ctx, req, &resp).ok());
      EXPECT_EQ(get_scene(admin, freezer.id, &scene).error_code(), grpc::StatusCode::NOT_FOUND);
    }

    TEST_F(BoxLayout3dTest, SceneWithoutBearerIsUnauthenticated) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto freezer = create_freezer(admin, kF740);
      grpc::ClientContext ctx;
      fmgr::v1::GetFreezerSceneRequest req;
      req.set_freezer_id(freezer.id);
      fmgr::v1::GetFreezerSceneResponse resp;
      EXPECT_EQ(box_stub_->GetFreezerScene(&ctx, req, &resp).error_code(),
                grpc::StatusCode::UNAUTHENTICATED);
    }

  } // namespace
} // namespace fmgr::test
