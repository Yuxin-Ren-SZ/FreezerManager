// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "core/identity.h"
#include "core/permissions.h"
#include "core/role.h"
#include "rpc/AuthMiddleware.h"
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
#include <fmgr/v1/sample.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

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
             ("fmgr-item-type-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Four principals across two labs:
    //   - admin   : SystemAdmin in lab1 (holds ItemTypeDefine + CustomFieldDefine)
    //   - member  : Member in lab1 (holds SampleRead, neither define permission)
    //   - readonly: ReadOnly in lab1 (holds SampleRead + AuditRead only)
    //   - outsider: SystemAdmin in lab2 only (holds nothing for lab1)
    //
    // The catalog *read* RPCs (ListItemTypes/GetItemType/ListCustomFieldDefinitions)
    // are gated on SampleRead so a Member can render a generated sample form
    // (#69); `readonly` asserts that this reaches the read-only role too, and the
    // mutating define RPCs use `member` as the negative authz principal.
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
        sample_stub_ = fmgr::v1::SampleService::NewStub(channel_);
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

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      std::string create_child_item_type(const std::string& token, const std::string& lab,
                                         const std::string& parent_id, const std::string& name) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateItemTypeRequest req;
        req.set_lab_id(lab);
        req.set_parent_id(parent_id);
        req.set_name(name);
        fmgr::v1::CreateItemTypeResponse resp;
        EXPECT_TRUE(item_type_stub_->CreateItemType(&ctx, req, &resp).ok());
        return resp.item_type().id();
      }

      // A CFD write as the tightening tests vary it. `item_type_id` empty means
      // lab-global; the update helper sets it explicitly because the proto
      // replaces the whole definition, attachment included.
      struct CfdWriteSpec {
        std::string item_type_id;
        std::string key{"field"};
        bool required{false};
        std::string validation_json{"{}"};
      };

      // Returns the status rather than asserting it, so a test can pin the
      // refusal. `out_id` receives the created id on success.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      grpc::Status create_cfd_spec(const std::string& token, const std::string& lab,
                                   const CfdWriteSpec& spec, std::string* out_id = nullptr) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateCfdRequest req;
        auto* const cfd = req.mutable_cfd();
        cfd->set_lab_id(lab);
        cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
        if (!spec.item_type_id.empty()) {
          cfd->set_item_type_id(spec.item_type_id);
        }
        cfd->set_key(spec.key);
        cfd->set_label(spec.key + " label");
        cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
        cfd->set_required(spec.required);
        cfd->set_validation_json(spec.validation_json);
        fmgr::v1::CreateCfdResponse resp;
        const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
        if (status.ok() && out_id != nullptr) {
          *out_id = resp.cfd().id();
        }
        return status;
      }

      // Setup counterpart of `create_cfd_spec`: expects success and returns the id.
      std::string make_cfd(const std::string& token, const std::string& lab,
                           const CfdWriteSpec& spec) {
        std::string id;
        EXPECT_TRUE(create_cfd_spec(token, lab, spec, &id).ok());
        return id;
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      grpc::Status update_cfd_spec(const std::string& token, const std::string& lab,
                                   const std::string& id, const CfdWriteSpec& spec) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::UpdateCfdRequest req;
        auto* const cfd = req.mutable_cfd();
        cfd->set_id(id);
        cfd->set_lab_id(lab);
        cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
        if (!spec.item_type_id.empty()) {
          cfd->set_item_type_id(spec.item_type_id);
        }
        cfd->set_key(spec.key);
        cfd->set_label(spec.key + " label");
        cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
        cfd->set_required(spec.required);
        cfd->set_validation_json(spec.validation_json);
        fmgr::v1::UpdateCfdResponse resp;
        return item_type_stub_->UpdateCustomFieldDefinition(&ctx, req, &resp);
      }

      // The stored definition of `key` attached to `item_type_id`, read back
      // through the list RPC ("" = lab-global). Used to prove a refused write
      // left no partial change behind.
      [[nodiscard]] std::optional<fmgr::v1::CustomFieldDefinition>
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      stored_cfd(const std::string& token, const std::string& item_type_id,
                 const std::string& key) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListCfdsRequest req;
        req.set_lab_id(kLab1);
        if (!item_type_id.empty()) {
          req.set_item_type_id(item_type_id);
        }
        fmgr::v1::ListCfdsResponse resp;
        if (!item_type_stub_->ListCustomFieldDefinitions(&ctx, req, &resp).ok()) {
          return std::nullopt;
        }
        for (const auto& cfd : resp.cfds()) {
          if (cfd.key() == key) {
            return cfd;
          }
        }
        return std::nullopt;
      }

      // The row of `key` whatever it is attached to. `stored_cfd` filters by
      // item type, and an empty `item_type_id` on the request means *no filter*
      // rather than "lab-global", so it cannot tell a global that stayed global
      // from one that was narrowed onto a type — the thing #115's global tests
      // assert. Ask for the row, then look at its attachment.
      [[nodiscard]] std::optional<fmgr::v1::CustomFieldDefinition>
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      stored_cfd_anywhere(const std::string& token, const std::string& key) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListCfdsRequest req;
        req.set_lab_id(kLab1);
        fmgr::v1::ListCfdsResponse resp;
        if (!item_type_stub_->ListCustomFieldDefinitions(&ctx, req, &resp).ok()) {
          return std::nullopt;
        }
        for (const auto& cfd : resp.cfds()) {
          if (cfd.key() == key) {
            return cfd;
          }
        }
        return std::nullopt;
      }

      // What a type's subtree *resolves*, observed through the one RPC that runs
      // the server's own resolver: `CreateSample` validates the incoming custom
      // fields against `resolve_custom_field_defs`. Deliberately not a second
      // copy of the ranking inside the test — the assertion is about the
      // server's resolution, so it has to ask the server (#115).
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      grpc::Status create_sample_of_type(const std::string& token, const std::string& item_type_id,
                                         const std::string& custom_fields_json) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CreateSampleRequest req;
        req.set_lab_id(kLab1);
        req.set_item_type_id(item_type_id);
        req.set_name("sample of " + item_type_id);
        req.set_custom_fields_json(custom_fields_json);
        fmgr::v1::CreateSampleResponse resp;
        return sample_stub_->CreateSample(&ctx, req, &resp);
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
      std::unique_ptr<fmgr::v1::ItemTypeService::Stub> item_type_stub_;
      std::unique_ptr<fmgr::v1::SampleService::Stub> sample_stub_;

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
        const core::UserId readonly_id =
            core::UserId::parse("10000000-0000-0000-0000-000000000004");

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

    // #69: the catalog read RPCs are gated on sample.read, the permission the
    // sample screens already require, so a Member can render the custom fields of
    // a generated sample form. This member holds neither define permission.
    TEST_F(ItemTypeServiceTest, ListItemTypesAllowsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      create_item_type(admin, kLab1, "liquid");
      create_item_type(admin, kLab1, "solid");

      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ListItemTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListItemTypesResponse resp;
      const auto status = item_type_stub_->ListItemTypes(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.item_types_size(), 2);
    }

    TEST_F(ItemTypeServiceTest, ListItemTypesAllowsReadOnly) {
      const auto admin = login(kAdminEmail, kPassword);
      create_item_type(admin, kLab1, "liquid");

      const auto readonly_user = login(kReadOnlyEmail, kPassword);
      ASSERT_FALSE(readonly_user.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, readonly_user);
      fmgr::v1::ListItemTypesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListItemTypesResponse resp;
      const auto status = item_type_stub_->ListItemTypes(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.item_types_size(), 1);
    }

    // Relaxing the read must not make the catalog readable across labs.
    TEST_F(ItemTypeServiceTest, ListItemTypesRejectsOutsiderCrossLab) {
      const auto admin = login(kAdminEmail, kPassword);
      create_item_type(admin, kLab1, "liquid");

      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
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

    // #69: GetItemType resolves the owning lab from the row and then checks the
    // same read permission, so a Member can load the item type a form needs.
    TEST_F(ItemTypeServiceTest, GetItemTypeAllowsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto id = create_item_type(admin, kLab1, "liquid");

      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::GetItemTypeRequest req;
      req.set_item_type_id(id);
      fmgr::v1::GetItemTypeResponse resp;
      const auto status = item_type_stub_->GetItemType(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
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

    // #69: the custom-field catalog a generated sample form is built from is a
    // read of the sample's own schema, not a schema-authoring operation.
    TEST_F(ItemTypeServiceTest, ListCfdsAllowsMember) {
      const auto admin = login(kAdminEmail, kPassword);
      create_cfd(admin, kLab1, "global_key");
      create_cfd(admin, kLab1, "other_key");

      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::ListCfdsRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListCfdsResponse resp;
      const auto status = item_type_stub_->ListCustomFieldDefinitions(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.cfds_size(), 2);
    }

    TEST_F(ItemTypeServiceTest, ListCfdsAllowsReadOnly) {
      const auto admin = login(kAdminEmail, kPassword);
      create_cfd(admin, kLab1, "global_key");

      const auto readonly_user = login(kReadOnlyEmail, kPassword);
      ASSERT_FALSE(readonly_user.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, readonly_user);
      fmgr::v1::ListCfdsRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListCfdsResponse resp;
      const auto status = item_type_stub_->ListCustomFieldDefinitions(&ctx, req, &resp);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(resp.cfds_size(), 1);
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

    // =====================================================================
    // Tightening an inherited definition (#103)
    // =====================================================================
    //
    // N5: "a child may tighten a parent's field but must not drop a required
    // parent field". G3.9's form enforces it in the browser; these tests hold
    // the *server* to the same rule with no web client in the picture, on both
    // write RPCs, and in both directions — a check that only refuses things is
    // as wrong as one that only allows them.

    TEST_F(ItemTypeServiceTest, CreateCfdRejectsLooseningInheritedDefinition) {
      const auto token = login(kAdminEmail, kPassword);
      const auto parent_type = create_item_type(token, kLab1, "liquid");
      const auto child_type = create_child_item_type(token, kLab1, parent_type, "blood");
      make_cfd(
          token, kLab1,
          {.item_type_id = parent_type, .key = "notes", .validation_json = R"({"max_length":20})"});

      // The child raises the cap it inherits: values the parent's type refuses
      // would be accepted under the child.
      const auto status = create_cfd_spec(
          token, kLab1,
          {.item_type_id = child_type, .key = "notes", .validation_json = R"({"max_length":40})"});
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("max_length"), std::string::npos);
      EXPECT_EQ(stored_cfd(token, child_type, "notes"), std::nullopt);
    }

    // The rule's name, and the case a hurried implementation folds into
    // "narrower": dropping `required` is not a constraint change, it is the
    // whole requirement disappearing for the subtree.
    TEST_F(ItemTypeServiceTest, UpdateCfdRejectsDroppingRequiredInheritedField) {
      const auto token = login(kAdminEmail, kPassword);
      const auto parent_type = create_item_type(token, kLab1, "liquid");
      const auto child_type = create_child_item_type(token, kLab1, parent_type, "blood");
      make_cfd(token, kLab1, {.item_type_id = parent_type, .key = "patient_id", .required = true});
      // The child override keeps the requirement, which is a legitimate tightening.
      const auto child_cfd = make_cfd(
          token, kLab1, {.item_type_id = child_type, .key = "patient_id", .required = true});

      const auto status =
          update_cfd_spec(token, kLab1, child_cfd,
                          {.item_type_id = child_type, .key = "patient_id", .required = false});
      ASSERT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("required"), std::string::npos);

      // And the refusal wrote nothing: the requirement is still in place.
      const auto stored = stored_cfd(token, child_type, "patient_id");
      ASSERT_TRUE(stored.has_value());
      EXPECT_TRUE(stored->required());
    }

    TEST_F(ItemTypeServiceTest, UpdateCfdRejectsWideningInheritedConstraint) {
      const auto token = login(kAdminEmail, kPassword);
      const auto parent_type = create_item_type(token, kLab1, "liquid");
      const auto child_type = create_child_item_type(token, kLab1, parent_type, "blood");
      make_cfd(
          token, kLab1,
          {.item_type_id = parent_type, .key = "notes", .validation_json = R"({"max_length":20})"});
      const auto child_cfd = make_cfd(
          token, kLab1,
          {.item_type_id = child_type, .key = "notes", .validation_json = R"({"max_length":5})"});

      // Raising the child's cap back to the parent's 20 would be allowed —
      // equal is a tightening; only going *beyond* the inherited limit is not.
      const auto status = update_cfd_spec(
          token, kLab1, child_cfd,
          {.item_type_id = child_type, .key = "notes", .validation_json = R"({"max_length":40})"});
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("max_length"), std::string::npos);
    }

    TEST_F(ItemTypeServiceTest, UpdateCfdAllowsTighteningInheritedConstraint) {
      const auto token = login(kAdminEmail, kPassword);
      const auto parent_type = create_item_type(token, kLab1, "liquid");
      const auto child_type = create_child_item_type(token, kLab1, parent_type, "blood");
      make_cfd(
          token, kLab1,
          {.item_type_id = parent_type, .key = "notes", .validation_json = R"({"max_length":20})"});
      const auto child_cfd = make_cfd(
          token, kLab1,
          {.item_type_id = child_type, .key = "notes", .validation_json = R"({"max_length":5})"});

      const auto status = update_cfd_spec(
          token, kLab1, child_cfd,
          {.item_type_id = child_type, .key = "notes", .validation_json = R"({"max_length":3})"});
      ASSERT_TRUE(status.ok()) << status.error_message();
      const auto stored = stored_cfd(token, child_type, "notes");
      ASSERT_TRUE(stored.has_value());
      EXPECT_EQ(stored->validation_json(), R"({"max_length":3})");
    }

    // The permissive direction: an optional inherited field *may* be made
    // required, and a brand-new field with no inherited counterpart may be
    // anything at all. A rule that refused these would be tighter than the one
    // the SPA implements.
    TEST_F(ItemTypeServiceTest, UpdateCfdAllowsRequiringAnOptionalInheritedField) {
      const auto token = login(kAdminEmail, kPassword);
      const auto parent_type = create_item_type(token, kLab1, "liquid");
      const auto child_type = create_child_item_type(token, kLab1, parent_type, "blood");
      make_cfd(token, kLab1, {.item_type_id = parent_type, .key = "notes"});
      const auto child_cfd = make_cfd(token, kLab1, {.item_type_id = child_type, .key = "notes"});

      const auto status = update_cfd_spec(
          token, kLab1, child_cfd, {.item_type_id = child_type, .key = "notes", .required = true});
      EXPECT_TRUE(status.ok()) << status.error_message();
      const auto stored = stored_cfd(token, child_type, "notes");
      ASSERT_TRUE(stored.has_value());
      EXPECT_TRUE(stored->required());
    }

    // A lab-global definition is inherited by every node, so an override of it
    // is bound by the same rule — including at a root, which has no ancestors.
    TEST_F(ItemTypeServiceTest, CreateCfdRejectsDroppingARequiredLabGlobalField) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      make_cfd(token, kLab1, {.key = "patient_id", .required = true});

      const auto status = create_cfd_spec(
          token, kLab1, {.item_type_id = root_type, .key = "patient_id", .required = false});
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("required"), std::string::npos);
    }

    TEST_F(ItemTypeServiceTest, UpdateCfdWithoutAnInheritedCounterpartIsUnconstrained) {
      const auto token = login(kAdminEmail, kPassword);
      const auto parent_type = create_item_type(token, kLab1, "liquid");
      const auto child_type = create_child_item_type(token, kLab1, parent_type, "blood");
      const auto child_cfd = make_cfd(token, kLab1, {.item_type_id = child_type, .key = "own_key"});

      // Nothing above the child defines `own_key`, so a wide constraint is not a
      // loosening of anything.
      const auto status = update_cfd_spec(token, kLab1, child_cfd,
                                          {.item_type_id = child_type,
                                           .key = "own_key",
                                           .validation_json = R"({"max_length":400})"});
      EXPECT_TRUE(status.ok()) << status.error_message();
    }

    // =====================================================================
    // Moving a definition between item types (#115)
    // =====================================================================
    //
    // An attachment decides *which* subtree a definition constrains, so an
    // update that changes `item_type_id` writes to two subtrees at once: the
    // destination inherits something new, and the source falls back to whatever
    // the moved row was shadowing. Checking only the destination is a complete
    // bypass — the move is a no-op there whenever the two subtrees share their
    // inherited definition — and it is one no client in this repo exercises,
    // because the SPA cannot express a move. That is why every test below pins
    // the *source* subtree's resolution, not merely the refusal: a fix that only
    // looks at the destination passes the refusal-free half of each test.

    TEST_F(ItemTypeServiceTest, UpdateCfdRejectsMovingAnOverrideOutOfTheSubtreeItConstrains) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto blood_type = create_child_item_type(token, kLab1, root_type, "blood");
      const auto csf_type = create_child_item_type(token, kLab1, root_type, "csf");
      // The lab-global is the weaker definition the old subtree would fall back
      // to; `blood` tightens it by requiring the field.
      make_cfd(token, kLab1, {.key = "patient_id"});
      const auto blood_cfd = make_cfd(
          token, kLab1, {.item_type_id = blood_type, .key = "patient_id", .required = true});

      // Preconditions, so nothing below is vacuous: `blood` resolves the
      // requirement today, its sibling does not.
      EXPECT_FALSE(create_sample_of_type(token, blood_type, "{}").ok());
      EXPECT_TRUE(create_sample_of_type(token, csf_type, "{}").ok());

      // `csf` inherits the same optional global, so the destination end sees a
      // tightening and nothing else. `blood` is what loses.
      const auto status =
          update_cfd_spec(token, kLab1, blood_cfd,
                          {.item_type_id = csf_type, .key = "patient_id", .required = true});
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("required"), std::string::npos);

      // The old subtree's resolution, asked of the server's own resolver: a
      // sample of `blood` with no `patient_id` is still refused, and the row is
      // still attached to `blood`.
      EXPECT_FALSE(create_sample_of_type(token, blood_type, "{}").ok());
      const auto stored = stored_cfd(token, blood_type, "patient_id");
      ASSERT_TRUE(stored.has_value());
      EXPECT_TRUE(stored->required());
      // And nothing landed on the destination.
      EXPECT_EQ(stored_cfd(token, csf_type, "patient_id"), std::nullopt);
      EXPECT_TRUE(create_sample_of_type(token, csf_type, "{}").ok());
    }

    // The same write where the old subtree has nothing at all to fall back to:
    // the definition does not get weaker, it disappears, which is the largest
    // loosening the rule has a name for.
    TEST_F(ItemTypeServiceTest, UpdateCfdRejectsMovingTheOnlyDefinitionOutOfASubtree) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto blood_type = create_child_item_type(token, kLab1, root_type, "blood");
      const auto csf_type = create_child_item_type(token, kLab1, root_type, "csf");
      const auto blood_cfd = make_cfd(
          token, kLab1, {.item_type_id = blood_type, .key = "patient_id", .required = true});

      const auto status =
          update_cfd_spec(token, kLab1, blood_cfd,
                          {.item_type_id = csf_type, .key = "patient_id", .required = true});
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("required"), std::string::npos);

      EXPECT_FALSE(create_sample_of_type(token, blood_type, "{}").ok());
      const auto stored = stored_cfd(token, blood_type, "patient_id");
      ASSERT_TRUE(stored.has_value());
      EXPECT_TRUE(stored->required());
    }

    // The permissive direction: a move that leaves the old subtree exactly as
    // constrained as it was is a legitimate reorganization, and the rule must
    // allow it. A check that refused every re-parent would be tighter than N5.
    TEST_F(ItemTypeServiceTest, UpdateCfdAllowsMovingAnOverrideWhenTheOldSubtreeInheritsTheSame) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto blood_type = create_child_item_type(token, kLab1, root_type, "blood");
      const auto csf_type = create_child_item_type(token, kLab1, root_type, "csf");
      make_cfd(token, kLab1, {.key = "patient_id", .required = true});
      const auto blood_cfd = make_cfd(
          token, kLab1, {.item_type_id = blood_type, .key = "patient_id", .required = true});

      const auto status =
          update_cfd_spec(token, kLab1, blood_cfd,
                          {.item_type_id = csf_type, .key = "patient_id", .required = true});
      ASSERT_TRUE(status.ok()) << status.error_message();

      // Moved, and the requirement is still resolved on both sides — `blood`
      // through the global it now inherits, `csf` through the row it received.
      EXPECT_EQ(stored_cfd(token, blood_type, "patient_id"), std::nullopt);
      ASSERT_TRUE(stored_cfd(token, csf_type, "patient_id").has_value());
      EXPECT_FALSE(create_sample_of_type(token, blood_type, "{}").ok());
      EXPECT_FALSE(create_sample_of_type(token, csf_type, "{}").ok());
    }

    // A lab-global's source subtree is every item type in the lab, so narrowing
    // one onto a single type drops it for all the others. Its destination end is
    // a no-op — the type inherits the very row being attached to it — so this is
    // the same bypass from the other side.
    TEST_F(ItemTypeServiceTest, UpdateCfdRejectsNarrowingAConstrainedLabGlobalOntoOneItemType) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto blood_type = create_child_item_type(token, kLab1, root_type, "blood");
      const auto global_cfd = make_cfd(token, kLab1, {.key = "patient_id", .required = true});

      const auto status =
          update_cfd_spec(token, kLab1, global_cfd,
                          {.item_type_id = blood_type, .key = "patient_id", .required = true});
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("required"), std::string::npos);

      // Still lab-global, and the requirement still reaches a type that has no
      // definition of its own.
      const auto stored = stored_cfd_anywhere(token, "patient_id");
      ASSERT_TRUE(stored.has_value());
      EXPECT_FALSE(stored->has_item_type_id());
      EXPECT_FALSE(create_sample_of_type(token, blood_type, "{}").ok());
    }

    // The boundary, stated as a test: the rule is about what a definition
    // *refuses*, not about which nodes may carry a field. A definition that
    // constrains nothing can still be moved, narrowed included — the same line
    // `tighten_violations` draws for `indexed`.
    TEST_F(ItemTypeServiceTest, UpdateCfdAllowsNarrowingALabGlobalThatConstrainsNothing) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto global_cfd = make_cfd(token, kLab1, {.key = "free_text"});

      const auto status = update_cfd_spec(token, kLab1, global_cfd,
                                          {.item_type_id = root_type, .key = "free_text"});
      ASSERT_TRUE(status.ok()) << status.error_message();
      const auto stored = stored_cfd_anywhere(token, "free_text");
      ASSERT_TRUE(stored.has_value());
      EXPECT_EQ(stored->item_type_id(), root_type);
    }

    // =====================================================================
    // Renaming a key (#121)
    // =====================================================================
    //
    // `UpdateCfdRequest` replaces `key` as well as the attachment, so a row can be
    // renamed where it stands. Neither #115 check runs for that — the node does
    // not change — and the destination check only asks whether the row suits the
    // *new* key's inheritance, which a rename onto a fresh key satisfies
    // trivially. What can weaken is the old key: the row stops shadowing what it
    // shadowed, and the subtree falls back to it.
    //
    // The line these tests hold: **a rename may shed a name and must not shed a
    // tightening.** A row that tightened an inherited definition cannot be
    // renamed away from it, because the subtree then accepts values it refused
    // before. A row that was only a relabel — equal to what it inherited, or with
    // nothing above it at all — renames freely, which is what fixing a typo in a
    // key is.

    TEST_F(ItemTypeServiceTest, UpdateCfdRejectsRenamingAnOverrideThatWouldShedItsTightening) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto blood_type = create_child_item_type(token, kLab1, root_type, "blood");
      make_cfd(token, kLab1,
               {.key = "notes", .required = true, .validation_json = R"({"max_length":100})"});
      const auto override_cfd =
          make_cfd(token, kLab1,
                   {.item_type_id = blood_type,
                    .key = "notes",
                    .required = true,
                    .validation_json = R"({"max_length":5})"});
      const std::string too_long(50, 'a');
      // The probe carries `notes_v2` as well, and the *same* payload is used
      // before and after the attempt. A rename that succeeded leaves `notes_v2`
      // required, so a probe carrying only `notes` would be refused in both runs
      // for a different reason and the assertion would pass without proving
      // anything; with the second key supplied, the only thing that can refuse
      // the payload is the `notes` cap under test. (Keys with no definition are
      // carried in the blob rather than rejected — `validate_custom_fields` walks
      // the definitions, not the payload.)
      const std::string value_json =
          std::string(R"({"notes":")") + too_long + R"(","notes_v2":"x"})";

      // Precondition, so the assertion below is about the rename rather than
      // about a sample that never worked: `blood` caps `notes` at 5 today.
      EXPECT_FALSE(create_sample_of_type(token, blood_type, value_json).ok());

      // Renaming to a fresh key releases the cap: `notes` would fall back to the
      // inherited 100, and the tightened row would constrain nothing.
      const auto status = update_cfd_spec(token, kLab1, override_cfd,
                                          {.item_type_id = blood_type,
                                           .key = "notes_v2",
                                           .required = true,
                                           .validation_json = R"({"max_length":5})"});
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("max_length"), std::string::npos);

      // The effective resolution is the assertion, not the status code: the value
      // refused before the rename is still refused after it, and the row is still
      // the `notes` definition it was.
      EXPECT_FALSE(create_sample_of_type(token, blood_type, value_json).ok());
      const auto stored = stored_cfd(token, blood_type, "notes");
      EXPECT_TRUE(stored.has_value());
      EXPECT_EQ(stored_cfd(token, blood_type, "notes_v2"), std::nullopt);
      if (stored.has_value()) {
        EXPECT_EQ(stored->validation_json(), R"({"max_length":5})");
      }

      // The refusal did not leave `notes` unusable: a value the tightened
      // definition accepts still stores.
      EXPECT_TRUE(create_sample_of_type(token, blood_type, R"({"notes":"short"})").ok());
    }

    // The use case the refusal must not break: fixing a typo in a key. Nothing
    // above the row defines `patinet_id`, so the rename sheds no inherited
    // constraint — and the row's own constraints travel with the new name.
    TEST_F(ItemTypeServiceTest, UpdateCfdAllowsRenamingAKeyThatInheritsNothingAndKeepsConstraints) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto blood_type = create_child_item_type(token, kLab1, root_type, "blood");
      const auto typo_cfd = make_cfd(token, kLab1,
                                     {.item_type_id = blood_type,
                                      .key = "patinet_id",
                                      .required = true,
                                      .validation_json = R"({"max_length":5})"});
      const std::string too_long(50, 'a');
      EXPECT_FALSE(create_sample_of_type(token, blood_type,
                                         std::string(R"({"patinet_id":")") + too_long + R"("})")
                       .ok());

      const auto status = update_cfd_spec(token, kLab1, typo_cfd,
                                          {.item_type_id = blood_type,
                                           .key = "patient_id",
                                           .required = true,
                                           .validation_json = R"({"max_length":5})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      // The resolution after the rename: the cap travelled with the row (50
      // characters is still refused, under the corrected key), the requirement
      // travelled with it (an empty sample is still refused), and the typo'd key
      // is gone rather than left behind as a second definition.
      EXPECT_EQ(stored_cfd(token, blood_type, "patinet_id"), std::nullopt);
      ASSERT_TRUE(stored_cfd(token, blood_type, "patient_id").has_value());
      EXPECT_FALSE(create_sample_of_type(token, blood_type,
                                         std::string(R"({"patient_id":")") + too_long + R"("})")
                       .ok());
      EXPECT_FALSE(create_sample_of_type(token, blood_type, "{}").ok());
      EXPECT_TRUE(create_sample_of_type(token, blood_type, R"({"patient_id":"short"})").ok());
    }

    // A rename is also a write of the new key, so the destination half of the
    // rule holds for it too: `notes` is inherited with a cap of 5, and a row
    // arriving under that name may not raise it.
    TEST_F(ItemTypeServiceTest, UpdateCfdRejectsRenamingAKeyOntoADefinitionItWouldLoosen) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto blood_type = create_child_item_type(token, kLab1, root_type, "blood");
      make_cfd(token, kLab1, {.key = "notes", .validation_json = R"({"max_length":5})"});
      const auto loose_cfd = make_cfd(token, kLab1,
                                      {.item_type_id = blood_type,
                                       .key = "notes_v2",
                                       .validation_json = R"({"max_length":50})"});

      const auto status = update_cfd_spec(token, kLab1, loose_cfd,
                                          {.item_type_id = blood_type,
                                           .key = "notes",
                                           .validation_json = R"({"max_length":50})"});
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("max_length"), std::string::npos);

      // Nothing was written: the inherited cap still holds and the wide row is
      // still where the rename found it.
      const std::string too_long(30, 'a');
      EXPECT_FALSE(create_sample_of_type(token, blood_type,
                                         std::string(R"({"notes":")") + too_long + R"("})")
                       .ok());
      ASSERT_TRUE(stored_cfd(token, blood_type, "notes_v2").has_value());
      EXPECT_EQ(stored_cfd(token, blood_type, "notes"), std::nullopt);
    }

    // The boundary of the refusal, as a test: renaming a row that was *equal* to
    // what it shadowed sheds no tightening, so it stays a relabel. The rule is
    // about the constraint that disappears, not about a key having a parent.
    TEST_F(ItemTypeServiceTest, UpdateCfdAllowsRenamingAKeyItWasNotTightening) {
      const auto token = login(kAdminEmail, kPassword);
      const auto root_type = create_item_type(token, kLab1, "liquid");
      const auto blood_type = create_child_item_type(token, kLab1, root_type, "blood");
      make_cfd(token, kLab1, {.key = "notes", .validation_json = R"({"max_length":100})"});
      const auto equal_cfd = make_cfd(token, kLab1,
                                      {.item_type_id = blood_type,
                                       .key = "notes",
                                       .validation_json = R"({"max_length":100})"});

      const auto status = update_cfd_spec(token, kLab1, equal_cfd,
                                          {.item_type_id = blood_type,
                                           .key = "notes_v2",
                                           .validation_json = R"({"max_length":100})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      // `notes` resolves to the inherited definition before and after, so what
      // the subtree accepts did not change; the relabelled row is where the
      // rename put it.
      const std::string fifty(50, 'a');
      EXPECT_TRUE(create_sample_of_type(token, blood_type,
                                        std::string(R"({"notes":")") + fifty + R"("})")
                      .ok());
      ASSERT_TRUE(stored_cfd(token, blood_type, "notes_v2").has_value());
      EXPECT_EQ(stored_cfd(token, blood_type, "notes"), std::nullopt);
    }

    // =====================================================================
    // Permission boundaries (#69)
    // =====================================================================

    // The acceptance criterion that a positive-only test would miss: relaxing the
    // catalog reads must not relax the catalog writes. One Member reads all three
    // RPCs successfully and is then still refused on all six mutating paths, so a
    // careless "relax everything in this file" change cannot pass.
    TEST_F(ItemTypeServiceTest, MemberCanReadItemTypeCatalogButStillCannotDefine) {
      const auto admin = login(kAdminEmail, kPassword);
      const auto item_type_id = create_item_type(admin, kLab1, "liquid");
      const auto cfd_id = create_cfd(admin, kLab1, "patient_id");

      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());

      // Reads: all three succeed, and return real rows.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::ListItemTypesRequest req;
        req.set_lab_id(kLab1);
        fmgr::v1::ListItemTypesResponse resp;
        const auto status = item_type_stub_->ListItemTypes(&ctx, req, &resp);
        ASSERT_TRUE(status.ok()) << status.error_message();
        EXPECT_EQ(resp.item_types_size(), 1);
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::GetItemTypeRequest req;
        req.set_item_type_id(item_type_id);
        fmgr::v1::GetItemTypeResponse resp;
        const auto status = item_type_stub_->GetItemType(&ctx, req, &resp);
        ASSERT_TRUE(status.ok()) << status.error_message();
        EXPECT_EQ(resp.item_type().id(), item_type_id);
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::ListCfdsRequest req;
        req.set_lab_id(kLab1);
        fmgr::v1::ListCfdsResponse resp;
        const auto status = item_type_stub_->ListCustomFieldDefinitions(&ctx, req, &resp);
        ASSERT_TRUE(status.ok()) << status.error_message();
        EXPECT_EQ(resp.cfds_size(), 1);
      }

      // Writes: every mutating path stays behind *.define.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::CreateItemTypeRequest req;
        req.set_lab_id(kLab1);
        req.set_name("hijack");
        fmgr::v1::CreateItemTypeResponse resp;
        const auto status = item_type_stub_->CreateItemType(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::UpdateItemTypeRequest req;
        auto* const it = req.mutable_item_type();
        it->set_id(item_type_id);
        it->set_lab_id(kLab1);
        it->set_name("hijack");
        fmgr::v1::UpdateItemTypeResponse resp;
        const auto status = item_type_stub_->UpdateItemType(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::ArchiveItemTypeRequest req;
        req.set_item_type_id(item_type_id);
        fmgr::v1::ArchiveItemTypeResponse resp;
        const auto status = item_type_stub_->ArchiveItemType(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::CreateCfdRequest req;
        auto* const cfd = req.mutable_cfd();
        cfd->set_lab_id(kLab1);
        cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
        cfd->set_key("hijack");
        cfd->set_label("Hijack");
        cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
        fmgr::v1::CreateCfdResponse resp;
        const auto status = item_type_stub_->CreateCustomFieldDefinition(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::UpdateCfdRequest req;
        auto* const cfd = req.mutable_cfd();
        cfd->set_id(cfd_id);
        cfd->set_lab_id(kLab1);
        cfd->set_scope_kind(fmgr::v1::SCOPE_KIND_SAMPLE);
        cfd->set_key("patient_id");
        cfd->set_label("hijack");
        cfd->set_data_type(fmgr::v1::FIELD_DATA_TYPE_TEXT);
        fmgr::v1::UpdateCfdResponse resp;
        const auto status = item_type_stub_->UpdateCustomFieldDefinition(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      }
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, member);
        fmgr::v1::ArchiveCfdRequest req;
        req.set_cfd_id(cfd_id);
        fmgr::v1::ArchiveCfdResponse resp;
        const auto status = item_type_stub_->ArchiveCustomFieldDefinition(&ctx, req, &resp);
        EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
      }
    }

    // The registry entry is documentation and the method's authorize() call is
    // the enforcement point (AGENTS.md §5), so the two have to move together.
    // RpcRegistryCoversAllExpectedMethods asserts only a count floor and cannot
    // see a permission mismatch (#60 makes the pairing structural); these two
    // tests pin the registry half for every RPC in this service. The behavioral
    // half is the tests above: if a method's authorize() still demanded
    // item_type.define, ListItemTypesAllowsMember would be red.
    void expect_registered(const std::string& rpc, core::Permission expected) {
      const auto registry = rpc::AuthMiddleware::registered_rpcs();
      const auto entry = registry.find(rpc);
      ASSERT_NE(entry, registry.end()) << rpc << " is missing from the RPC registry";
      // #78: an entry is either a permission the gate verifies or an explicit
      // "no permission required"; these RPCs must be the former.
      ASSERT_EQ(entry->second.kind(), rpc::RpcGate::Kind::Permission) << rpc;
      EXPECT_EQ(entry->second.permission(), expected) << rpc;
    }

    TEST_F(ItemTypeServiceTest, RegistryGatesCatalogReadsOnSampleRead) {
      expect_registered("/fmgr.v1.ItemTypeService/ListItemTypes", core::Permission::SampleRead);
      expect_registered("/fmgr.v1.ItemTypeService/GetItemType", core::Permission::SampleRead);
      expect_registered("/fmgr.v1.ItemTypeService/ListCustomFieldDefinitions",
                        core::Permission::SampleRead);
    }

    TEST_F(ItemTypeServiceTest, RegistryKeepsDefineOnCatalogWrites) {
      expect_registered("/fmgr.v1.ItemTypeService/CreateItemType",
                        core::Permission::ItemTypeDefine);
      expect_registered("/fmgr.v1.ItemTypeService/UpdateItemType",
                        core::Permission::ItemTypeDefine);
      expect_registered("/fmgr.v1.ItemTypeService/ArchiveItemType",
                        core::Permission::ItemTypeDefine);
      expect_registered("/fmgr.v1.ItemTypeService/CreateCustomFieldDefinition",
                        core::Permission::CustomFieldDefine);
      expect_registered("/fmgr.v1.ItemTypeService/UpdateCustomFieldDefinition",
                        core::Permission::CustomFieldDefine);
      expect_registered("/fmgr.v1.ItemTypeService/ArchiveCustomFieldDefinition",
                        core::Permission::CustomFieldDefine);
    }

  } // namespace
} // namespace fmgr::test
