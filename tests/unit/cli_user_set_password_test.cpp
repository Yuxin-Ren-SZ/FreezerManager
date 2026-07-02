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
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

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

  } // namespace
} // namespace fmgr::cli
