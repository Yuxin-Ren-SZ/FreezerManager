// SPDX-License-Identifier: AGPL-3.0-or-later

// E3: Integration tests for AuthMiddleware (RBAC gate).
//
// Fixture: in-file SQLite backend, LocalAuthProvider, four users covering all
// built-in roles.  Mirrors the pattern in local_auth_provider_test.cpp.
//
// Users seeded:
//   user_sysadmin   — SystemAdmin role; no TOTP
//   user_lab_admin  — LabAdmin role; TOTP enrolled (RFC 6238 test secret)
//   user_member     — Member role; no TOTP
//   user_readonly   — ReadOnly role; no TOTP
//
// Two labs seeded: lab_a (all users except ReadOnly) and lab_b (no members).

#include "rpc/AuthMiddleware.h"

#include "auth/AuthTypes.h"
#include "auth/LocalAuthProvider.h"
#include "auth/Totp.h"
#include "core/identity.h"
#include "core/permissions.h"
#include "core/role.h"
#include "core/session.h"
#include "storage/IStorageBackend.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SessionTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace fmgr::rpc {
  namespace {

    // ---- Helpers ----

    [[nodiscard]] core::Uuid uuid_from_low(std::uint64_t low_bits) {
      std::array<std::uint8_t, 16> bytes{};
      for (std::size_t i = 0; i < 8; ++i) {
        bytes.at(15 - i) = static_cast<std::uint8_t>((low_bits >> (i * 8U)) & 0xffU);
      }
      return core::Uuid(bytes);
    }

    template <typename StrongId> [[nodiscard]] StrongId id_from_low(std::uint64_t low_bits) {
      return StrongId(uuid_from_low(low_bits));
    }

    [[nodiscard]] core::Timestamp ts(std::int64_t micros) {
      return core::Timestamp::from_unix_micros(micros);
    }

    [[nodiscard]] std::filesystem::path sqlite_test_path(std::string_view suffix) {
      const auto seed = std::to_string(
          static_cast<unsigned long long>(::testing::UnitTest::GetInstance()->random_seed()));
      const auto addr = std::to_string(reinterpret_cast<std::uintptr_t>(&suffix)); // NOLINT
      return std::filesystem::temp_directory_path() /
             (std::string("freezermanager-mw-") + seed + "-" + addr + "-" + std::string(suffix) +
              ".db");
    }

    void remove_sqlite_files(const std::filesystem::path& path) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + "-wal");
      std::filesystem::remove(path.string() + "-shm");
    }

    [[nodiscard]] storage::MutationContext test_ctx() {
      return storage::MutationContext{
          .actor_user_id = id_from_low<core::UserId>(999),
          .actor_session_id = "test-session",
          .request_id = "test-request",
          .reason = "auth middleware test",
      };
    }

    [[nodiscard]] auth::LocalAuthProviderConfig fast_config() {
      auth::LocalAuthProviderConfig cfg;
      cfg.pwhash_memlimit = 8192;
      cfg.pwhash_opslimit = 1;
      return cfg;
    }

    // ---- MockTransaction — captures set_session_var calls ----

    class CapturingTransaction final : public storage::ITransaction {
    public:
      void commit() override {}
      void rollback() override {}
      void set_session_var(std::string_view key, std::string_view value) override {
        captured[std::string(key)] = std::string(value);
      }
      std::unordered_map<std::string, std::string> captured;
    };

    // ---- Test helper: non-[[nodiscard]] wrapper so EXPECT_THROW/NO_THROW macros
    //      can discard the return value without triggering [[nodiscard]] warnings. ----

    auth::SessionContext do_authorize(AuthMiddleware& mw, std::string_view bearer,
                                      core::Permission perm,
                                      std::optional<core::LabId> lab = std::nullopt) {
      return mw.authorize(bearer, perm, lab);
    }

    // ---- Fixture ----

    class AuthMiddlewareTest : public ::testing::Test {
    protected:
      // TOTP secret: RFC 6238 test vector
      static constexpr std::string_view kTotpSecret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

      // Lab IDs
      const core::LabId kLabA{id_from_low<core::LabId>(1)};
      const core::LabId kLabB{id_from_low<core::LabId>(2)};

      // User IDs
      const core::UserId kSysAdminId{id_from_low<core::UserId>(10)};
      const core::UserId kLabAdminId{id_from_low<core::UserId>(11)};
      const core::UserId kMemberId{id_from_low<core::UserId>(12)};
      const core::UserId kReadOnlyId{id_from_low<core::UserId>(13)};

      void SetUp() override {
        db_path_ = sqlite_test_path("middleware");
        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_path_.string()});
        storage::register_identity_repositories(*backend_);
        storage::register_role_repositories(*backend_);
        storage::register_session_repositories(*backend_);
        backend_->migrate_to_latest();

        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_config());
        middleware_ = std::make_unique<AuthMiddleware>(*provider_);

        seed_test_data();

        // Pre-authenticate all users (except lab_admin TOTP path)
        sysadmin_token_ = provider_->authenticate(
            auth::PasswordCredentials{.email = "sysadmin@example.com", .password = "pw-sysadmin"},
            auth::ClientInfo{});
        lab_admin_token_ = provider_->authenticate(
            auth::PasswordCredentials{.email = "labadmin@example.com", .password = "pw-labadmin"},
            auth::ClientInfo{});
        member_token_ = provider_->authenticate(
            auth::PasswordCredentials{.email = "member@example.com", .password = "pw-member"},
            auth::ClientInfo{});
        readonly_token_ = provider_->authenticate(
            auth::PasswordCredentials{.email = "readonly@example.com", .password = "pw-readonly"},
            auth::ClientInfo{});
      }

      void TearDown() override {
        middleware_.reset();
        provider_.reset();
        backend_.reset();
        remove_sqlite_files(db_path_);
      }

      // Complete TOTP verification for lab_admin and return a fresh valid token.
      [[nodiscard]] auth::AuthToken lab_admin_mfa_complete() {
        const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const auto code = auth::totp_generate(kTotpSecret, now_s);
        provider_->verify_totp(lab_admin_token_.session_id, code);
        return lab_admin_token_;
      }

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      std::unique_ptr<AuthMiddleware> middleware_;

      auth::AuthToken sysadmin_token_;
      auth::AuthToken lab_admin_token_;
      auth::AuthToken member_token_;
      auth::AuthToken readonly_token_;

    private:
      void seed_test_data() {
        const auto pw_sysadmin = provider_->hash_password("pw-sysadmin");
        const auto pw_lab_admin = provider_->hash_password("pw-labadmin");
        const auto pw_member = provider_->hash_password("pw-member");
        const auto pw_readonly = provider_->hash_password("pw-readonly");

        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        const auto ctx = test_ctx();

        // Labs
        txn->repo<core::Lab>().insert(
            core::Lab{
                .id = kLabA, .name = "Lab A", .contact = "a@example.com", .created_at = ts(1'000)},
            ctx);
        txn->repo<core::Lab>().insert(
            core::Lab{
                .id = kLabB, .name = "Lab B", .contact = "b@example.com", .created_at = ts(1'001)},
            ctx);

        // Users
        auto insert_user = [&](core::UserId uid, std::string email, std::string pw_hash,
                               std::optional<std::string> totp = std::nullopt) {
          txn->repo<core::User>().insert(
              core::User{
                  .id = uid,
                  .primary_email = std::move(email),
                  .display_name = "Test User",
                  .status = core::UserStatus::Active,
                  .created_at = ts(2'000),
                  .auth_bindings =
                      nlohmann::json::array({{{"provider", "local"}, {"hash", pw_hash}}}),
                  .totp_secret_enc = std::move(totp),
              },
              ctx);
        };
        insert_user(kSysAdminId, "sysadmin@example.com", pw_sysadmin);
        insert_user(kLabAdminId, "labadmin@example.com", pw_lab_admin, std::string(kTotpSecret));
        insert_user(kMemberId, "member@example.com", pw_member);
        insert_user(kReadOnlyId, "readonly@example.com", pw_readonly);

        // Memberships (all in lab_a)
        auto insert_mem = [&](core::UserId uid, core::RoleKind role) {
          txn->repo<core::LabMembership>().insert(
              core::LabMembership{
                  .user_id = uid,
                  .lab_id = kLabA,
                  .role_id = core::builtin_role_id(role),
                  .joined_at = ts(3'000),
              },
              ctx);
        };
        insert_mem(kSysAdminId, core::RoleKind::SystemAdmin);
        insert_mem(kLabAdminId, core::RoleKind::LabAdmin);
        insert_mem(kMemberId, core::RoleKind::Member);
        insert_mem(kReadOnlyId, core::RoleKind::ReadOnly);

        txn->commit();
      }
    };

    // ---- Tests ----

    TEST_F(AuthMiddlewareTest, AuthorizeSucceedsAndReturnsSessionContext) {
      const auto ctx =
          middleware_->authorize(member_token_.plaintext_token, core::Permission::SampleRead);
      EXPECT_EQ(ctx.user_id, kMemberId);
      EXPECT_TRUE(ctx.mfa_complete);
      EXPECT_TRUE(ctx.can_see_lab(kLabA));
    }

    TEST_F(AuthMiddlewareTest, AuthorizeThrowsForRevokedSession) {
      provider_->revoke_session(member_token_.session_id, test_ctx());
      EXPECT_THROW(
          do_authorize(*middleware_, member_token_.plaintext_token, core::Permission::SampleRead),
          auth::AuthError);
    }

    TEST_F(AuthMiddlewareTest, AuthorizeThrowsMfaRequiredWhenMfaIncomplete) {
      // lab_admin has TOTP — the session starts with mfa_complete=false.
      ASSERT_FALSE(lab_admin_token_.mfa_complete);
      EXPECT_THROW(do_authorize(*middleware_, lab_admin_token_.plaintext_token,
                                core::Permission::SampleRead),
                   auth::MfaRequired);
    }

    TEST_F(AuthMiddlewareTest, AuthorizeThrowsPermissionDeniedForInsufficientRole) {
      // ReadOnly cannot write samples.
      EXPECT_THROW(do_authorize(*middleware_, readonly_token_.plaintext_token,
                                core::Permission::SampleWrite),
                   auth::PermissionDenied);
    }

    TEST_F(AuthMiddlewareTest, AuthorizeThrowsPermissionDeniedForHardDeleteWithMemberRole) {
      EXPECT_THROW(do_authorize(*middleware_, member_token_.plaintext_token,
                                core::Permission::SampleDeleteHard),
                   auth::PermissionDenied);
    }

    TEST_F(AuthMiddlewareTest, SystemAdminPassesForAllPermissionsExceptPhi) {
      // SystemAdmin has all 20 non-PHI permissions.
      for (const auto& entry : core::k_permission_catalog) {
        if (entry.value == core::Permission::PhiRead) {
          EXPECT_THROW(do_authorize(*middleware_, sysadmin_token_.plaintext_token, entry.value),
                       auth::PermissionDenied)
              << "expected PhiRead to be denied for SystemAdmin by default";
        } else {
          EXPECT_NO_THROW(do_authorize(*middleware_, sysadmin_token_.plaintext_token, entry.value))
              << "expected SystemAdmin to have permission: " << entry.key;
        }
      }
    }

    TEST_F(AuthMiddlewareTest, LabAdminCannotHardDeleteOrRotateKeys) {
      const auto completed_token = lab_admin_mfa_complete();
      EXPECT_THROW(do_authorize(*middleware_, completed_token.plaintext_token,
                                core::Permission::SampleDeleteHard),
                   auth::PermissionDenied);
      EXPECT_THROW(
          do_authorize(*middleware_, completed_token.plaintext_token, core::Permission::KeyRotate),
          auth::PermissionDenied);
    }

    TEST_F(AuthMiddlewareTest, AuthorizeThrowsForGarbageBearerToken) {
      EXPECT_THROW(
          do_authorize(*middleware_, "not-a-valid-token-at-all", core::Permission::SampleRead),
          auth::AuthError);
    }

    TEST_F(AuthMiddlewareTest, AuthorizeEnforcesLabVisibility) {
      // member is only in lab_a; lab_b is not in their visible_labs.
      EXPECT_THROW(do_authorize(*middleware_, member_token_.plaintext_token,
                                core::Permission::SampleRead, kLabB),
                   auth::PermissionDenied);
    }

    TEST_F(AuthMiddlewareTest, AuthorizeSucceedsWithMatchingLabId) {
      // member is in lab_a — should pass with lab_id = lab_a.
      auth::SessionContext ctx;
      EXPECT_NO_THROW(ctx = middleware_->authorize(member_token_.plaintext_token,
                                                   core::Permission::SampleRead, kLabA));
      EXPECT_EQ(ctx.user_id, kMemberId);
    }

    TEST_F(AuthMiddlewareTest, InjectRlsVarsCallsSetSessionVarWithUserAndLabs) {
      // Build a SessionContext directly (no DB needed for this test).
      auth::SessionContext ctx;
      ctx.user_id = kMemberId;
      ctx.visible_labs = {kLabA, kLabB};
      ctx.mfa_complete = true;

      CapturingTransaction tx;
      AuthMiddleware::inject_rls_vars(tx, ctx);

      EXPECT_EQ(tx.captured.at("app.current_user_id"), kMemberId.to_string());

      const auto expected_labs = kLabA.to_string() + "," + kLabB.to_string();
      EXPECT_EQ(tx.captured.at("app.current_lab_ids"), expected_labs);
    }

    TEST_F(AuthMiddlewareTest, InjectRlsVarsEmptyLabsProducesEmptyString) {
      auth::SessionContext ctx;
      ctx.user_id = kSysAdminId;
      ctx.visible_labs = {};
      ctx.mfa_complete = true;

      CapturingTransaction tx;
      AuthMiddleware::inject_rls_vars(tx, ctx);

      EXPECT_EQ(tx.captured.at("app.current_user_id"), kSysAdminId.to_string());
      EXPECT_EQ(tx.captured.at("app.current_lab_ids"), "");
    }

    TEST_F(AuthMiddlewareTest, RpcRegistryRegisterAndLookup) {
      const std::string rpc_name = "e3_test.GetSample";
      AuthMiddleware::register_rpc(rpc_name, core::Permission::SampleRead);
      EXPECT_TRUE(AuthMiddleware::is_rpc_registered(rpc_name));
      EXPECT_FALSE(AuthMiddleware::is_rpc_registered("nonexistent.Rpc"));
    }

    TEST_F(AuthMiddlewareTest, MfaCompleteRequiredBeforePrivilegedOps) {
      // lab_admin session has mfa_complete=false.
      ASSERT_FALSE(lab_admin_token_.mfa_complete);
      // Even a low-privilege permission should be blocked without MFA.
      EXPECT_THROW(do_authorize(*middleware_, lab_admin_token_.plaintext_token,
                                core::Permission::AuditExport),
                   auth::MfaRequired);
    }

    TEST_F(AuthMiddlewareTest, AuthorizeSucceedsAfterMfaCompletion) {
      const auto completed = lab_admin_mfa_complete();
      // After TOTP verification, lab_admin's session should pass for LabAdmin perms.
      auth::SessionContext ctx;
      EXPECT_NO_THROW(ctx = do_authorize(*middleware_, completed.plaintext_token,
                                         core::Permission::AuditExport));
      EXPECT_TRUE(ctx.mfa_complete);
    }

  } // namespace
} // namespace fmgr::rpc
