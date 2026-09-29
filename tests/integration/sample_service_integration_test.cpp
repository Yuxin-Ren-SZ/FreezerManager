// SPDX-License-Identifier: AGPL-3.0-or-later

#include "auth/LocalAuthProvider.h"
#include "core/audit_event.h"
#include "core/box.h"
#include "core/freezer.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/role.h"
#include "core/sample.h"
#include "crypto/FieldCipher.h"
#include "kms/EnvVarKms.h"
#include "kms/KmsFactory.h"
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
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
        std::string barcode{};        // empty = none
      };
      grpc::Status create_sample(const CreateArgs& args, std::string* out_id) {
        grpc::ClientContext ctx;
        set_bearer(ctx, args.token);
        fmgr::v1::CreateSampleRequest req;
        req.set_lab_id(args.lab.empty() ? kLab1 : args.lab);
        req.set_item_type_id(kItemType);
        req.set_name(args.name);
        if (!args.barcode.empty()) {
          req.set_barcode(args.barcode);
        }
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

      // Edit a lab-1 sample as the given principal, sending only what a client
      // that read the record would have to send back: identity, name and the
      // custom-field blob. `custom_fields` is what GetSample returned, verbatim,
      // so a caller without phi.read can only ever echo non-PHI keys.
      struct UpdateArgs {
        std::string token;
        std::string id;
        std::string name{"specimen"};
        std::string custom_fields{"{}"};
      };
      grpc::Status update_sample(const UpdateArgs& args,
                                 fmgr::v1::UpdateSampleResponse* out = nullptr) {
        grpc::ClientContext ctx;
        set_bearer(ctx, args.token);
        fmgr::v1::UpdateSampleRequest req;
        auto* const sample = req.mutable_sample();
        sample->set_id(args.id);
        sample->set_lab_id(kLab1);
        sample->set_item_type_id(kItemType);
        sample->set_name(args.name);
        sample->set_custom_fields_json(args.custom_fields);
        fmgr::v1::UpdateSampleResponse resp;
        const auto status = sample_stub_->UpdateSample(&ctx, req, &resp);
        if (out != nullptr) {
          *out = resp;
        }
        return status;
      }

      // The raw `phi_fields_enc_json` column as stored, or nullopt if the row is
      // gone. Read straight from storage, so assertions cannot be fooled by the
      // read path's disclosure rules.
      [[nodiscard]] std::optional<std::string> stored_phi_envelope(const std::string& sample_id) {
        auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
        const auto row = txn->repo<core::Sample>().find_by_id(core::SampleId::parse(sample_id));
        txn->commit();
        if (!row.has_value()) {
          return std::nullopt;
        }
        return row->phi_fields_enc_json;
      }

      // The stored PHI, decrypted with the same dev KEK the server loaded from
      // FMGR_MASTER_KEK. Empty when the row holds no PHI.
      [[nodiscard]] crypto::PhiFields stored_phi(const std::string& sample_id) {
        const auto envelope = stored_phi_envelope(sample_id);
        if (!envelope.has_value()) {
          return {};
        }
        return crypto::decrypt(*envelope, kms::EnvVarKms::from_base64(kMasterKek));
      }

      // The stored name, read straight from storage. Used where the read path
      // cannot answer: a phi.read holder's GetSample returns INTERNAL while the
      // row's envelope is unreadable.
      [[nodiscard]] std::optional<std::string> stored_name(const std::string& sample_id) {
        auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
        const auto row = txn->repo<core::Sample>().find_by_id(core::SampleId::parse(sample_id));
        txn->commit();
        if (!row.has_value()) {
          return std::nullopt;
        }
        return row->name;
      }

      // Overwrite a sample's stored PHI envelope straight through storage, so a
      // test can plant the state the read path already treats as broken: an
      // envelope whose wrapped DEK names a KEK this server does not hold.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      void put_stored_phi_envelope(const std::string& sample_id, const std::string& envelope) {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        const auto row = txn->repo<core::Sample>().find_by_id(core::SampleId::parse(sample_id));
        ASSERT_TRUE(row.has_value());
        auto planted = *row;
        planted.phi_fields_enc_json = envelope;
        txn->repo<core::Sample>().update(planted, direct_write_ctx());
        txn->commit();
      }

      // A PHI envelope this server cannot open: the same synthetic value the rest
      // of this file uses, sealed under a KEK the fixture's server was never given
      // — what a rotation that ran without `freezerctl key rotate` leaves behind.
      [[nodiscard]] static std::string orphan_phi_envelope() {
        const kms::EnvVarKms unknown_kek{std::vector<std::uint8_t>(32, 0xAB)};
        return crypto::encrypt(crypto::PhiFields{{"mrn", "MRN-555"}}, unknown_kek);
      }

      // Read one sample back over gRPC as the given principal.
      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      grpc::Status get_sample(const std::string& token, const std::string& sample_id,
                              fmgr::v1::Sample* out) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::GetSampleRequest req;
        req.set_sample_id(sample_id);
        fmgr::v1::GetSampleResponse resp;
        const auto status = sample_stub_->GetSample(&ctx, req, &resp);
        if (status.ok() && out != nullptr) {
          *out = resp.sample();
        }
        return status;
      }

      // The custom fields a response carries, as JSON.
      [[nodiscard]] static nlohmann::json custom_fields(const fmgr::v1::Sample& sample) {
        return nlohmann::json::parse(sample.custom_fields_json());
      }

      // RFC 4180 quoting (doubled inner quotes) around one cell.
      [[nodiscard]] static std::string csv_quote(const std::string& cell) {
        std::string quoted = "\"";
        for (const char chr : cell) {
          if (chr == '"') {
            quoted += '"';
          }
          quoted += chr;
        }
        quoted += '"';
        return quoted;
      }

      // One-row import CSV whose custom_fields_json cell holds `fields` verbatim.
      [[nodiscard]] std::string import_csv_with_custom_fields(const std::string& fields) const {
        return "item_type_id,name,custom_fields_json\n" + kItemType + ",phi-import," +
               csv_quote(fields) + "\n";
      }

      // Run one ImportSamples call. Returns the gRPC status; `resp` always holds
      // the body so a caller can assert on header_error/rows. `dry_run` sits
      // between the two strings so neither pair is adjacent: clang-tidy's
      // bugprone-easily-swappable-parameters is enforced on this file.
      grpc::Status import_csv(const std::string& token, bool dry_run, const std::string& csv,
                              fmgr::v1::ImportSamplesResponse* resp) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ImportSamplesRequest req;
        req.set_lab_id(kLab1);
        req.set_csv_content(csv);
        req.set_dry_run(dry_run);
        return sample_stub_->ImportSamples(&ctx, req, resp);
      }

      // The whole stored row, read straight from storage so an assertion cannot
      // be satisfied or defeated by the read path's disclosure rules.
      [[nodiscard]] std::optional<core::Sample> stored_row(const std::string& sample_id) {
        auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
        const auto row = txn->repo<core::Sample>().find_by_id(core::SampleId::parse(sample_id));
        txn->commit();
        return row;
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

      // MutationContext for rows a test writes straight through storage, bypassing
      // the services: seeding, and planting state the services would never write
      // themselves (e.g. an envelope wrapped under a KEK this server cannot open).
      [[nodiscard]] static storage::MutationContext direct_write_ctx() {
        return storage::MutationContext{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "test setup",
        };
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
        const storage::MutationContext ctx = direct_write_ctx();

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
          // PHI-tagged (is_phi) custom fields, none required, on the seeded item
          // type. Separate transaction: the repository validates item_type_id
          // against the committed DB, not the staging map.
          //
          // Three of them, one per type that matters here: `mrn` (String) is the
          // usual fixture; `age_years` (Int) and `consent_flag` (Bool) exist so a
          // test can pin that `0` and `false` count as PHI *values* and not as
          // blanks (#83 review F2) — with only a String field, reclassifying them
          // as blanks would have gone unnoticed.
          auto txn = backend_->begin(storage::IsolationLevel::Serializable);
          const auto add_phi_field = [this, &txn,
                                      &ctx](const std::string& id, const std::string& key,
                                            const std::string& label, core::FieldDataType type) {
            txn->repo<core::CustomFieldDefinition>().insert(
                core::CustomFieldDefinition{.id = core::CustomFieldDefinitionId::parse(id),
                                            .lab_id = core::LabId::parse(kLab1),
                                            .scope_kind = core::ScopeKind::Sample,
                                            .item_type_id = core::ItemTypeId::parse(kItemType),
                                            .key = key,
                                            .label = label,
                                            .data_type = type,
                                            .required = false,
                                            .is_phi = true,
                                            .created_at = core::Timestamp::from_unix_micros(1)},
                ctx);
          };
          add_phi_field("80000000-0000-0000-0000-0000000000ff", "mrn", "Medical Record Number",
                        core::FieldDataType::String);
          add_phi_field("80000000-0000-0000-0000-0000000000a1", "age_years", "Age (years)",
                        core::FieldDataType::Int);
          add_phi_field("80000000-0000-0000-0000-0000000000b1", "consent_flag", "Consent given",
                        core::FieldDataType::Bool);
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

    // A caller without phi.read never receives the PHI fields (GetSample leaves
    // them out of custom_fields_json), so its update request cannot mention
    // them. "The caller sent no PHI" must not be read as "the sample has no
    // PHI": the stored envelope survives such an edit untouched.
    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderPreservesStoredPhi) {
      const auto admin = login(kAdminEmail, kPassword); // SystemAdmin + phi.read
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .name = "before", .custom_fields = R"({"mrn":"MRN-555"})"},
                        &id)
              .ok());

      // member holds SampleWrite but not phi.read.
      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      fmgr::v1::Sample seen;
      ASSERT_TRUE(get_sample(member, id, &seen).ok());
      ASSERT_EQ(seen.name(), "before");

      // Edit an unrelated field, sending back exactly the custom fields the
      // member was shown — nothing else is possible for this caller.
      fmgr::v1::UpdateSampleResponse updated;
      const auto status = update_sample({.token = member,
                                         .id = id,
                                         .name = "renamed",
                                         .custom_fields = seen.custom_fields_json()},
                                        &updated);
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(updated.sample().name(), "renamed");
      EXPECT_EQ(updated.sample().custom_fields_json().find("mrn"), std::string::npos);

      // The row still carries the original envelope, ciphertext intact.
      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_NE(*envelope, "{}");
      EXPECT_EQ(envelope->find("MRN-555"), std::string::npos);
      EXPECT_EQ(stored_phi(id).at("mrn"), "MRN-555");

      // And the phi.read holder still sees the value after the foreign edit.
      fmgr::v1::Sample reread;
      ASSERT_TRUE(get_sample(admin, id, &reread).ok());
      EXPECT_EQ(custom_fields(reread).value("mrn", ""), "MRN-555");
      EXPECT_EQ(reread.name(), "renamed");
    }

    // An empty PHI value is not a write, it is an erasure. A caller without
    // phi.read never saw the stored value, so it has no basis to intend one;
    // key membership alone called this a supplied PHI key and let it through
    // (validation treats "" as present — see custom_field_validator.h). The
    // stored value must survive, exactly as when the key is absent.
    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderWithEmptyPhiValuePreservesStoredPhi) {
      const auto admin = login(kAdminEmail, kPassword); // SystemAdmin + phi.read
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      // member holds SampleWrite but not phi.read.
      const auto member = login(kMemberEmail, kPassword);
      const auto status = update_sample(
          {.token = member, .id = id, .name = "blanked", .custom_fields = R"({"mrn":""})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_NE(*envelope, "{}");
      EXPECT_EQ(stored_phi(id).at("mrn"), "MRN-555");

      fmgr::v1::Sample reread;
      ASSERT_TRUE(get_sample(admin, id, &reread).ok());
      EXPECT_EQ(custom_fields(reread).value("mrn", ""), "MRN-555");
    }

    // JSON null is the same erasure by another spelling: the validator reads it
    // as "not present", so nothing rejects it, but the split loop still files it
    // under the PHI keys.
    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderWithNullPhiValuePreservesStoredPhi) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      const auto member = login(kMemberEmail, kPassword);
      const auto status = update_sample(
          {.token = member, .id = id, .name = "nulled", .custom_fields = R"({"mrn":null})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_NE(*envelope, "{}");
      EXPECT_EQ(stored_phi(id).at("mrn"), "MRN-555");

      fmgr::v1::Sample reread;
      ASSERT_TRUE(get_sample(admin, id, &reread).ok());
      EXPECT_EQ(custom_fields(reread).value("mrn", ""), "MRN-555");
    }

    // The complement, and the line the empty-value rule must not cross: a
    // non-reader that supplies a real value is still writing PHI, which has
    // never required phi.read. The guard keys off "a value was supplied", not
    // off "the caller holds phi.read", so this is honored — and because this
    // caller saw only the keys it names, the value is merged per key rather than
    // answering for the whole envelope. Single-key case here; the case where the
    // difference bites is UpdateSampleByNonPhiReaderSupplyingOnePhiKeyKeepsThe-
    // OtherKeys below.
    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderWithNonEmptyPhiValueMergesIntoStoredPhi) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      const auto member = login(kMemberEmail, kPassword);
      const auto status = update_sample({.token = member,
                                         .id = id,
                                         .name = "rewritten",
                                         .custom_fields = R"({"mrn":"MRN-777"})"});
      ASSERT_TRUE(status.ok()) << status.error_message();
      EXPECT_EQ(stored_phi(id).at("mrn"), "MRN-777");
    }

    // The decisive case for #83. A caller without phi.read is shown no PHI key,
    // yet may still *name* one — the item type's field definitions are not secret
    // — and PHI write has never required phi.read (#71), so the value it supplies
    // is stored. Only that one, though: the keys it was never shown must survive.
    // Recomputing the whole envelope from the request destroyed them silently,
    // and the destruction was invisible to any test that asserted on the supplied
    // key alone. Note the two-key assertion below; that is the point of the test.
    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderSupplyingOnePhiKeyKeepsTheOtherKeys) {
      const auto admin = login(kAdminEmail, kPassword); // SystemAdmin + phi.read
      std::string id;
      ASSERT_TRUE(create_sample(
                      {.token = admin,
                       .name = "before",
                       .custom_fields = R"({"mrn":"MRN-555","age_years":41,"consent_flag":true})"},
                      &id)
                      .ok());
      ASSERT_EQ(stored_phi(id).size(), 3U); // three PHI keys stored, none of them shown

      const auto member = login(kMemberEmail, kPassword); // SampleWrite, no phi.read
      fmgr::v1::Sample seen;
      ASSERT_TRUE(get_sample(member, id, &seen).ok());
      EXPECT_EQ(seen.custom_fields_json().find("mrn"), std::string::npos); // nothing to echo

      const auto status = update_sample(
          {.token = member, .id = id, .name = "renamed", .custom_fields = R"({"mrn":"MRN-777"})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      // Both keys, from storage: the supplied one and the two that were invisible.
      const auto after = stored_phi(id);
      ASSERT_TRUE(after.contains("mrn"));
      ASSERT_TRUE(after.contains("age_years"));
      ASSERT_TRUE(after.contains("consent_flag"));
      EXPECT_EQ(after.at("mrn"), "MRN-777");     // what the member supplied
      EXPECT_EQ(after.at("age_years"), 41);      // what it never saw
      EXPECT_EQ(after.at("consent_flag"), true); // and the same for the Bool

      // And the phi.read holder still reads all three back after the foreign edit.
      fmgr::v1::Sample reread;
      ASSERT_TRUE(get_sample(admin, id, &reread).ok());
      const auto fields = custom_fields(reread);
      EXPECT_EQ(fields.value("mrn", ""), "MRN-777");
      EXPECT_EQ(fields.value("age_years", 0), 41);
      EXPECT_EQ(fields.value("consent_flag", false), true);
      EXPECT_EQ(reread.name(), "renamed");
    }

    // A blank is not a write (#79), and that has to hold *inside* the merge too:
    // a non-reader supplying one real value while blanking a different unseen key
    // must not blank that key. Same destruction, second route — key membership
    // plus a value elsewhere in the blob.
    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderBlankForAnotherPhiKeyDoesNotEraseIt) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample(
                      {.token = admin, .custom_fields = R"({"mrn":"MRN-555","age_years":41})"}, &id)
                      .ok());

      const auto member = login(kMemberEmail, kPassword);
      const auto status = update_sample({.token = member,
                                         .id = id,
                                         .name = "mixed",
                                         .custom_fields = R"({"age_years":9,"mrn":""})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      const auto after = stored_phi(id);
      ASSERT_TRUE(after.contains("mrn"));
      ASSERT_TRUE(after.contains("age_years"));
      EXPECT_EQ(after.at("mrn"), "MRN-555"); // the blank was ignored, not written
      EXPECT_EQ(after.at("age_years"), 9);   // the supplied value was stored
    }

    // F2 (#82 review): `0` and `false` are PHI *values*, not blanks — only JSON
    // null and "" are blanks (is_blank_phi_value). Nothing pinned that claim: the
    // fixture's only PHI field was a String, so a change that reclassified 0 or
    // false as blank would have silently dropped a non-reader's write with no
    // test going red. Int and Bool PHI fields now exist in the fixture for this.
    // is_number_integer()/is_boolean() also pin FieldCipher's dump()/parse
    // round-trip: a value that came back as a string would be a different bug.
    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderSupplyingZeroForIntPhiFieldStoresIt) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      const auto member = login(kMemberEmail, kPassword);
      const auto status = update_sample(
          {.token = member, .id = id, .name = "aged", .custom_fields = R"({"age_years":0})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      const auto after = stored_phi(id);
      ASSERT_TRUE(after.contains("age_years"));
      EXPECT_TRUE(after.at("age_years").is_number_integer());
      EXPECT_EQ(after.at("age_years").get<std::int64_t>(), 0);
      EXPECT_EQ(after.at("mrn"), "MRN-555"); // `0` is a write, and the merge kept the rest
    }

    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderSupplyingFalseForBoolPhiFieldStoresIt) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      const auto member = login(kMemberEmail, kPassword);
      const auto status = update_sample({.token = member,
                                         .id = id,
                                         .name = "declined",
                                         .custom_fields = R"({"consent_flag":false})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      const auto after = stored_phi(id);
      ASSERT_TRUE(after.contains("consent_flag"));
      EXPECT_TRUE(after.at("consent_flag").is_boolean());
      EXPECT_FALSE(after.at("consent_flag").get<bool>());
      EXPECT_EQ(after.at("mrn"), "MRN-555"); // `false` is a write too
    }

    // F4 (#82 review): "holds phi.read" is not "saw the values". An envelope this
    // server cannot open — its wrapping KEK is gone before `freezerctl key rotate`
    // re-wrapped it — makes GetSample return INTERNAL, so a holder learns nothing
    // from it. Treating that holder's request as authoritative rewrote the
    // envelope to {} and destroyed ciphertext that key rotate could still have
    // recovered. The condition now depends on the decryption succeeding; when it
    // does not, the request fails and nothing at all is written. (The non-PHI half
    // of the same request is checked too: the whole update rolls back, so the name
    // does not change either.)
    TEST_F(SampleServiceTest, UpdateSampleByPhiReaderWithUndecryptableEnvelopeFailsWithoutWriting) {
      const auto admin = login(kAdminEmail, kPassword); // SystemAdmin + phi.read
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .name = "before", .custom_fields = R"({"mrn":"MRN-555"})"},
                        &id)
              .ok());
      const std::string orphan = orphan_phi_envelope();
      put_stored_phi_envelope(id, orphan);

      // The read path already refuses this row; that is the state being modelled.
      fmgr::v1::Sample unreadable;
      EXPECT_EQ(get_sample(admin, id, &unreadable).error_code(), grpc::StatusCode::INTERNAL);

      // An unrelated edit that does not mention PHI at all.
      const auto status =
          update_sample({.token = admin, .id = id, .name = "renamed", .custom_fields = R"({})"});
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL) << status.error_message();
      // The failure detail is not where PHI may appear (AGENTS.md §5).
      EXPECT_EQ(status.error_message().find("MRN-555"), std::string::npos);

      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_EQ(*envelope, orphan); // byte-identical: not rewritten, not emptied
      EXPECT_EQ(stored_name(id), "before");
    }

    // The same unreadable state reached through the merge path: a non-reader's
    // supplied value has to be merged with the stored keys, so the stored keys
    // must be recoverable. They are not — so the request fails whole rather than
    // writing the caller's key over an envelope that still held the others.
    TEST_F(SampleServiceTest,
           UpdateSampleByNonPhiReaderWithUndecryptableEnvelopeFailsWithoutWriting) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .name = "before", .custom_fields = R"({"mrn":"MRN-555"})"},
                        &id)
              .ok());
      const std::string orphan = orphan_phi_envelope();
      put_stored_phi_envelope(id, orphan);

      const auto member = login(kMemberEmail, kPassword);
      const auto status = update_sample(
          {.token = member, .id = id, .name = "renamed", .custom_fields = R"({"age_years":7})"});
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL) << status.error_message();
      EXPECT_EQ(status.error_message().find("MRN-555"), std::string::npos);

      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_EQ(*envelope, orphan);
      EXPECT_EQ(stored_name(id), "before");
    }

    // The boundary of the two tests above: when the request needs nothing out of
    // the envelope, there is nothing to fail about. A non-reader's request is
    // never authoritative for stored PHI (#79), so an unreadable envelope changes
    // nothing for it — the edit is honoured and the envelope is left alone.
    // Failing this edit would be an availability regression with no security gain.
    TEST_F(SampleServiceTest,
           UpdateSampleByNonPhiReaderWithUndecryptableEnvelopeKeepsEnvelopeOnUnrelatedEdit) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .name = "before", .custom_fields = R"({"mrn":"MRN-555"})"},
                        &id)
              .ok());
      const std::string orphan = orphan_phi_envelope();
      put_stored_phi_envelope(id, orphan);

      const auto member = login(kMemberEmail, kPassword);
      const auto status =
          update_sample({.token = member, .id = id, .name = "renamed", .custom_fields = R"({})"});
      EXPECT_TRUE(status.ok()) << status.error_message();

      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_EQ(*envelope, orphan);
      EXPECT_EQ(stored_name(id), "renamed");
    }

    // The flip side of the empty-value rule: a phi.read holder *did* see the
    // stored value, so its explicit empty value is a deliberate clear and is
    // still honored. Only the "never saw it" case is refused.
    TEST_F(SampleServiceTest, UpdateSampleByPhiReaderWithEmptyPhiValueClearsStoredPhi) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      const auto status = update_sample(
          {.token = admin, .id = id, .name = "cleared", .custom_fields = R"({"mrn":""})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_NE(*envelope, "{}"); // the holder's request was honored, not ignored
      EXPECT_EQ(stored_phi(id).at("mrn"), "");

      fmgr::v1::Sample reread;
      ASSERT_TRUE(get_sample(admin, id, &reread).ok());
      EXPECT_EQ(custom_fields(reread).value("mrn", "still-set"), "");
    }

    // The phi.read holder's request *is* authoritative for PHI: it saw the
    // fields, so a new value replaces the stored one.
    TEST_F(SampleServiceTest, UpdateSampleByPhiReaderReplacesStoredPhi) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      const auto status = update_sample(
          {.token = admin, .id = id, .name = "edited", .custom_fields = R"({"mrn":"MRN-999"})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      EXPECT_EQ(stored_phi(id).at("mrn"), "MRN-999");
      fmgr::v1::Sample reread;
      ASSERT_TRUE(get_sample(admin, id, &reread).ok());
      EXPECT_EQ(custom_fields(reread).value("mrn", ""), "MRN-999");
    }

    // Deliberate clearing keeps working: a phi.read holder whose request carries
    // no PHI key at all clears the envelope, exactly as before this fix.
    TEST_F(SampleServiceTest, UpdateSampleByPhiReaderWithoutPhiKeysClearsStoredPhi) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());

      const auto status =
          update_sample({.token = admin, .id = id, .name = "cleared", .custom_fields = R"({})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_EQ(*envelope, "{}");
      fmgr::v1::Sample reread;
      ASSERT_TRUE(get_sample(admin, id, &reread).ok());
      EXPECT_EQ(reread.custom_fields_json().find("mrn"), std::string::npos);
    }

    // The complement of the preservation case: a request that *does* carry PHI
    // keys is honored whether or not the caller holds phi.read, because PHI
    // write has never required phi.read (see PhiWriteDoesNotRequirePhiRead).
    // Silently dropping supplied values would be the same class of data loss.
    TEST_F(SampleServiceTest, UpdateSampleByNonPhiReaderWhoSuppliesPhiStoresIt) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(create_sample({.token = admin, .name = "no-phi"}, &id).ok());

      const auto member = login(kMemberEmail, kPassword);
      const auto status = update_sample(
          {.token = member, .id = id, .name = "with-phi", .custom_fields = R"({"mrn":"MRN-777"})"});
      ASSERT_TRUE(status.ok()) << status.error_message();

      const auto envelope = stored_phi_envelope(id);
      ASSERT_TRUE(envelope.has_value());
      EXPECT_NE(*envelope, "{}");
      EXPECT_EQ(envelope->find("MRN-777"), std::string::npos);
      EXPECT_EQ(stored_phi(id).at("mrn"), "MRN-777");
    }

    // The same rule covers the misconfigured-server case: with no master KEK
    // wired, reveal_phi() cannot decrypt for anyone, so even a phi.read holder
    // has not seen the fields and their request must not clear them either.
    TEST_F(SampleServiceTest, UpdateSampleWithoutConfiguredKmsPreservesStoredPhi) {
      const auto admin = login(kAdminEmail, kPassword);
      std::string id;
      ASSERT_TRUE(
          create_sample({.token = admin, .custom_fields = R"({"mrn":"MRN-555"})"}, &id).ok());
      const auto before = stored_phi_envelope(id);
      ASSERT_TRUE(before.has_value());
      ASSERT_NE(*before, "{}");

      ::unsetenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
      if (kms::make_default_kms() != nullptr) {
        GTEST_SKIP() << "a master KEK is still discoverable in this environment";
      }

      // A second listener over the same database and identity provider, this
      // time with no KMS: the fixture's server keeps the key it loaded at start.
      server::FreezerServerOptions options;
      options.listen_address = "localhost:0";
      server::FreezerServer kmsless(*backend_, *provider_, options);
      kmsless.build();
      std::thread kmsless_thread([&kmsless] { kmsless.wait(); });
      const auto stop_kmsless = [&] {
        kmsless.shutdown();
        if (kmsless_thread.joinable()) {
          kmsless_thread.join();
        }
      };

      const auto kmsless_stub = fmgr::v1::SampleService::NewStub(grpc::CreateChannel(
          "localhost:" + std::to_string(kmsless.bound_port()), grpc::InsecureChannelCredentials()));
      grpc::ClientContext ctx;
      set_bearer(ctx, admin);
      fmgr::v1::UpdateSampleRequest req;
      auto* const wire = req.mutable_sample();
      wire->set_id(id);
      wire->set_lab_id(kLab1);
      wire->set_item_type_id(kItemType);
      wire->set_name("edited-without-kms");
      wire->set_custom_fields_json("{}");
      fmgr::v1::UpdateSampleResponse resp;
      const auto status = kmsless_stub->UpdateSample(&ctx, req, &resp);
      const auto after = stored_phi_envelope(id);
      stop_kmsless(); // before anything below can abort the test body

      EXPECT_TRUE(status.ok()) << status.error_message();
      ASSERT_TRUE(after.has_value());
      EXPECT_NE(*after, "{}");
      EXPECT_EQ(stored_phi(id).at("mrn"), "MRN-555");

      fmgr::v1::Sample reread;
      ASSERT_TRUE(get_sample(admin, id, &reread).ok());
      EXPECT_EQ(custom_fields(reread).value("mrn", ""), "MRN-555");
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
    // G0.4: ListSamples `query` (name/barcode search)
    // =====================================================================

    TEST_F(SampleServiceTest, ListSamplesQueryMatchesNameSubstringCaseInsensitively) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample({.token = token, .name = "Alpha-1"}, nullptr).ok());
      ASSERT_TRUE(create_sample({.token = token, .name = "beta-2"}, nullptr).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_query("PHa");
      fmgr::v1::ListSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.samples_size(), 1);
      EXPECT_EQ(resp.samples(0).name(), "Alpha-1");
    }

    TEST_F(SampleServiceTest, ListSamplesQueryMatchesBarcodeSubstring) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(
          create_sample({.token = token, .name = "unrelated", .barcode = "BC-9981"}, nullptr).ok());
      ASSERT_TRUE(
          create_sample({.token = token, .name = "other", .barcode = "BC-1234"}, nullptr).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_query("9981");
      fmgr::v1::ListSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.samples_size(), 1);
      EXPECT_EQ(resp.samples(0).barcode(), "BC-9981");
    }

    TEST_F(SampleServiceTest, ListSamplesQueryCombinesWithOtherFiltersAndPaginates) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample({.token = token, .name = "zz-1", .barcode = "B1"}, nullptr).ok());
      ASSERT_TRUE(create_sample({.token = token, .name = "zz-2", .barcode = "B2"}, nullptr).ok());
      ASSERT_TRUE(create_sample({.token = token, .name = "zz-3", .barcode = "B3"}, nullptr).ok());
      ASSERT_TRUE(create_sample({.token = token, .name = "other", .barcode = "B4"}, nullptr).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_query("ZZ");
      req.set_barcode("B2");
      fmgr::v1::ListSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.samples_size(), 1); // ANDed with the exact barcode filter
      EXPECT_EQ(resp.samples(0).name(), "zz-2");

      // Pagination: page 1 holds two of the three "zz" rows and hands back a token.
      grpc::ClientContext page1_ctx;
      set_bearer(page1_ctx, token);
      fmgr::v1::ListSamplesRequest page1_req;
      page1_req.set_lab_id(kLab1);
      page1_req.set_query("zz");
      page1_req.mutable_page()->set_page_size(2);
      fmgr::v1::ListSamplesResponse page1_resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&page1_ctx, page1_req, &page1_resp).ok());
      ASSERT_EQ(page1_resp.samples_size(), 2);
      ASSERT_FALSE(page1_resp.page().next_page_token().empty());

      grpc::ClientContext page2_ctx;
      set_bearer(page2_ctx, token);
      fmgr::v1::ListSamplesRequest page2_req;
      page2_req.set_lab_id(kLab1);
      page2_req.set_query("zz");
      page2_req.mutable_page()->set_page_size(2);
      page2_req.mutable_page()->set_page_token(page1_resp.page().next_page_token());
      fmgr::v1::ListSamplesResponse page2_resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&page2_ctx, page2_req, &page2_resp).ok());
      ASSERT_EQ(page2_resp.samples_size(), 1);
      EXPECT_EQ(page2_resp.samples(0).name(), "zz-3");
    }

    TEST_F(SampleServiceTest, ListSamplesQueryUnderTwoCharactersIsInvalidArgument) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample({.token = token, .name = "Alpha-1"}, nullptr).ok());

      for (const std::string& too_short : {std::string("A"), std::string("")}) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLab1);
        req.set_query(too_short);
        fmgr::v1::ListSamplesResponse resp;
        const auto status = sample_stub_->ListSamples(&ctx, req, &resp);
        EXPECT_FALSE(status.ok()) << "query='" << too_short << "'";
        EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      }
    }

    // The floor is two *bytes*, not two code points: a lone multi-byte character
    // is already selective (it cannot match ASCII names), so it is accepted. This
    // pins that contract rather than leaving it implied by the length check.
    TEST_F(SampleServiceTest, ListSamplesQueryAcceptsALoneMultiByteCharacter) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample({.token = token, .name = "M\xC3\xBCller"}, nullptr).ok());
      ASSERT_TRUE(create_sample({.token = token, .name = "plain"}, nullptr).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_query("\xC3\xBC"); // "ü", two bytes in UTF-8
      ASSERT_EQ(req.query().size(), 2U);
      fmgr::v1::ListSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.samples_size(), 1);
      EXPECT_EQ(resp.samples(0).name(), "M\xC3\xBCller");
    }

    TEST_F(SampleServiceTest, ListSamplesQueryEscapesLikeWildcards) {
      const auto token = login(kAdminEmail, kPassword);
      ASSERT_TRUE(create_sample({.token = token, .name = R"(50%_x)"}, nullptr).ok());
      ASSERT_TRUE(create_sample({.token = token, .name = "50abc"}, nullptr).ok());

      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_query("%_"); // literal %, not "match anything"
      fmgr::v1::ListSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.samples_size(), 1);
      EXPECT_EQ(resp.samples(0).name(), R"(50%_x)");
    }

    TEST_F(SampleServiceTest, ListSamplesQueryDoesNotSearchCustomFieldsOrPhi) {
      const auto token = login(kAdminEmail, kPassword);
      // A plain (non-PHI) sample-scoped custom field, declared before the sample
      // that uses it so the resolver sees a committed definition.
      const storage::MutationContext seed_ctx{
          .actor_user_id = core::UserId::parse("10000000-0000-0000-0000-000000000001"),
          .actor_session_id = "seed",
          .request_id = "seed",
          .reason = "test setup",
      };
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::CustomFieldDefinition>().insert(
            core::CustomFieldDefinition{
                .id = core::CustomFieldDefinitionId::parse("80000000-0000-0000-0000-0000000000fe"),
                .lab_id = core::LabId::parse(kLab1),
                .scope_kind = core::ScopeKind::Sample,
                .item_type_id = core::ItemTypeId::parse(kItemType),
                .key = "project",
                .label = "Project",
                .data_type = core::FieldDataType::String,
                .required = false,
                .is_phi = false,
                .created_at = core::Timestamp::from_unix_micros(1)},
            seed_ctx);
        txn->commit();
      }
      ASSERT_TRUE(create_sample({.token = token,
                                 .name = "plain",
                                 .custom_fields = R"({"project":"needle-xyz","mrn":"needle-abc"})"},
                                nullptr)
                      .ok());

      // The needle only exists in a custom field and in the PHI-tagged field, so
      // neither the search nor a wider query may surface the row.
      for (const std::string& needle :
           {std::string("needle"), std::string("xyz"), std::string("abc")}) {
        grpc::ClientContext ctx;
        set_bearer(ctx, token);
        fmgr::v1::ListSamplesRequest req;
        req.set_lab_id(kLab1);
        req.set_query(needle);
        fmgr::v1::ListSamplesResponse resp;
        ASSERT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok()) << needle;
        EXPECT_EQ(resp.samples_size(), 0) << "needle=" << needle;
      }

      // Sanity: the sample itself is findable through its name.
      grpc::ClientContext ctx;
      set_bearer(ctx, token);
      fmgr::v1::ListSamplesRequest req;
      req.set_lab_id(kLab1);
      req.set_query("plai");
      fmgr::v1::ListSamplesResponse resp;
      ASSERT_TRUE(sample_stub_->ListSamples(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.samples_size(), 1);
      EXPECT_EQ(resp.samples(0).name(), "plain");
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

    // A PHI-tagged key that arrives in the CSV's custom_fields_json cell must be
    // split out and encrypted exactly like CreateSample does it, never stored in
    // the plaintext column. Asserted on the stored columns, so a read path that
    // happens to hide the key cannot make this pass. The untagged key beside it
    // must stay in the plaintext column: this is a split, not "encrypt the blob".
    TEST_F(SampleServiceTest, ImportSamplesStoresPhiTaggedKeyEncryptedAtRest) {
      const auto admin = login(kAdminEmail, kPassword);
      fmgr::v1::ImportSamplesResponse resp;
      ASSERT_TRUE(import_csv(admin, false,
                             import_csv_with_custom_fields(R"({"mrn":"MRN-555","strain":"EC-1"})"),
                             &resp)
                      .ok())
          << resp.header_error();
      ASSERT_TRUE(resp.committed());
      ASSERT_EQ(resp.succeeded(), 1);
      const std::string id = resp.rows(0).sample_id();
      ASSERT_FALSE(id.empty());

      const auto row = stored_row(id);
      ASSERT_TRUE(row.has_value());
      EXPECT_EQ(row->custom_fields_json.find("MRN-555"), std::string::npos);
      EXPECT_EQ(row->custom_fields_json.find("mrn"), std::string::npos);
      EXPECT_NE(row->custom_fields_json.find("strain"), std::string::npos);
      EXPECT_NE(row->phi_fields_enc_json, "{}");
      EXPECT_EQ(row->phi_fields_enc_json.find("MRN-555"), std::string::npos);
      const auto phi = stored_phi(id);
      ASSERT_TRUE(phi.contains("mrn"));
      EXPECT_EQ(phi.at("mrn"), "MRN-555");
    }

    // The disclosure is the harm, so the assertion is written from the side that
    // must not see it: a sample.read holder without phi.read.
    TEST_F(SampleServiceTest, ImportSamplesHidesPhiTaggedKeyFromNonPhiReader) {
      const auto admin = login(kAdminEmail, kPassword);
      fmgr::v1::ImportSamplesResponse resp;
      ASSERT_TRUE(
          import_csv(admin, false, import_csv_with_custom_fields(R"({"mrn":"MRN-555"})"), &resp)
              .ok())
          << resp.header_error();
      ASSERT_TRUE(resp.committed());
      const std::string id = resp.rows(0).sample_id();
      ASSERT_FALSE(id.empty());

      const auto member = login(kMemberEmail, kPassword);
      ASSERT_FALSE(member.empty());
      fmgr::v1::Sample as_member;
      ASSERT_TRUE(get_sample(member, id, &as_member).ok());
      EXPECT_EQ(as_member.custom_fields_json().find("mrn"), std::string::npos);
      EXPECT_EQ(as_member.custom_fields_json().find("MRN-555"), std::string::npos);

      // The value must exist, not merely be invisible: the phi.read holder sees
      // it. Otherwise "hidden" would also be satisfied by having dropped it.
      fmgr::v1::Sample as_admin;
      ASSERT_TRUE(get_sample(admin, id, &as_admin).ok());
      EXPECT_EQ(custom_fields(as_admin).value("mrn", ""), "MRN-555");
    }

    // The import now validates custom fields exactly like CreateSample, because
    // it runs the same split. A known field of the wrong type is refused instead
    // of being written to the plaintext column unchecked.
    TEST_F(SampleServiceTest, ImportSamplesRejectsCustomFieldFailingValidation) {
      const auto admin = login(kAdminEmail, kPassword);
      fmgr::v1::ImportSamplesResponse resp;
      const auto status =
          import_csv(admin, false, import_csv_with_custom_fields(R"({"mrn":5})"), &resp);
      EXPECT_FALSE(status.ok());
      EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
      EXPECT_NE(status.error_message().find("mrn"), std::string::npos);

      grpc::ClientContext lctx;
      set_bearer(lctx, admin);
      fmgr::v1::ListSamplesRequest lreq;
      lreq.set_lab_id(kLab1);
      fmgr::v1::ListSamplesResponse lresp;
      ASSERT_TRUE(sample_stub_->ListSamples(&lctx, lreq, &lresp).ok());
      EXPECT_EQ(lresp.samples_size(), 0);
    }

    // ... and the dry run says so too: the probe runs the same split as the
    // commit path, so it cannot approve a row the commit will refuse. Without
    // that, a user would be told the file is good and then shown an error on the
    // row the preview passed (#110).
    TEST_F(SampleServiceTest, ImportSamplesDryRunReportsCustomFieldValidationFailure) {
      const auto admin = login(kAdminEmail, kPassword);
      fmgr::v1::ImportSamplesResponse resp;
      ASSERT_TRUE(
          import_csv(admin, true, import_csv_with_custom_fields(R"({"mrn":5})"), &resp).ok());
      EXPECT_FALSE(resp.committed());
      ASSERT_EQ(resp.rows_size(), 1);
      EXPECT_FALSE(resp.rows(0).ok());
      EXPECT_NE(resp.rows(0).error().find("mrn"), std::string::npos);
      EXPECT_EQ(resp.succeeded(), 0);
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
