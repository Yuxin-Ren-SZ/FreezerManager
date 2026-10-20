// SPDX-License-Identifier: AGPL-3.0-or-later

// Pins the demo seed's shape and the phi.read gate it is meant to demonstrate
// (#143). Unlike the repository fixtures next door, this test runs the real
// `scripts/seed_demo.py` against a fresh, migrated SQLite database (the seed
// shells out to `freezerctl sample import`, so the C++ write path is what
// creates the sample rows and their audit events). It then asserts:
//
//   - the seed exits 0 and reports the 32 samples;
//   - every seeded sample has a real `sample.insert` audit event, the chain
//     verifies, and audit/list returns the seeded sample's history;
//   - the Neuroscience lab has PHI mode on and its CSF `diagnosis` field is
//     tagged PHI, with the value encrypted at rest;
//   - both box grids (9x9 and 10x10) are in use;
//   - `admin@freezer.local` (SystemAdmin, granted phi.read by the seed) sees the
//     diagnosis while `zhang@neuro.local` (ReadOnly) and `li@neuro.local`
//     (Member) do not.
//
// The seed runs without argon2-cffi: it falls back to the precomputed hash for
// the demo password. Set FMGR_SEED_PYTHON to run the red/green check against a
// python that has argon2-cffi if desired.

#include "audit/AuditChainVerifier.h"
#include "auth/LocalAuthProvider.h"
#include "core/audit_event.h"
#include "core/identity.h"
#include "core/item_type.h"
#include "core/sample.h"
#include "server/FreezerServer.h"
#include "storage/AuditTraits.h"
#include "storage/BoxGeometryTraits.h"
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
#include "test_helpers.h"

#include <fmgr/v1/audit.grpc.pb.h>
#include <fmgr/v1/auth.grpc.pb.h>
#include <fmgr/v1/sample.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <sqlite3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fmgr::test {
  namespace {

    // base64 of 32 bytes 0x00..0x1F — a fixed dev KEK, the same one the other
    // integration tests use. The seed encrypts with it; the server decrypts.
    constexpr const char* kMasterKek = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
    constexpr const char* kNeuroLab = "00000000-0000-a001-0000-000000000001";
    constexpr const char* kAdminEmail = "admin@freezer.local";
    constexpr const char* kReadonlyEmail = "zhang@neuro.local";
    constexpr const char* kMemberEmail = "li@neuro.local";
    constexpr const char* kPassword = "admin";
    constexpr const char* kSeedSample = "P001-CSF-01";
    constexpr const char* kSeedSampleDiagnosis = "Hydrocephalus";
    constexpr std::size_t kSeedSampleCount = 32;

    [[nodiscard]] std::filesystem::path unique_db_path() {
      static std::atomic<int> counter{0};
      return std::filesystem::temp_directory_path() /
             ("fmgr-demo-seed-test-" + std::to_string(counter.fetch_add(1)) + ".db");
    }

    [[nodiscard]] auth::LocalAuthProviderConfig fast_config() {
      auth::LocalAuthProviderConfig cfg;
      cfg.pwhash_memlimit = 8192;
      cfg.pwhash_opslimit = 1;
      return cfg;
    }

    // Quote one argv element for /bin/sh. The paths under test are controlled,
    // but a checkout directory may contain spaces.
    [[nodiscard]] std::string shell_quote(const std::string& value) {
      std::string quoted = "'";
      for (const char chr : value) {
        if (chr == '\'') {
          quoted += "'\\''";
        } else {
          quoted += chr;
        }
      }
      quoted += '\'';
      return quoted;
    }

    class DemoSeedTest : public ::testing::Test {
    protected:
      void SetUp() override {
        ::setenv("FMGR_MASTER_KEK", kMasterKek, 1); // NOLINT(concurrency-mt-unsafe)
        db_path_ = unique_db_path();
        remove_sqlite_files(db_path_);

        // A migrated database is the seed's documented precondition.
        {
          storage::SqliteBackend backend(
              storage::SqliteBackendOptions{.database_path = db_path_.string()});
          register_all_repositories(backend);
          backend.migrate_to_latest();
        }

        const auto [exit_code, output] = run_seed();
        ASSERT_EQ(exit_code, 0) << output;
        seed_output_ = output;

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_path_.string()});
        register_all_repositories(*backend_);
        backend_->migrate_to_latest();
        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_config());

        server_opts_.listen_address = "localhost:0";
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, server_opts_);
        server_->build();
        server_thread_ = std::thread([this] { server_->wait(); });

        channel_ = grpc::CreateChannel("localhost:" + std::to_string(server_->bound_port()),
                                       grpc::InsecureChannelCredentials());
        auth_stub_ = fmgr::v1::AuthService::NewStub(channel_);
        sample_stub_ = fmgr::v1::SampleService::NewStub(channel_);
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
        ::unsetenv("FMGR_MASTER_KEK"); // NOLINT(concurrency-mt-unsafe)
        ::unsetenv("FREEZER_DB_PATH");
        ::unsetenv("FMGR_FREEZERCTL");
      }

      [[nodiscard]] std::pair<int, std::string> run_seed() {
        const char* python_override = std::getenv("FMGR_SEED_PYTHON"); // NOLINT
        const std::string python = (python_override != nullptr && *python_override != '\0')
                                       ? python_override
                                       : FMGR_PYTHON3;
        ::setenv("FREEZER_DB_PATH", db_path_.c_str(), 1);    // NOLINT(concurrency-mt-unsafe)
        ::setenv("FMGR_FREEZERCTL", FMGR_FREEZERCTL_BIN, 1); // NOLINT(concurrency-mt-unsafe)

        const std::string command =
            shell_quote(python) + " " +
            shell_quote(std::string(FMGR_SOURCE_DIR) + "/scripts/seed_demo.py") + " 2>&1";
        FILE* pipe = ::popen(command.c_str(), "r");
        if (pipe == nullptr) {
          return {-1, "popen failed"};
        }
        std::string output;
        std::array<char, 4096> buffer{};
        while (std::fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
          output += buffer.data();
        }
        const int status = ::pclose(pipe);
        return {status, output};
      }

      static void register_all_repositories(storage::SqliteBackend& backend) {
        storage::register_identity_repositories(backend);
        storage::register_role_repositories(backend);
        storage::register_session_repositories(backend);
        storage::register_audit_repositories(backend);
        storage::register_box_geometry_repositories(backend);
        storage::register_box_repositories(backend);
        storage::register_item_type_repositories(backend);
        storage::register_layout_repositories(backend);
        storage::register_sample_repositories(backend);
        storage::register_share_request_repositories(backend);
      }

      // One scalar read straight from the seeded file, for shape checks that
      // span tables the typed query DSL cannot join (boxes -> box types).
      [[nodiscard]] std::string scalar_sql(const std::string& sql) const {
        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(db_path_.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
          return {};
        }
        sqlite3_stmt* statement = nullptr;
        std::string value;
        if (sqlite3_prepare_v2(raw, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK) {
          if (sqlite3_step(statement) == SQLITE_ROW) {
            const auto* text = sqlite3_column_text(statement, 0);
            if (text != nullptr) {
              value = reinterpret_cast<const char*>(text);
            }
          }
        }
        sqlite3_finalize(statement);
        sqlite3_close(raw);
        return value;
      }

      [[nodiscard]] std::vector<core::Sample> samples_named(const std::string& name) {
        auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
        auto rows = txn->repo<core::Sample>().query(storage::Query<core::Sample>::where(
            storage::field<core::Sample, std::string>(core::Sample::Field::Name) == name));
        txn->commit();
        return rows;
      }

      [[nodiscard]] std::vector<core::AuditEvent> all_audit_events() {
        auto txn = backend_->begin(storage::IsolationLevel::ReadCommitted);
        auto events = txn->repo<core::AuditEvent>().query(storage::Query<core::AuditEvent>::all());
        txn->commit();
        return events;
      }

      [[nodiscard]] std::string login(const std::string& email) {
        grpc::ClientContext ctx;
        fmgr::v1::LoginRequest req;
        req.set_email(email);
        req.set_password(kPassword);
        fmgr::v1::LoginResponse resp;
        if (!auth_stub_->Login(&ctx, req, &resp).ok()) {
          return {};
        }
        return resp.session_token();
      }

      // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
      [[nodiscard]] grpc::Status get_sample(const std::string& token, const std::string& sample_id,
                                            fmgr::v1::Sample* out) {
        grpc::ClientContext ctx;
        ctx.AddMetadata("authorization", "Bearer " + token);
        fmgr::v1::GetSampleRequest req;
        req.set_sample_id(sample_id);
        fmgr::v1::GetSampleResponse resp;
        const auto status = sample_stub_->GetSample(&ctx, req, &resp);
        if (status.ok() && out != nullptr) {
          *out = resp.sample();
        }
        return status;
      }

      [[nodiscard]] static nlohmann::json custom_fields(const fmgr::v1::Sample& sample) {
        return nlohmann::json::parse(sample.custom_fields_json());
      }

      [[nodiscard]] const std::string& seed_output() const {
        return seed_output_;
      }

      std::filesystem::path db_path_;
      std::string seed_output_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;
      server::FreezerServerOptions server_opts_;
      std::shared_ptr<grpc::Channel> channel_;
      std::unique_ptr<fmgr::v1::AuthService::Stub> auth_stub_;
      std::unique_ptr<fmgr::v1::SampleService::Stub> sample_stub_;
      std::unique_ptr<fmgr::v1::AuditService::Stub> audit_stub_;
    };

    TEST_F(DemoSeedTest, SeedCompletesInOneCommandAndReportsWhatItCreated) {
      EXPECT_NE(seed_output().find("32 samples"), std::string::npos) << seed_output();
      EXPECT_NE(seed_output().find("32 audit events"), std::string::npos) << seed_output();
      EXPECT_NE(seed_output().find("9x9 Cryobox"), std::string::npos) << seed_output();
      EXPECT_NE(seed_output().find("10x10 Cryobox"), std::string::npos) << seed_output();
      EXPECT_NE(seed_output().find("OK: 32 audit event(s) verified"), std::string::npos)
          << seed_output();
    }

    TEST_F(DemoSeedTest, EverySeededSampleHasRealAuditHistory) {
      const auto events = all_audit_events();
      ASSERT_EQ(events.size(), kSeedSampleCount) << seed_output();

      const auto report = audit::verify_audit_chain(events);
      EXPECT_TRUE(report.ok);
      EXPECT_EQ(report.verified_count, kSeedSampleCount);

      const auto seeded = samples_named(kSeedSample);
      ASSERT_EQ(seeded.size(), 1U);
      const std::string sample_id = seeded.front().id.to_string();
      EXPECT_EQ(scalar_sql("SELECT count(*) FROM audit_events WHERE entity_kind = 'sample'"
                           " AND entity_id = '" +
                           sample_id + "' AND action = 'insert'"),
                "1");

      // This is the query the sample detail screen's History panel makes.
      const auto token = login(kAdminEmail);
      ASSERT_FALSE(token.empty());
      grpc::ClientContext ctx;
      ctx.AddMetadata("authorization", "Bearer " + token);
      fmgr::v1::ListAuditEventsRequest req;
      req.set_entity_kind("sample");
      req.set_entity_id(sample_id);
      fmgr::v1::ListAuditEventsResponse resp;
      ASSERT_TRUE(audit_stub_->ListAuditEvents(&ctx, req, &resp).ok());
      ASSERT_EQ(resp.events_size(), 1);
      EXPECT_EQ(resp.events(0).action(), "insert");
      EXPECT_EQ(resp.events(0).entity_id(), sample_id);
    }

    TEST_F(DemoSeedTest, SeededPhiGateShowsTheValueToAPhiReaderAndHidesItFromReadOnly) {
      EXPECT_EQ(
          scalar_sql("SELECT is_phi_enabled FROM labs WHERE id = '" + std::string(kNeuroLab) + "'"),
          "1");
      EXPECT_EQ(scalar_sql("SELECT count(*) FROM custom_field_definitions WHERE lab_id = '" +
                           std::string(kNeuroLab) + "' AND key = 'diagnosis' AND is_phi = 1"),
                "1");

      // Stored encrypted: the plaintext column has neither the key nor the value,
      // and the envelope is not the empty object.
      const auto envelope = scalar_sql("SELECT phi_fields_enc_json FROM samples WHERE name = '" +
                                       std::string(kSeedSample) + "'");
      ASSERT_FALSE(envelope.empty());
      EXPECT_NE(envelope, "{}");
      EXPECT_EQ(envelope.find(kSeedSampleDiagnosis), std::string::npos);
      EXPECT_EQ(scalar_sql("SELECT custom_fields_json FROM samples WHERE name = '" +
                           std::string(kSeedSample) + "'")
                    .find("diagnosis"),
                std::string::npos);

      const auto seeded = samples_named(kSeedSample);
      ASSERT_EQ(seeded.size(), 1U);
      const std::string sample_id = seeded.front().id.to_string();

      const auto admin = login(kAdminEmail);
      ASSERT_FALSE(admin.empty());
      fmgr::v1::Sample seen_by_admin;
      ASSERT_TRUE(get_sample(admin, sample_id, &seen_by_admin).ok());
      EXPECT_EQ(custom_fields(seen_by_admin).value("diagnosis", ""), kSeedSampleDiagnosis);

      for (const std::string& email : {std::string(kReadonlyEmail), std::string(kMemberEmail)}) {
        const auto token = login(email);
        ASSERT_FALSE(token.empty()) << email;
        fmgr::v1::Sample seen;
        ASSERT_TRUE(get_sample(token, sample_id, &seen).ok()) << email;
        const auto fields = custom_fields(seen);
        EXPECT_FALSE(fields.contains("diagnosis")) << email;
        EXPECT_EQ(seen.custom_fields_json().find(kSeedSampleDiagnosis), std::string::npos) << email;
      }
    }

    TEST_F(DemoSeedTest, SeedUsesBothBoxGrids) {
      EXPECT_GT(std::stoi(scalar_sql("SELECT count(*) FROM boxes b JOIN box_types bt"
                                     " ON bt.id = b.box_type_id WHERE bt.name = '9x9 Cryobox'")),
                0);
      EXPECT_GT(std::stoi(scalar_sql("SELECT count(*) FROM boxes b JOIN box_types bt"
                                     " ON bt.id = b.box_type_id WHERE bt.name = '10x10 Cryobox'")),
                0);
      EXPECT_EQ(scalar_sql("SELECT count(*) FROM box_type_positions p JOIN box_types bt"
                           " ON bt.id = p.box_type_id WHERE bt.name = '9x9 Cryobox'"
                           " AND bt.lab_id = '" +
                           std::string(kNeuroLab) + "'"),
                "81");
      EXPECT_EQ(scalar_sql("SELECT count(*) FROM box_type_positions p JOIN box_types bt"
                           " ON bt.id = p.box_type_id WHERE bt.name = '10x10 Cryobox'"
                           " AND bt.lab_id = '" +
                           std::string(kNeuroLab) + "'"),
                "100");
    }

  } // namespace
} // namespace fmgr::test
