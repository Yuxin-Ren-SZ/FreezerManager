// SPDX-License-Identifier: AGPL-3.0-or-later

// Round-trip tests for `freezerctl user set-password`: enrolling a local password
// must produce exactly the auth_bindings shape LocalAuthProvider::authenticate
// consumes, so a fresh authenticate() against the same email+password succeeds.

#include "cli/UserCommands.h"

#include "auth/AuthTypes.h"
#include "auth/LocalAuthProvider.h"
#include "core/identity.h"
#include "core/timestamp.h"
#include "core/uuid.h"
#include "storage/IdentityTraits.h"
#include "storage/MfaSecurityStateTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>

namespace fmgr::cli {
  namespace {

    constexpr const char* kEmail = "demo@lab.test";

    [[nodiscard]] auth::LocalAuthProviderConfig fast_config() {
      auth::LocalAuthProviderConfig cfg;
      cfg.pwhash_memlimit = 8192; // crypto_pwhash_MEMLIMIT_MIN — keep tests quick
      cfg.pwhash_opslimit = 1;    // crypto_pwhash_OPSLIMIT_MIN
      return cfg;
    }

    class UserSetPasswordTest : public ::testing::Test {
    protected:
      void SetUp() override {
        const auto seed = std::to_string(
            static_cast<unsigned long long>(::testing::UnitTest::GetInstance()->random_seed()));
        db_path_ = std::filesystem::temp_directory_path() /
                   (std::string("fmgr-setpw-") + seed + "-" +
                    ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db");
        std::filesystem::remove(db_path_);

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_path_.string()});
        storage::register_identity_repositories(*backend_);
        storage::register_role_repositories(*backend_);
        storage::register_session_repositories(*backend_);
        backend_->migrate_to_latest();

        seed_user(kEmail);
      }

      void TearDown() override {
        backend_.reset();
        std::filesystem::remove(db_path_);
      }

      void seed_user(const std::string& email) {
        const core::User user{
            .id = core::UserId::parse(core::generate_uuid_v4()),
            .primary_email = email,
            .display_name = email,
            .status = core::UserStatus::Active,
            .created_at = core::Timestamp::from_unix_micros(1'000),
        };
        const storage::MutationContext ctx{
            .actor_user_id = user.id,
            .actor_session_id = "test",
            .request_id = "",
            .reason = "seed",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::User>().insert(user, ctx);
        txn->commit();
      }

      // True iff authenticate() accepts the email+password (no TOTP → mfa_complete).
      [[nodiscard]] bool can_login(const std::string& email, const std::string& password) {
        auth::LocalAuthProvider provider(*backend_, fast_config());
        try {
          const auto token = provider.authenticate(
              auth::PasswordCredentials{.email = email, .password = password}, auth::ClientInfo{});
          return token.mfa_complete;
        } catch (const std::exception&) {
          return false;
        }
      }

      [[nodiscard]] int local_binding_count(const std::string& email) {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        const auto users = txn->repo<core::User>().query(storage::Query<core::User>::where(
            storage::field<core::User, std::string>(core::User::Field::PrimaryEmail) == email));
        txn->commit();
        if (users.empty()) {
          return -1;
        }
        int count = 0;
        for (const auto& binding : users.front().auth_bindings) {
          if (binding.contains("provider") && binding.at("provider") == "local") {
            ++count;
          }
        }
        return count;
      }

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::ostringstream out_;
      std::ostringstream err_;
    };

    TEST_F(UserSetPasswordTest, EnrolThenAuthenticateRoundTrips) {
      const int rc = run_user_set_password(*backend_, kEmail, "hunter22", std::nullopt, out_, err_);
      EXPECT_EQ(rc, 0);
      EXPECT_TRUE(can_login(kEmail, "hunter22"));
      EXPECT_EQ(local_binding_count(kEmail), 1);
    }

    TEST_F(UserSetPasswordTest, WrongPasswordRejected) {
      ASSERT_EQ(run_user_set_password(*backend_, kEmail, "hunter22", std::nullopt, out_, err_), 0);
      EXPECT_FALSE(can_login(kEmail, "guess123"));
    }

    TEST_F(UserSetPasswordTest, ReEnrolReplacesWithoutDuplicateBinding) {
      ASSERT_EQ(run_user_set_password(*backend_, kEmail, "hunter22", std::nullopt, out_, err_), 0);
      ASSERT_EQ(run_user_set_password(*backend_, kEmail, "newpass1", std::nullopt, out_, err_), 0);
      EXPECT_FALSE(can_login(kEmail, "hunter22"));
      EXPECT_TRUE(can_login(kEmail, "newpass1"));
      EXPECT_EQ(local_binding_count(kEmail), 1);
    }

    TEST_F(UserSetPasswordTest, EmailLookupIsCaseInsensitive) {
      // Stored email is lowercase; enrolling with a mixed-case argument still finds
      // it (authenticate lowercases too).
      const int rc =
          run_user_set_password(*backend_, "Demo@Lab.Test", "hunter22", std::nullopt, out_, err_);
      EXPECT_EQ(rc, 0);
      EXPECT_TRUE(can_login(kEmail, "hunter22"));
    }

    TEST_F(UserSetPasswordTest, ShortPasswordRejectedNoBinding) {
      const int rc = run_user_set_password(*backend_, kEmail, "short", std::nullopt, out_, err_);
      EXPECT_NE(rc, 0);
      EXPECT_EQ(local_binding_count(kEmail), 0);
      EXPECT_FALSE(err_.str().empty());
    }

    TEST_F(UserSetPasswordTest, UnknownEmailRejected) {
      const int rc =
          run_user_set_password(*backend_, "ghost@lab.test", "hunter22", std::nullopt, out_, err_);
      EXPECT_NE(rc, 0);
    }

    TEST_F(UserSetPasswordTest, TotpProvisioningUsesGuardedAuditedLifecycleAndNeverPrintsSecret) {
      const std::string secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
      const auto before = backend_->audit_event_count_for_tests();
      ASSERT_EQ(run_user_set_totp(*backend_, kEmail, secret, std::nullopt, out_, err_), 0);
      auto txn = backend_->begin(storage::IsolationLevel::Serializable);
      const auto users = txn->repo<core::User>().query(storage::Query<core::User>::all());
      ASSERT_EQ(users.size(), 1U);
      EXPECT_EQ(users.front().totp_secret_enc, secret);
      EXPECT_TRUE(
          txn->repo<core::MfaSecurityState>().find_by_id(users.front().id)->totp_generation);
      txn->commit();
      EXPECT_EQ(backend_->audit_event_count_for_tests(), before + 3);
      EXPECT_EQ(out_.str().find(secret), std::string::npos);
      EXPECT_EQ(err_.str().find(secret), std::string::npos);
    }

    TEST_F(UserSetPasswordTest, TotpProvisioningRejectsInvalidSecretWithoutWritingOrLeaking) {
      const std::string invalid = "PRIVATE-invalid-secret";
      const auto before = backend_->audit_event_count_for_tests();
      EXPECT_EQ(run_user_set_totp(*backend_, kEmail, invalid, std::nullopt, out_, err_), 1);
      EXPECT_EQ(backend_->audit_event_count_for_tests(), before);
      EXPECT_EQ(err_.str().find(invalid), std::string::npos);
      EXPECT_EQ(out_.str().find(invalid), std::string::npos);
    }

    TEST(TotpSecretInput, RequiresRegularOwnerOnlyFileAndRejectsSymlinksAndFifos) {
      const auto path =
          std::filesystem::temp_directory_path() / ("fmgr-totp-secret-" + core::generate_uuid_v4());
      const auto link = std::filesystem::path(path.string() + ".link");
      const auto fifo = std::filesystem::path(path.string() + ".fifo");
      {
        std::ofstream file(path);
        file << "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n";
      }
      std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write);
      std::istringstream unused;
      EXPECT_EQ(read_totp_secret_input(path.string(), unused), "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
      std::filesystem::create_symlink(path, link);
      EXPECT_THROW((void)read_totp_secret_input(link.string(), unused), std::runtime_error);
      std::filesystem::permissions(path, std::filesystem::perms::group_read,
                                   std::filesystem::perm_options::add);
      EXPECT_THROW((void)read_totp_secret_input(path.string(), unused), std::runtime_error);
      std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write);
      {
        std::ofstream file(path);
        file << "private\nextra\n";
      }
      EXPECT_THROW((void)read_totp_secret_input(path.string(), unused), std::runtime_error);
      ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
      EXPECT_THROW((void)read_totp_secret_input(fifo.string(), unused), std::runtime_error);
      std::filesystem::remove(link);
      std::filesystem::remove(fifo);
      std::filesystem::remove(path);
    }

    TEST(TotpSecretInput, ReadsOnlyOneBoundedStdinLine) {
      std::istringstream input("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n");
      EXPECT_EQ(read_totp_secret_input("-", input), "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
      std::istringstream extra("private\nsecond-line\n");
      EXPECT_THROW((void)read_totp_secret_input("-", extra), std::runtime_error);
      std::istringstream too_long(std::string(513, 'A'));
      EXPECT_THROW((void)read_totp_secret_input("-", too_long), std::runtime_error);
    }

  } // namespace
} // namespace fmgr::cli
