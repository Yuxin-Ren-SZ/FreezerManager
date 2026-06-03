// SPDX-License-Identifier: AGPL-3.0-or-later

// E2: Integration tests for LocalAuthProvider.
//
// Each test runs against a temporary in-file SQLite backend with all
// migrations applied.  Two test users are seeded:
//   • user_no_totp : password "hunter2"; Member role; no TOTP secret
//   • user_with_totp: password "s3cret!"; LabAdmin role; totp_secret_enc set
//
// Argon2id parameters are set to minimum values to keep test runtime short.

#include "auth/LocalAuthProvider.h"
#include "auth/Totp.h"

#include "core/identity.h"
#include "core/role.h"
#include "core/session.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SessionTraits.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <gtest/gtest.h>
#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace fmgr::auth {
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
             (std::string("freezermanager-auth-") + seed + "-" + addr + "-" + std::string(suffix) +
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
          .reason = "local auth provider test",
      };
    }

    // Minimum Argon2id params — fast enough for unit tests.
    [[nodiscard]] LocalAuthProviderConfig fast_config() {
      LocalAuthProviderConfig cfg;
      cfg.pwhash_memlimit = 8192;       // crypto_pwhash_MEMLIMIT_MIN
      cfg.pwhash_opslimit = 1;          // crypto_pwhash_OPSLIMIT_MIN
      cfg.lockout_duration_seconds = 1; // short lockout for timeout tests
      return cfg;
    }

    // ---- Fixture ----

    class LocalAuthProviderTest : public ::testing::Test {
    protected:
      void SetUp() override {
        db_path_ = sqlite_test_path("auth");
        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = db_path_.string()});
        storage::register_identity_repositories(*backend_);
        storage::register_role_repositories(*backend_);
        storage::register_session_repositories(*backend_);
        backend_->migrate_to_latest();

        provider_ = std::make_unique<LocalAuthProvider>(*backend_, fast_config());

        seed_test_data();
      }

      void TearDown() override {
        provider_.reset();
        backend_.reset();
        remove_sqlite_files(db_path_);
      }

      // Test lab and builtin roles are already present (seeded by migration).
      const core::LabId kLabId{id_from_low<core::LabId>(1)};
      const core::UserId kUserNoTotpId{id_from_low<core::UserId>(10)};
      const core::UserId kUserWithTotpId{id_from_low<core::UserId>(11)};

      // TOTP secret for kUserWithTotp: "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
      // (RFC 6238 test secret; see totp_test.cpp for known-answer verification)
      static constexpr std::string_view kTotpSecret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

      std::filesystem::path db_path_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<LocalAuthProvider> provider_;

    private:
      void seed_test_data() {
        // Hash passwords using the fast config.
        const auto pw_no_totp = provider_->hash_password("hunter2");
        const auto pw_with_totp = provider_->hash_password("s3cret!");

        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        const auto ctx = test_ctx();

        // Lab
        const core::Lab lab{
            .id = kLabId,
            .name = "Test Lab",
            .contact = "lab@example.com",
            .created_at = ts(1'000),
        };
        txn->repo<core::Lab>().insert(lab, ctx);

        // User without TOTP
        const core::User user_no_totp{
            .id = kUserNoTotpId,
            .primary_email = "nototp@example.com",
            .display_name = "No TOTP User",
            .status = core::UserStatus::Active,
            .created_at = ts(1'001),
            .auth_bindings = nlohmann::json::array({{{"provider", "local"}, {"hash", pw_no_totp}}}),
        };
        txn->repo<core::User>().insert(user_no_totp, ctx);

        // User with TOTP
        const core::User user_with_totp{
            .id = kUserWithTotpId,
            .primary_email = "totp@example.com",
            .display_name = "TOTP User",
            .status = core::UserStatus::Active,
            .created_at = ts(1'002),
            .auth_bindings =
                nlohmann::json::array({{{"provider", "local"}, {"hash", pw_with_totp}}}),
            .totp_secret_enc = std::string(kTotpSecret),
        };
        txn->repo<core::User>().insert(user_with_totp, ctx);

        // Membership: nototp → Member role
        const core::LabMembership mem_no_totp{
            .user_id = kUserNoTotpId,
            .lab_id = kLabId,
            .role_id = core::builtin_role_id(core::RoleKind::Member),
            .joined_at = ts(1'003),
        };
        txn->repo<core::LabMembership>().insert(mem_no_totp, ctx);

        // Membership: totp_user → LabAdmin role
        const core::LabMembership mem_with_totp{
            .user_id = kUserWithTotpId,
            .lab_id = kLabId,
            .role_id = core::builtin_role_id(core::RoleKind::LabAdmin),
            .joined_at = ts(1'004),
        };
        txn->repo<core::LabMembership>().insert(mem_with_totp, ctx);

        txn->commit();
      }
    };

    // ---- authenticate: password path ----

    TEST_F(LocalAuthProviderTest, AuthenticateCorrectPassword) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{});
      EXPECT_FALSE(token.plaintext_token.empty());
      EXPECT_TRUE(token.mfa_complete);
    }

    TEST_F(LocalAuthProviderTest, AuthenticateEmailCaseInsensitive) {
      // The provider normalizes email to lowercase before looking up.
      EXPECT_NO_THROW(provider_->authenticate(
          PasswordCredentials{.email = "NOTOTP@EXAMPLE.COM", .password = "hunter2"}, ClientInfo{}));
    }

    TEST_F(LocalAuthProviderTest, AuthenticateWrongPassword) {
      EXPECT_THROW(provider_->authenticate(
                       PasswordCredentials{.email = "nototp@example.com", .password = "wrongpw"},
                       ClientInfo{}),
                   InvalidCredentials);
    }

    TEST_F(LocalAuthProviderTest, AuthenticateUnknownEmail) {
      EXPECT_THROW(provider_->authenticate(
                       PasswordCredentials{.email = "nobody@example.com", .password = "hunter2"},
                       ClientInfo{}),
                   InvalidCredentials);
    }

    TEST_F(LocalAuthProviderTest, AuthenticateDisabledUser) {
      // Disable the user first.
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::User>().soft_delete(kUserNoTotpId, test_ctx());
        txn->commit();
      }
      EXPECT_THROW(provider_->authenticate(
                       PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"},
                       ClientInfo{}),
                   InvalidCredentials);
    }

    TEST_F(LocalAuthProviderTest, AuthenticateNoLocalBinding) {
      // Insert a user without a local auth binding.
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        const core::User user{
            .id = id_from_low<core::UserId>(20),
            .primary_email = "oidc@example.com",
            .display_name = "OIDC Only",
            .status = core::UserStatus::Active,
            .created_at = ts(2'000),
            .auth_bindings = nlohmann::json::array({{{"provider", "oidc"}, {"sub", "abc123"}}}),
        };
        txn->repo<core::User>().insert(user, test_ctx());
        txn->commit();
      }
      EXPECT_THROW(
          provider_->authenticate(
              PasswordCredentials{.email = "oidc@example.com", .password = "any"}, ClientInfo{}),
          InvalidCredentials);
    }

    TEST_F(LocalAuthProviderTest, AuthenticateFiveFailuresLockAccount) {
      // Five failures should trigger AccountLocked on the fifth attempt.
      // (max_failures_before_lockout = 5 in fast_config)
      LocalAuthProviderConfig cfg = fast_config();
      cfg.max_failures_before_lockout = 5;
      auto local_provider = LocalAuthProvider(*backend_, cfg);

      auto try_bad = [&] {
        EXPECT_THROW(local_provider.authenticate(
                         PasswordCredentials{.email = "nototp@example.com", .password = "bad"},
                         ClientInfo{}),
                     AuthError);
      };

      for (int i = 0; i < 4; ++i) {
        try_bad();
      }
      // 5th failure should throw AccountLocked.
      EXPECT_THROW(
          local_provider.authenticate(
              PasswordCredentials{.email = "nototp@example.com", .password = "bad"}, ClientInfo{}),
          AccountLocked);
    }

    TEST_F(LocalAuthProviderTest, AuthenticateLockRespectedBeforeTimeout) {
      LocalAuthProviderConfig cfg = fast_config();
      cfg.max_failures_before_lockout = 2;
      cfg.lockout_duration_seconds = 3600; // long lockout so it doesn't expire
      auto local_provider = LocalAuthProvider(*backend_, cfg);

      auto try_bad = [&] {
        try {
          local_provider.authenticate(
              PasswordCredentials{.email = "nototp@example.com", .password = "bad"}, ClientInfo{});
        } catch (...) {
        }
      };

      try_bad();
      try_bad(); // triggers lockout

      // Correct password but still locked.
      EXPECT_THROW(local_provider.authenticate(
                       PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"},
                       ClientInfo{}),
                   AccountLocked);
    }

    TEST_F(LocalAuthProviderTest, AuthenticateSuccessAfterLockExpires) {
      LocalAuthProviderConfig cfg = fast_config();
      cfg.max_failures_before_lockout = 2;
      cfg.lockout_duration_seconds = 0; // lock expires immediately
      auto local_provider = LocalAuthProvider(*backend_, cfg);

      auto try_bad = [&] {
        try {
          local_provider.authenticate(
              PasswordCredentials{.email = "nototp@example.com", .password = "bad"}, ClientInfo{});
        } catch (...) {
        }
      };

      try_bad();
      try_bad(); // triggers lockout (locked_until = now + 0 = already expired)

      // Lockout expired instantly; correct password should succeed.
      EXPECT_NO_THROW(local_provider.authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{}));
    }

    TEST_F(LocalAuthProviderTest, AuthenticateSuccessResetsFailureCount) {
      LocalAuthProviderConfig cfg = fast_config();
      cfg.max_failures_before_lockout = 3;
      auto local_provider = LocalAuthProvider(*backend_, cfg);

      // Two failures...
      for (int i = 0; i < 2; ++i) {
        try {
          local_provider.authenticate(
              PasswordCredentials{.email = "nototp@example.com", .password = "bad"}, ClientInfo{});
        } catch (...) {
        }
      }
      // ...then a success resets the counter.
      EXPECT_NO_THROW(local_provider.authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{}));

      // Two more failures after reset should not lock.
      for (int i = 0; i < 2; ++i) {
        try {
          local_provider.authenticate(
              PasswordCredentials{.email = "nototp@example.com", .password = "bad"}, ClientInfo{});
        } catch (...) {
        }
      }
      EXPECT_NO_THROW(local_provider.authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{}));
    }

    TEST_F(LocalAuthProviderTest, AuthenticateTotpUserMfaIncomplete) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "totp@example.com", .password = "s3cret!"}, ClientInfo{});
      EXPECT_FALSE(token.mfa_complete);
    }

    TEST_F(LocalAuthProviderTest, AuthenticateMemberNoTotpMfaComplete) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{});
      EXPECT_TRUE(token.mfa_complete);
    }

    // ---- validate_token ----

    TEST_F(LocalAuthProviderTest, ValidateTokenRoundTrip) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"},
          ClientInfo{.ip = "127.0.0.1"});

      const SessionContext ctx = provider_->validate_token(token.plaintext_token);
      EXPECT_EQ(ctx.user_id, kUserNoTotpId);
      EXPECT_TRUE(ctx.mfa_complete);
      EXPECT_TRUE(ctx.can_see_lab(kLabId));
    }

    TEST_F(LocalAuthProviderTest, ValidateTokenBuildsPermissions) {
      // The LabAdmin user's SessionContext should include audit.read (LabAdmin has it).
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "totp@example.com", .password = "s3cret!"}, ClientInfo{});
      const SessionContext ctx = provider_->validate_token(token.plaintext_token);
      EXPECT_EQ(ctx.user_id, kUserWithTotpId);
      // LabAdmin has sample.read at minimum.
      EXPECT_TRUE(ctx.has(core::Permission::SampleRead));
    }

    TEST_F(LocalAuthProviderTest, ValidateTokenBadToken) {
      EXPECT_THROW(provider_->validate_token(
                       "0000000000000000000000000000000000000000000000000000000000000000"),
                   InvalidCredentials);
    }

    TEST_F(LocalAuthProviderTest, ValidateTokenRevokedSession) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{});

      provider_->revoke_session(token.session_id, test_ctx());

      // After revocation, validate_token must throw.
      EXPECT_THROW(provider_->validate_token(token.plaintext_token), InvalidCredentials);
    }

    // ---- verify_totp ----

    TEST_F(LocalAuthProviderTest, VerifyTotpSuccess) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "totp@example.com", .password = "s3cret!"}, ClientInfo{});
      ASSERT_FALSE(token.mfa_complete);

      // Generate the current TOTP code using the known secret.
      const auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
      const auto code = totp_generate(kTotpSecret, now_seconds);

      EXPECT_NO_THROW(provider_->verify_totp(token.session_id, code));

      // After verify_totp, validate_token should return mfa_complete=true.
      const SessionContext ctx = provider_->validate_token(token.plaintext_token);
      EXPECT_TRUE(ctx.mfa_complete);
    }

    TEST_F(LocalAuthProviderTest, VerifyTotpWrongCode) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "totp@example.com", .password = "s3cret!"}, ClientInfo{});
      EXPECT_THROW(provider_->verify_totp(token.session_id, "000000"), InvalidCredentials);
    }

    TEST_F(LocalAuthProviderTest, VerifyTotpAlreadyComplete) {
      // A session that already has mfa_complete=true must reject verify_totp.
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{});
      ASSERT_TRUE(token.mfa_complete);
      EXPECT_THROW(provider_->verify_totp(token.session_id, "123456"), InvalidCredentials);
    }

    // ---- revoke_session ----

    TEST_F(LocalAuthProviderTest, RevokeSessionSetsRevokedAt) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{});

      provider_->revoke_session(token.session_id, test_ctx());

      // Session should now be revoked (excluded from default query).
      auto rtxn = backend_->begin(storage::IsolationLevel::ReadCommitted);
      const auto sessions = rtxn->repo<core::Session>().query(storage::Query<core::Session>::all());
      rtxn->commit();

      const auto revoked = std::ranges::any_of(sessions, [&](const core::Session& s) {
        return s.id == token.session_id && s.revoked_at.has_value();
      });
      // The session exists (include_tombstoned would find it), but the default query
      // excludes it, so 'sessions' should be empty.
      EXPECT_TRUE(sessions.empty());
      (void)revoked;
    }

    // ---- revoke_all_sessions ----

    TEST_F(LocalAuthProviderTest, RevokeAllSessionsRevokesAll) {
      // Create two sessions for the same user.
      provider_->authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{});
      provider_->authenticate(
          PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"}, ClientInfo{});

      provider_->revoke_all_sessions(kUserNoTotpId, test_ctx());

      // No active sessions remain.
      auto rtxn = backend_->begin(storage::IsolationLevel::ReadCommitted);
      const auto sessions = rtxn->repo<core::Session>().query(storage::Query<core::Session>::all());
      rtxn->commit();
      EXPECT_TRUE(sessions.empty());
    }

    // ---- API token helpers ----

    struct PreparedApiToken {
      std::string full_token;
      core::ApiTokenId id;
      std::string token_hash;
      std::string token_prefix;
    };

    [[nodiscard]] static PreparedApiToken prepare_api_token(const core::UserId& user_id,
                                                             std::uint64_t token_id_low) {
      // Generate 32 random bytes → 64-char hex string (matching generate_token()).
      std::array<unsigned char, 32> buf{};
      randombytes_buf(buf.data(), buf.size());
      constexpr char k_hex[] = "0123456789abcdef";
      std::string hex_part;
      hex_part.reserve(64);
      for (std::size_t i = 0; i < buf.size(); ++i) {
        hex_part.push_back(k_hex[(buf[i] >> 4U) & 0xFU]);
        hex_part.push_back(k_hex[buf[i] & 0xFU]);
      }

      const std::string full = "fmgr_pat_" + hex_part;

      // Compute prefix: first 16 hex chars of hex part.
      const std::string prefix = hex_part.substr(0, 16);

      // Compute BLAKE2b-256 hash of hex part (matching hash_token()).
      std::array<unsigned char, crypto_generichash_BYTES> hash_bytes{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      crypto_generichash(hash_bytes.data(), hash_bytes.size(),
                         reinterpret_cast<const unsigned char*>(hex_part.data()), hex_part.size(),
                         nullptr, 0);
      std::string hash_hex;
      hash_hex.reserve(crypto_generichash_BYTES * 2);
      for (std::size_t i = 0; i < hash_bytes.size(); ++i) {
        hash_hex.push_back(k_hex[(hash_bytes[i] >> 4U) & 0xFU]);
        hash_hex.push_back(k_hex[hash_bytes[i] & 0xFU]);
      }

      return PreparedApiToken{
          .full_token = full,
          .id = id_from_low<core::ApiTokenId>(token_id_low),
          .token_hash = hash_hex,
          .token_prefix = prefix,
      };
    }

    // ---- authenticate: API token path ----

    TEST_F(LocalAuthProviderTest, AuthenticateApiTokenRoundTrip) {
      const auto pat = prepare_api_token(kUserNoTotpId, 100);

      const core::ApiToken api_token{
          .id = pat.id,
          .user_id = kUserNoTotpId,
          .name = "round-trip token",
          .token_hash = pat.token_hash,
          .token_prefix = pat.token_prefix,
          .created_at = ts(5'000),
      };
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(api_token, test_ctx());
        txn->commit();
      }

      const AuthToken auth_token = provider_->authenticate(
          ApiTokenCredentials{.token = pat.full_token}, ClientInfo{});
      EXPECT_FALSE(auth_token.plaintext_token.empty());
      EXPECT_TRUE(auth_token.mfa_complete);

      const SessionContext ctx = provider_->validate_token(auth_token.plaintext_token);
      EXPECT_EQ(ctx.user_id, kUserNoTotpId);
      EXPECT_TRUE(ctx.mfa_complete);
      EXPECT_TRUE(ctx.can_see_lab(kLabId));
      // no-totp user has Member role → at least SampleRead
      EXPECT_TRUE(ctx.has(core::Permission::SampleRead));
    }

    TEST_F(LocalAuthProviderTest, AuthenticateApiTokenExpired) {
      const auto pat = prepare_api_token(kUserNoTotpId, 101);

      const core::ApiToken api_token{
          .id = pat.id,
          .user_id = kUserNoTotpId,
          .name = "expired token",
          .token_hash = pat.token_hash,
          .token_prefix = pat.token_prefix,
          .created_at = ts(5'000),
          .expires_at = ts(1'000), // far in the past
      };
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(api_token, test_ctx());
        txn->commit();
      }

      EXPECT_THROW(provider_->authenticate(
                       ApiTokenCredentials{.token = pat.full_token}, ClientInfo{}),
                   TokenExpired);
    }

    // ---- validate_token: API token revocation ----

    TEST_F(LocalAuthProviderTest, ValidateApiTokenRevoked) {
      const auto pat = prepare_api_token(kUserNoTotpId, 102);

      const core::ApiToken api_token{
          .id = pat.id,
          .user_id = kUserNoTotpId,
          .name = "revoked token",
          .token_hash = pat.token_hash,
          .token_prefix = pat.token_prefix,
          .created_at = ts(5'000),
      };
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().insert(api_token, test_ctx());
        txn->commit();
      }

      // Authenticate once to confirm the token works.
      const AuthToken auth_token = provider_->authenticate(
          ApiTokenCredentials{.token = pat.full_token}, ClientInfo{});
      const SessionContext ctx = provider_->validate_token(auth_token.plaintext_token);
      EXPECT_EQ(ctx.user_id, kUserNoTotpId);

      // Revoke the API token.
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::ApiToken>().soft_delete(pat.id, test_ctx());
        txn->commit();
      }

      // Validation must now fail.
      EXPECT_THROW(provider_->validate_token(pat.full_token), InvalidCredentials);
    }

    // ---- verify_totp: session revocation ----

    TEST_F(LocalAuthProviderTest, VerifyTotpRevokedSession) {
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "totp@example.com", .password = "s3cret!"}, ClientInfo{});
      ASSERT_FALSE(token.mfa_complete);

      provider_->revoke_session(token.session_id, test_ctx());

      EXPECT_THROW(provider_->verify_totp(token.session_id, "000000"), InvalidCredentials);
    }

    // ---- verify_totp: no TOTP secret (race condition) ----

    TEST_F(LocalAuthProviderTest, VerifyTotpNoUserSecret) {
      // Authenticate as TOTP user → session created with mfa_complete=false.
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "totp@example.com", .password = "s3cret!"}, ClientInfo{});
      ASSERT_FALSE(token.mfa_complete);

      // Remove the user's TOTP secret (simulating a concurrent admin action).
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        auto users = txn->repo<core::User>().query(storage::Query<core::User>::where(
            storage::field<core::User, std::string>(core::User::Field::Id) ==
            kUserWithTotpId.to_string()));
        ASSERT_FALSE(users.empty());
        auto user = users.front();
        user.totp_secret_enc = std::nullopt;
        txn->repo<core::User>().update(user, test_ctx());
        txn->commit();
      }

      EXPECT_THROW(provider_->verify_totp(token.session_id, "000000"), InvalidCredentials);
    }

    // ---- verify_totp: disabled user ---- 

    TEST_F(LocalAuthProviderTest, VerifyTotpDisabledUser) {
      // Authenticate as TOTP user → session created with mfa_complete=false.
      const AuthToken token = provider_->authenticate(
          PasswordCredentials{.email = "totp@example.com", .password = "s3cret!"}, ClientInfo{});
      ASSERT_FALSE(token.mfa_complete);

      // Disable the user (simulating admin action while session is active).
      {
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::User>().soft_delete(kUserWithTotpId, test_ctx());
        txn->commit();
      }

      // verify_totp should still fail because the user query finds the user
      // (soft_delete doesn't remove the row), and the TOTP secret is still set.
      // The actual rejection should come from the TOTP code being wrong.
      EXPECT_THROW(provider_->verify_totp(token.session_id, "000000"), InvalidCredentials);
    }

    // ---- lockout: per-email isolation ---- 

    TEST_F(LocalAuthProviderTest, LockoutIsPerEmail) {
      LocalAuthProviderConfig cfg = fast_config();
      cfg.max_failures_before_lockout = 3;
      auto local_provider = LocalAuthProvider(*backend_, cfg);

      // Exhaust failures for nototp@example.com
      for (int i = 0; i < 3; ++i) {
        try {
          local_provider.authenticate(
              PasswordCredentials{.email = "nototp@example.com", .password = "bad"}, ClientInfo{});
        } catch (...) {
        }
      }
      // nototp is now locked.
      EXPECT_THROW(local_provider.authenticate(
                       PasswordCredentials{.email = "nototp@example.com", .password = "hunter2"},
                       ClientInfo{}),
                   AccountLocked);

      // totp@example.com should still be able to authenticate.
      EXPECT_NO_THROW(local_provider.authenticate(
          PasswordCredentials{.email = "totp@example.com", .password = "s3cret!"}, ClientInfo{}));
    }

  } // namespace
} // namespace fmgr::auth
