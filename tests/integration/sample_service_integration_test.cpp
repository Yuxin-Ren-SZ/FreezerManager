// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "core/audit_event.h"
#include "core/box.h"
#include "core/freezer.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/role.h"
#include "core/sample.h"
#include "server/FreezerServer.h"
#include "storage/AuditTraits.h"
#include "storage/BoxGeometryTraits.h"
#include "storage/FreezerTraits.h"
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
#include "storage/sqlite/LoginAttemptRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SampleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/ShareRequestRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/sample.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
             ("fmgr-sample-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    // Four principals across two labs:
    //   - admin   : SystemAdmin in lab1 (holds every sample permission)
    //   - member  : Member in lab1 (holds SampleRead/Write/Checkout/DeleteSoft)
    //   - readonly: ReadOnly in lab1 (holds SampleRead only) — in-lab negative for
    //               every mutating RPC
    //   - outsider: SystemAdmin in lab2 only — cross-lab isolation negative
    //
    // Lab-1 sample-placement prerequisites (item type, container type, box type
    // with positions A1/A2, storage container, box) are seeded directly.
    class SampleServiceTest : public ::testing::Test {
    protected:
      // base64 of 32 bytes 0x00..0x1F — a fixed dev master KEK for the test server.
      static constexpr const char* kMasterKek = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";

      void SetUp() override {
        ::setenv("FMGR_MASTER_KEK", kMasterKek, 1); // NOLINT(concurrency-mt-unsafe)
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
        ::unsetenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
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

      // Create a sample as the given principal. Optionally place it at a position
      // (with the seeded container type) and attach custom fields. Returns the
      // gRPC status so callers can assert on both success and failure paths.
      struct CreateArgs {
        std::string token;
        std::string lab{};
        std::string name{"specimen"};
        std::string position{};       // empty = unplaced
        std::string container_type{}; // empty = none
        std::string custom_fields{};  // empty = {}
        std::int64_t volume_ul{0};    // >0 sets volume_value (µL)
      };
      grpc::Status create_sample(const CreateArgs& args, std::string* out_id) {
        grpc::ClientContext ctx;
        set_bearer(ctx, args.token);
        fmgr::v1::CreateSampleRequest req;
        req.set_lab_id(args.lab.empty() ? kLab1 : args.lab);
        req.set_item_type_id(kItemType);
        req.set_name(args.name);
        if (!args.position.empty()) {
          req.set_box_id(kBox);
          req.set_position_label(args.position);
        }
        if (!args.container_type.empty()) {
          req.set_container_type_id(args.container_type);
        }
        if (!args.custom_fields.empty()) {
          req.set_custom_fields_json(args.custom_fields);
        }
        if (args.volume_ul > 0) {
          req.set_volume_value(static_cast<double>(args.volume_ul));
          req.set_volume_unit("µL");
        }
        fmgr::v1::CreateSampleResponse resp;
        const auto status = sample_stub_->CreateSample(&ctx, req, &resp);
        if (out_id != nullptr) {
          *out_id = resp.sample().id();
        }
        return status;
      }

      const std::string kAdminEmail{"admin@example.com"};
      const std::string kMemberEmail{"member@example.com"};
      const std::string kReadonlyEmail{"readonly@example.com"};
      const std::string kOutsiderEmail{"outsider@example.com"};
      const std::string kPassword{"hunter22"};
      const std::string kLab1{"20000000-0000-0000-0000-000000000001"};
      const std::string kLab2{"20000000-0000-0000-0000-000000000002"};
      const std::string kItemType{"30000000-0000-0000-0000-000000000001"};
      const std::string kContainerType{"40000000-0000-0000-0000-000000000001"};
      const std::string kWrongContainerType{"40000000-0000-0000-0000-000000000002"};
      const std::string kBox{"70000000-0000-0000-0000-000000000001"};

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      server::FreezerServerOptions server_opts_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
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
              .is_phi_enabled = true, // PHI mode on so PHI custom fields can be stored
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

        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::Lab>().insert(make_lab(lab1, "Lab One"), ctx);
          txn->repo<core::Lab>().insert(make_lab(lab2, "Lab Two"), ctx);
          txn->repo<core::User>().insert(make_user(admin_id, kAdminEmail), ctx);
          txn->repo<core::User>().insert(make_user(member_id, kMemberEmail), ctx);
          txn->repo<core::User>().insert(make_user(outsider_id, kOutsiderEmail), ctx);
          txn->repo<core::User>().insert(make_user(readonly_id, kReadonlyEmail), ctx);
          txn->repo<core::LabMembership>().insert(
              make_membership(admin_id, lab1, core::RoleKind::SystemAdmin), ctx);
          txn->repo<core::LabMembership>().insert(
              make_membership(member_id, lab1, core::RoleKind::Member), ctx);
          txn->repo<core::LabMembership>().insert(
              make_membership(readonly_id, lab1, core::RoleKind::ReadOnly), ctx);
          txn->repo<core::LabMembership>().insert(
              make_membership(outsider_id, lab2, core::RoleKind::SystemAdmin), ctx);
          // PhiRead is excluded from every built-in role by default (PRD §3); grant
          // it to SystemAdmin here so `admin` is the phi.read-holder and `member`
          // (Member role) is the in-lab negative for PHI disclosure.
          txn->repo<core::RolePermission>().insert(
              core::RolePermission{.role_id = core::builtin_role_id(core::RoleKind::SystemAdmin),
                                   .permission = core::Permission::PhiRead},
              ctx);
          txn->repo<core::ItemType>().insert(
              core::ItemType{.id = core::ItemTypeId::parse(kItemType),
                             .lab_id = lab1,
                             .parent_id = std::nullopt,
                             .name = "liquid",
                             .created_at = core::Timestamp::from_unix_micros(1)},
              ctx);
          txn->commit();
        }
        {
          // PHI-tagged (is_phi) custom field, not required, on the seeded item type.
          // Separate transaction: the repository validates item_type_id against the
          // committed DB, not the staging map.
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::CustomFieldDefinition>().insert(
              core::CustomFieldDefinition{.id = core::CustomFieldDefinitionId::parse(
                                              "80000000-0000-0000-0000-0000000000ff"),
                                          .lab_id = lab1,
                                          .scope_kind = core::ScopeKind::Sample,
                                          .item_type_id = core::ItemTypeId::parse(kItemType),
                                          .key = "mrn",
                                          .label = "Medical Record Number",
                                          .data_type = core::FieldDataType::String,
                                          .required = false,
                                          .is_phi = true,
                                          .created_at = core::Timestamp::from_unix_micros(1)},
              ctx);
          txn->commit();
        }
        // Container types committed before the box type that references the
        // size_class, then the storage container, then the box.
        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::ContainerType>().insert(
              core::ContainerType{.id = core::ContainerTypeId::parse(kContainerType),
                                  .lab_id = lab1,
                                  .name = "9x9 vial",
                                  .size_class = "9x9",
                                  .material = "polypropylene",
                                  .supplier_sku = "SKU-9x9",
                                  .created_at = core::Timestamp::from_unix_micros(1)},
              ctx);
          txn->repo<core::ContainerType>().insert(
              core::ContainerType{.id = core::ContainerTypeId::parse(kWrongContainerType),
                                  .lab_id = lab1,
                                  .name = "50mL tube",
                                  .size_class = "tube50",
                                  .material = "polypropylene",
                                  .supplier_sku = "SKU-tube50",
                                  .created_at = core::Timestamp::from_unix_micros(1)},
              ctx);
          txn->commit();
        }
        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::BoxType>().insert(
              core::BoxType{
                  .id = core::BoxTypeId::parse("50000000-0000-0000-0000-000000000001"),
                  .lab_id = lab1,
                  .name = "9x9 cryobox",
                  .manufacturer = "Acme",
                  .sku = "ACM-9X9",
                  .positions =
                      {core::Position{.label = "A1", .row = 0, .col = 0, .accepts = {"9x9"}},
                       core::Position{.label = "A2", .row = 0, .col = 1, .accepts = {"9x9"}}},
                  .created_at = core::Timestamp::from_unix_micros(1)},
              ctx);
          txn->commit();
        }
        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::StorageContainer>().insert(
              core::StorageContainer{
                  .id = core::StorageContainerId::parse("60000000-0000-0000-0000-000000000001"),
                  .lab_id = lab1,
                  .parent_id = std::nullopt,
                  .kind = core::ContainerKind::Shelf,
                  .name = "Shelf 1",
                  .ordering_index = 0,
                  .created_at = core::Timestamp::from_unix_micros(1)},
              ctx);
          txn->commit();
        }
        {
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          txn->repo<core::Box>().insert(
              core::Box{.id = core::BoxId::parse(kBox),
                        .lab_id = lab1,
                        .box_type_id =
                            core::BoxTypeId::parse("50000000-0000-0000-0000-000000000001"),
                        .storage_container_id =
                            core::StorageContainerId::parse("60000000-0000-0000-0000-000000000001"),
                        .label = "Box-1",
                        .created_at = core::Timestamp::from_unix_micros(1)},
              ctx);
          txn->commit();
        }
      }
    };

    // =====================================================================
    // CreateSample
    // =====================================================================

    TEST_F(SampleServiceTest, CreateSampleAsAdminSucceeds) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());
      std::string id;
      const auto status = create_sample({.token = token, .name = "blood-1"}, &id);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_FALSE(id.empty());
    }

    TEST_F(SampleServiceTest, CreateSampleAsMemberSucceeds) {
      const auto token = login(kMemberEmail, kPassword);
      const auto status = create_sample({.token = token}, nullptr);
      EXPECT_TRUE(status.ok()) << status.error_message();
    }

    TEST_F(SampleServiceTest, CreateSampleRejectsReadOnly) {
      const auto token = login(kReadonlyEmail, kPassword);
      const auto status = create_sample({.token = token}, nullptr);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(SampleServiceTest, CreateSampleCrossLabRejectsOutsider) {
      const auto token = login(kOutsiderEmail, kPassword);
      const auto status = create_sample({.token = token}, nullptr);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(SampleServiceTest, CreateSampleWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::CreateSampleRequest req;
      req.set_lab_id(kLab1);
      req.set_item_type_id(kItemType);
      req.set_name("anon");
      fmgr::v1::CreateSampleResponse resp;
      const auto status = sample_stub_->CreateSample(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(SampleServiceTest, CreateSamplePlacedAtPositionSucceeds) {
      const auto token = login(kAdminEmail, kPassword);
      const auto status = create_sample(
          {.token = token, .position = "A1", .container_type = kContainerType}, nullptr);
      EXPECT_TRUE(status.ok()) << status.error_message();
    }

    TEST_F(SampleServiceTest, CreateSampleOccupiedPositionRejected) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample(
                      {.token = token, .position = "A1", .container_type = kContainerType}, nullptr)
                      .ok());
      const auto status = create_sample(
          {.token = token, .name = "dup", .position = "A1", .container_type = kContainerType},
          nullptr);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
    }

    TEST_F(SampleServiceTest, CreateSampleSizeClassMismatchRejected) {
      const auto token = login(kAdminEmail, kPassword);
      // Position A1 accepts only "9x9"; the wrong container type is "tube50".
      const auto status = create_sample(
          {.token = token, .position = "A1", .container_type = kWrongContainerType}, nullptr);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(SampleServiceTest, CreateSampleMissingRequiredCustomFieldRejected) {
      // Attach a required custom field to the item type, then omit it.
      const storage::MutationContext ctx{
          .actor_user_id = core::UserId::parse("10000000-0000-0000-0000-000000000001"),
          .actor_session_id = "seed",
          .request_id = "seed",
          .reason = "test setup",
      };
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(
            core::CustomFieldDefinition{
                .id = core::CustomFieldDefinitionId::parse("80000000-0000-0000-0000-000000000001"),
                .lab_id = core::LabId::parse(kLab1),
                .scope_kind = core::ScopeKind::Sample,
                .item_type_id = core::ItemTypeId::parse(kItemType),
                .key = "patient_ref",
                .label = "Patient Ref",
                .data_type = core::FieldDataType::String,
                .required = true,
                .created_at = core::Timestamp::from_unix_micros(1)},
            ctx);
        txn->commit();
      }
      const auto token = login(kAdminEmail, kPassword);
      const auto status = create_sample({.token = token}, nullptr); // no patient_ref
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

      // Supplying the field satisfies validation.
      const auto with_field = create_sample(
          {.token = token, .name = "with-field", .custom_fields = R"({"patient_ref":"P-1"})"},
          nullptr);
      EXPECT_TRUE(with_field.ok()) << with_field.error_message();
    }

    // =====================================================================
    // GetSample / ListSamples
    // =====================================================================

    TEST_F(SampleServiceTest, GetSampleReturnsCreated) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = token, .name = "findme"}, &id).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetSampleRequest req;
      req.set_sample_id(id);
      fmgr::v1::GetSampleResponse resp;
      ASSERT_TRUE(sample_stub_->GetSample(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.sample().name(), "findme");
      EXPECT_EQ(resp.sample().status(), fmgr::v1::SAMPLE_STATUS_ACTIVE);
    }

    // =====================================================================
    // PHI field-level encryption (M5)
    // =====================================================================

    TEST_F(SampleServiceTest, PhiFieldStoredEncryptedAtRest) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = token, .name = "phi-1", .custom_fields = R"({"mrn":"MRN-555"})"},
                        &id)
              .ok());

      // Read the row straight from storage: PHI must be ciphertext, the plaintext
      // value must appear in neither column.
      auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
      const auto row = txn->repo<core::Sample>().find_by_id(core::SampleId::parse(id));
      txn->commit();
      ASSERT_TRUE(row.has_value());
      EXPECT_NE(row->phi_fields_enc_json, "{}");
      EXPECT_EQ(row->phi_fields_enc_json.find("MRN-555"), std::string::npos);
      EXPECT_EQ(row->custom_fields_json.find("MRN-555"), std::string::npos);
      EXPECT_EQ(row->custom_fields_json.find("mrn"), std::string::npos);
    }

    TEST_F(SampleServiceTest, PhiVisibleToPhiReader) {
      const auto token = login(kAdminEmail, kPassword); // SystemAdmin + phi.read
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = token, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetSampleRequest req;
      req.set_sample_id(id);
      fmgr::v1::GetSampleResponse resp;
      ASSERT_TRUE(sample_stub_->GetSample(&ctx, req, &resp).ok());
      const auto fields = nlohmann::json::parse(resp.sample().custom_fields_json());
      EXPECT_EQ(fields.value("mrn", ""), "MRN-555");
    }

    TEST_F(SampleServiceTest, PhiHiddenFromNonReader) {
      // admin (phi.read) creates a PHI sample; member (SampleRead, no phi.read) reads it.
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, member);
      fmgr::v1::GetSampleRequest req;
      req.set_sample_id(id);
      fmgr::v1::GetSampleResponse resp;
      ASSERT_TRUE(sample_stub_->GetSample(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.sample().custom_fields_json().find("mrn"), std::string::npos);
      EXPECT_EQ(resp.sample().custom_fields_json().find("MRN-555"), std::string::npos);
    }

    TEST_F(SampleServiceTest, PhiWriteDoesNotRequirePhiRead) {
      // member holds SampleWrite but not phi.read; storing PHI must still succeed.
      const auto member = login(kMemberEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = member, .custom_fields = R"({"mrn":"MRN-777"})"}, &id).ok());

      auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
      const auto row = txn->repo<core::Sample>().find_by_id(core::SampleId::parse(id));
      txn->commit();
      ASSERT_TRUE(row.has_value());
      EXPECT_NE(row->phi_fields_enc_json, "{}");
      EXPECT_EQ(row->phi_fields_enc_json.find("MRN-777"), std::string::npos);
    }

    TEST_F(SampleServiceTest, PhiReadEmitsAuditEventWithKeysOnly) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = token, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetSampleRequest req;
      req.set_sample_id(id);
      fmgr::v1::GetSampleResponse resp;
      ASSERT_TRUE(sample_stub_->GetSample(&ctx, req, &resp).ok());

      auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
      const auto events =
          txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
      txn->commit();

      int phi_reads = 0;
      for (const auto& event : events) {
        if (event.action != "phi.read") {
          continue;
        }
        ++phi_reads;
        EXPECT_EQ(event.entity_kind, "sample");
        ASSERT_TRUE(event.entity_id.has_value());
        EXPECT_EQ(*event.entity_id, id);
        // Keys recorded, values never.
        EXPECT_NE(event.after_json.find("mrn"), std::string::npos);
        EXPECT_EQ(event.after_json.find("MRN-555"), std::string::npos);
      }
      EXPECT_EQ(phi_reads, 1);
    }

    TEST_F(SampleServiceTest, GetSampleCrossLabRejectsOutsider) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = admin, .name = "secret"}, &id).ok());

      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
      fmgr::v1::GetSampleRequest req;
      req.set_sample_id(id);
      fmgr::v1::GetSampleResponse resp;
      const auto status = sample_stub_->GetSample(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(SampleServiceTest, ListSamplesReturnsLabSamples) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample({.token = token, .name = "s1"}, nullptr).ok());
      ASSERT_TRUE(create_sample({.token = token, .name = "s2"}, nullptr).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.samples_size(), 2);
    }

    TEST_F(SampleServiceTest, ListSamplesCrossLabRejectsOutsider) {
      const auto admin = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample({.token = admin, .name = "s1"}, nullptr).ok());

      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ListSamplesResponse resp;
      const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // UpdateSample / SoftDeleteSample
    // =====================================================================

    TEST_F(SampleServiceTest, UpdateSampleRenamesAsAdmin) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = token, .name = "before"}, &id).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::UpdateSampleRequest req;
      auto* const s = req.mutable_sample();
      s->set_id(id);
      s->set_lab_id(kLab1);
      s->set_item_type_id(kItemType);
      s->set_name("after");
      fmgr::v1::UpdateSampleResponse resp;
      ASSERT_TRUE(sample_stub_->UpdateSample(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.sample().name(), "after");
    }

    TEST_F(SampleServiceTest, UpdateSampleRejectsReadOnly) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = admin, .name = "before"}, &id).ok());

      const auto readonly = login(kReadonlyEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, readonly);
      fmgr::v1::UpdateSampleRequest req;
      auto* const s = req.mutable_sample();
      s->set_id(id);
      s->set_lab_id(kLab1);
      s->set_item_type_id(kItemType);
      s->set_name("hijack");
      fmgr::v1::UpdateSampleResponse resp;
      const auto status = sample_stub_->UpdateSample(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(SampleServiceTest, SoftDeleteHidesSampleFromGet) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = token, .name = "doomed"}, &id).ok());

      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::SoftDeleteSampleRequest req;
        req.set_sample_id(id);
        fmgr::v1::SoftDeleteSampleResponse resp;
        ASSERT_TRUE(sample_stub_->SoftDeleteSample(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::GetSampleRequest req;
      req.set_sample_id(id);
      fmgr::v1::GetSampleResponse resp;
      const auto status = sample_stub_->GetSample(&ctx, req, &resp);
      EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
    }

    TEST_F(SampleServiceTest, SoftDeleteRejectsReadOnly) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = admin, .name = "doomed"}, &id).ok());

      const auto readonly = login(kReadonlyEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, readonly);
      fmgr::v1::SoftDeleteSampleRequest req;
      req.set_sample_id(id);
      fmgr::v1::SoftDeleteSampleResponse resp;
      const auto status = sample_stub_->SoftDeleteSample(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // MoveSample
    // =====================================================================

    TEST_F(SampleServiceTest, MoveSampleRelocatesToFreePosition) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = token, .position = "A1", .container_type = kContainerType}, &id)
              .ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::MoveSampleRequest req;
      req.set_sample_id(id);
      req.set_dest_box_id(kBox);
      req.set_dest_position("A2");
      fmgr::v1::MoveSampleResponse resp;
      ASSERT_TRUE(sample_stub_->MoveSample(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.sample().position_label(), "A2");
    }

    TEST_F(SampleServiceTest, MoveSampleToOccupiedPositionRejected) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id1;
      ASSERT_TRUE(
          create_sample({.token = token, .position = "A1", .container_type = kContainerType}, &id1)
              .ok());
      ASSERT_TRUE(
          create_sample(
              {.token = token, .name = "s2", .position = "A2", .container_type = kContainerType},
              nullptr)
              .ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::MoveSampleRequest req;
      req.set_sample_id(id1);
      req.set_dest_box_id(kBox);
      req.set_dest_position("A2"); // occupied
      fmgr::v1::MoveSampleResponse resp;
      const auto status = sample_stub_->MoveSample(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::ALREADY_EXISTS);
    }

    // =====================================================================
    // CheckoutSample
    // =====================================================================

    TEST_F(SampleServiceTest, CheckoutThenCheckinTransitionsStatus) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = token, .name = "vial", .volume_ul = 100}, &id).ok());

      // Check out.
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CheckoutSampleRequest req;
        req.set_sample_id(id);
        req.set_action(fmgr::v1::CHECKOUT_ACTION_CHECKOUT);
        fmgr::v1::CheckoutSampleResponse resp;
        ASSERT_TRUE(sample_stub_->CheckoutSample(&ctx, req, &resp).ok());
        EXPECT_EQ(resp.sample().status(), fmgr::v1::SAMPLE_STATUS_CHECKED_OUT);
      }
      // Check in consuming the full volume -> auto-depleted.
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CheckoutSampleRequest req;
      req.set_sample_id(id);
      req.set_action(fmgr::v1::CHECKOUT_ACTION_CHECKIN);
      req.set_volume_used(100);
      req.set_volume_unit("µL");
      fmgr::v1::CheckoutSampleResponse resp;
      ASSERT_TRUE(sample_stub_->CheckoutSample(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.sample().status(), fmgr::v1::SAMPLE_STATUS_DEPLETED);
    }

    TEST_F(SampleServiceTest, CheckoutDiscardDestroysSample) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = token, .name = "vial"}, &id).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CheckoutSampleRequest req;
      req.set_sample_id(id);
      req.set_action(fmgr::v1::CHECKOUT_ACTION_DISCARD);
      fmgr::v1::CheckoutSampleResponse resp;
      ASSERT_TRUE(sample_stub_->CheckoutSample(&ctx, req, &resp).ok());
      EXPECT_EQ(resp.sample().status(), fmgr::v1::SAMPLE_STATUS_DESTROYED);
    }

    TEST_F(SampleServiceTest, DoubleCheckoutRejected) {
      const auto token = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = token, .name = "vial"}, &id).ok());
      {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::CheckoutSampleRequest req;
        req.set_sample_id(id);
        req.set_action(fmgr::v1::CHECKOUT_ACTION_CHECKOUT);
        fmgr::v1::CheckoutSampleResponse resp;
        ASSERT_TRUE(sample_stub_->CheckoutSample(&ctx, req, &resp).ok());
      }
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::CheckoutSampleRequest req;
      req.set_sample_id(id);
      req.set_action(fmgr::v1::CHECKOUT_ACTION_CHECKOUT);
      fmgr::v1::CheckoutSampleResponse resp;
      const auto status = sample_stub_->CheckoutSample(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    }

    TEST_F(SampleServiceTest, CheckoutRejectsReadOnly) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = admin, .name = "vial"}, &id).ok());

      const auto readonly = login(kReadonlyEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, readonly);
      fmgr::v1::CheckoutSampleRequest req;
      req.set_sample_id(id);
      req.set_action(fmgr::v1::CHECKOUT_ACTION_CHECKOUT);
      fmgr::v1::CheckoutSampleResponse resp;
      const auto status = sample_stub_->CheckoutSample(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // ExportSamplesCsv
    // =====================================================================

    TEST_F(SampleServiceTest, ExportSamplesCsvReturnsHeaderAndRows) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample({.token = token, .name = "exp1"}, nullptr).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ExportSamplesCsvRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ExportSamplesCsvResponse resp;
      ASSERT_TRUE(sample_stub_->ExportSamplesCsv(&ctx, req, &resp).ok());
      EXPECT_NE(resp.csv_content().find("exp1"), std::string::npos);
      EXPECT_FALSE(resp.csv_content().empty());
    }

    // ---- ImportSamples ----

    TEST_F(SampleServiceTest, ImportSamplesAsAdminCommits) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_FALSE(token.empty());
      const std::string csv =
          "item_type_id,name\n" + kItemType + ",blood-1\n" + kItemType + ",blood-2\n";

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ImportSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_csv_content(csv);
      req.set_dry_run(false);
      fmgr::v1::ImportSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ImportSamples(&ctx, req, &resp).ok());

      EXPECT_TRUE(resp.committed());
      EXPECT_EQ(resp.succeeded(), 2);
      EXPECT_EQ(resp.failed(), 0);
      ASSERT_EQ(resp.rows_size(), 2);
      EXPECT_TRUE(resp.rows(0).ok());
      EXPECT_FALSE(resp.rows(0).sample_id().empty());

      grpc::ClientContext lctx;
      set_bearer(lctx, token);
      fmgr::v1::ListSamplesRequest lreq;
      lreq.set_lab_id(kLab1);
      fmgr::v1::ListSamplesResponse lresp;
      ASSERT_TRUE(sample_stub_->ListSamples(&lctx, lreq, &lresp).ok());
      EXPECT_EQ(lresp.samples_size(), 2);
    }

    TEST_F(SampleServiceTest, ImportSamplesDryRunDoesNotPersist) {
      const auto token = login(kAdminEmail, kPassword);
      const std::string csv =
          "item_type_id,name\n" + kItemType + ",blood-1\n" + kItemType + ",blood-2\n";

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ImportSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_csv_content(csv);
      req.set_dry_run(true);
      fmgr::v1::ImportSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ImportSamples(&ctx, req, &resp).ok());
      EXPECT_FALSE(resp.committed());
      EXPECT_EQ(resp.succeeded(), 2);

      grpc::ClientContext lctx;
      set_bearer(lctx, token);
      fmgr::v1::ListSamplesRequest lreq;
      lreq.set_lab_id(kLab1);
      fmgr::v1::ListSamplesResponse lresp;
      ASSERT_TRUE(sample_stub_->ListSamples(&lctx, lreq, &lresp).ok());
      EXPECT_EQ(lresp.samples_size(), 0);
    }

    TEST_F(SampleServiceTest, ImportSamplesMalformedRowReportedNothingPersisted) {
      const auto token = login(kAdminEmail, kPassword);
      // Second row is missing the required name → structural failure.
      const std::string csv = "item_type_id,name\n" + kItemType + ",ok-1\n" + kItemType + ",\n";

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ImportSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_csv_content(csv);
      req.set_dry_run(false);
      fmgr::v1::ImportSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ImportSamples(&ctx, req, &resp).ok());
      EXPECT_FALSE(resp.committed());
      EXPECT_GE(resp.failed(), 1);
      ASSERT_EQ(resp.rows_size(), 2);
      EXPECT_FALSE(resp.rows(1).ok());
      EXPECT_FALSE(resp.rows(1).error().empty());

      // All-or-nothing: a structural failure persists nothing.
      grpc::ClientContext lctx;
      set_bearer(lctx, token);
      fmgr::v1::ListSamplesRequest lreq;
      lreq.set_lab_id(kLab1);
      fmgr::v1::ListSamplesResponse lresp;
      ASSERT_TRUE(sample_stub_->ListSamples(&lctx, lreq, &lresp).ok());
      EXPECT_EQ(lresp.samples_size(), 0);
    }

    TEST_F(SampleServiceTest, ImportSamplesDryRunSurfacesBadItemType) {
      const auto token = login(kAdminEmail, kPassword);
      // Well-formed but non-existent item-type UUID: passes structural validation,
      // fails the dry-run DB probe (foreign key).
      const std::string kGhostItemType{"30000000-0000-0000-0000-0000000000ff"};
      const std::string csv = "item_type_id,name\n" + kGhostItemType + ",blood-1\n";

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ImportSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_csv_content(csv);
      req.set_dry_run(true);
      fmgr::v1::ImportSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ImportSamples(&ctx, req, &resp).ok());
      EXPECT_FALSE(resp.committed());
      ASSERT_EQ(resp.rows_size(), 1);
      EXPECT_FALSE(resp.rows(0).ok());
    }

    TEST_F(SampleServiceTest, ImportSamplesRejectsReadOnly) {
      const auto token = login(kReadonlyEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ImportSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_csv_content("item_type_id,name\n" + kItemType + ",x\n");
      fmgr::v1::ImportSamplesResponse resp;
      const auto status = sample_stub_->ImportSamples(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(SampleServiceTest, ImportSamplesCrossLabRejectsOutsider) {
      const auto token = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ImportSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_csv_content("item_type_id,name\n" + kItemType + ",x\n");
      fmgr::v1::ImportSamplesResponse resp;
      const auto status = sample_stub_->ImportSamples(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    TEST_F(SampleServiceTest, ImportSamplesWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      fmgr::v1::ImportSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_csv_content("item_type_id,name\n" + kItemType + ",x\n");
      fmgr::v1::ImportSamplesResponse resp;
      const auto status = sample_stub_->ImportSamples(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

    TEST_F(SampleServiceTest, ExportSamplesCsvCrossLabRejectsOutsider) {
      const auto outsider = login(kOutsiderEmail, kPassword);
      grpc::ClientContext ctx;
      set_bearer(ctx, outsider);
      fmgr::v1::ExportSamplesCsvRequest req;
      req.set_lab_id(kLab1);
      fmgr::v1::ExportSamplesCsvResponse resp;
      const auto status = sample_stub_->ExportSamplesCsv(&ctx, req, &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // =====================================================================
    // WatchSampleList (server-streaming live feed)
    // =====================================================================

    [[nodiscard]] std::int64_t now_micros() {
      return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    // A Member (holds sample.read, not audit.read) can open the feed; a sample
    // created after the stream opens is streamed live. This is the property the
    // audit feed cannot provide for ordinary members.
    TEST_F(SampleServiceTest, WatchSampleListStreamsNewSampleForMember) {
      const auto token = login(kMemberEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));
      fmgr::v1::WatchSampleListRequest req;
      req.set_lab_id(kLab1);
      req.mutable_since()->set_unix_micros(now_micros());
      auto reader = sample_stub_->WatchSampleList(&ctx, req);

      std::thread creator([this, &token] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string id;
        ASSERT_TRUE(create_sample({.token = token, .name = "watch-marker"}, &id).ok());
      });

      fmgr::v1::Sample sample;
      const bool got = reader->Read(&sample);
      creator.join();

      ASSERT_TRUE(got) << "no sample streamed within deadline";
      EXPECT_EQ(sample.lab_id(), kLab1);
      EXPECT_EQ(sample.name(), "watch-marker");

      ctx.TryCancel();
      reader->Finish();
    }

    // A soft-delete propagates as a SAMPLE_STATUS_TOMBSTONED row so the client
    // can remove it from a live view.
    TEST_F(SampleServiceTest, WatchSampleListStreamsTombstoneOnSoftDelete) {
      const auto token = login(kMemberEmail, kPassword);
      ASSERT_FALSE(token.empty());
      std::string id;
      ASSERT_TRUE(create_sample({.token = token, .name = "to-delete"}, &id).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));
      fmgr::v1::WatchSampleListRequest req;
      req.set_lab_id(kLab1);
      req.mutable_since()->set_unix_micros(now_micros());
      auto reader = sample_stub_->WatchSampleList(&ctx, req);

      std::thread deleter([this, &token, &id] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        grpc::ClientContext del_ctx;
        set_bearer(del_ctx, token);
        fmgr::v1::SoftDeleteSampleRequest del_req;
        del_req.set_sample_id(id);
        fmgr::v1::SoftDeleteSampleResponse del_resp;
        ASSERT_TRUE(sample_stub_->SoftDeleteSample(&del_ctx, del_req, &del_resp).ok());
      });

      fmgr::v1::Sample sample;
      const bool got = reader->Read(&sample);
      deleter.join();

      ASSERT_TRUE(got) << "no tombstone streamed within deadline";
      EXPECT_EQ(sample.id(), id);
      EXPECT_EQ(sample.status(), fmgr::v1::SAMPLE_STATUS_TOMBSTONED);

      ctx.TryCancel();
      reader->Finish();
    }

    // The box_id filter narrows the feed: an unplaced sample created after the
    // stream opens is excluded; only the sample placed in the watched box flows.
    TEST_F(SampleServiceTest, WatchSampleListFiltersByBox) {
      const auto token = login(kMemberEmail, kPassword);
      ASSERT_FALSE(token.empty());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));
      fmgr::v1::WatchSampleListRequest req;
      req.set_lab_id(kLab1);
      req.set_box_id(kBox);
      req.mutable_since()->set_unix_micros(now_micros());
      auto reader = sample_stub_->WatchSampleList(&ctx, req);

      std::thread creator([this, &token] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string unplaced_id;
        ASSERT_TRUE(create_sample({.token = token, .name = "unplaced"}, &unplaced_id).ok());
        std::string placed_id;
        ASSERT_TRUE(create_sample({.token = token,
                                   .name = "placed",
                                   .position = "A1",
                                   .container_type = kContainerType},
                                  &placed_id)
                        .ok());
      });

      fmgr::v1::Sample sample;
      const bool got = reader->Read(&sample);
      creator.join();

      ASSERT_TRUE(got) << "no in-box sample streamed within deadline";
      EXPECT_EQ(sample.name(), "placed");
      EXPECT_EQ(sample.box_id(), kBox);

      ctx.TryCancel();
      reader->Finish();
    }

    // A SystemAdmin of lab2 holds nothing for lab1; the cross-lab feed is denied
    // at stream-open.
    TEST_F(SampleServiceTest, WatchSampleListRejectsOutsider) {
      const auto token = login(kOutsiderEmail, kPassword);
      ASSERT_FALSE(token.empty());
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      fmgr::v1::WatchSampleListRequest req;
      req.set_lab_id(kLab1);
      auto reader = sample_stub_->WatchSampleList(&ctx, req);

      fmgr::v1::Sample sample;
      EXPECT_FALSE(reader->Read(&sample));
      EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }

    // No bearer → unauthenticated, even before any sample would stream.
    TEST_F(SampleServiceTest, WatchSampleListWithoutBearerIsUnauthenticated) {
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
      fmgr::v1::WatchSampleListRequest req;
      req.set_lab_id(kLab1);
      auto reader = sample_stub_->WatchSampleList(&ctx, req);

      fmgr::v1::Sample sample;
      EXPECT_FALSE(reader->Read(&sample));
      EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }

  } // namespace
} // namespace fmgr::test
