// SPDX-License-Identifier: AGPL-3.0-or-later

// E1: Tests for auth value types (AuthTypes.h) and the IAuthProvider interface.
// These tests cover the type contracts; provider behaviour is tested in
// local_auth_provider_test.cpp (E2).

#include "auth/AuthTypes.h"
#include "auth/IAuthProvider.h"

#include "core/ids.h"
#include "core/permissions.h"
#include "core/timestamp.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace fmgr::auth {
  namespace {
    using namespace fmgr::test;

    // ---- helpers ----

    [[nodiscard]] core::SessionId session_id_1() {
      return core::SessionId::parse("00000000-0000-0000-0000-000000000001");
    }

    [[nodiscard]] core::UserId user_id_1() {
      return core::UserId::parse("00000000-0000-0000-0000-000000000002");
    }

    [[nodiscard]] core::LabId lab_id_1() {
      return core::LabId::parse("00000000-0000-0000-0000-000000000010");
    }

    [[nodiscard]] core::LabId lab_id_2() {
      return core::LabId::parse("00000000-0000-0000-0000-000000000011");
    }

    // ---- PasswordCredentials ----

    TEST(AuthTypesTest, PasswordCredentialsFields) {
      const PasswordCredentials creds{.email = "user@lab.example", .password = "s3cr3t"};
      EXPECT_EQ(creds.email, "user@lab.example");
      EXPECT_EQ(creds.password, "s3cr3t");
    }

    // ---- ApiTokenCredentials ----

    TEST(AuthTypesTest, ApiTokenCredentialsFields) {
      const ApiTokenCredentials creds{.token = "fmgr_pat_abc123"};
      EXPECT_EQ(creds.token, "fmgr_pat_abc123");
    }

    // ---- AuthCredentials variant ----

    TEST(AuthTypesTest, AuthCredentialsHoldsPasswordCredentials) {
      const AuthCredentials creds = PasswordCredentials{.email = "a@b.com", .password = "pw"};
      EXPECT_TRUE(std::holds_alternative<PasswordCredentials>(creds));
      EXPECT_EQ(std::get<PasswordCredentials>(creds).email, "a@b.com");
    }

    TEST(AuthTypesTest, AuthCredentialsHoldsApiTokenCredentials) {
      const AuthCredentials creds = ApiTokenCredentials{.token = "tok"};
      EXPECT_TRUE(std::holds_alternative<ApiTokenCredentials>(creds));
      EXPECT_EQ(std::get<ApiTokenCredentials>(creds).token, "tok");
    }

    // ---- ClientInfo ----

    TEST(AuthTypesTest, ClientInfoDefaultHasEmptyOptionals) {
      const ClientInfo info{};
      EXPECT_FALSE(info.ip.has_value());
      EXPECT_FALSE(info.user_agent.has_value());
    }

    TEST(AuthTypesTest, ClientInfoWithIpAndUserAgent) {
      const ClientInfo info{.ip = "127.0.0.1", .user_agent = "Mozilla/5.0"};
      ASSERT_TRUE(info.ip.has_value());
      EXPECT_EQ(*info.ip, "127.0.0.1");
      ASSERT_TRUE(info.user_agent.has_value());
      EXPECT_EQ(*info.user_agent, "Mozilla/5.0");
    }

    // ---- AuthToken ----

    TEST(AuthTypesTest, AuthTokenFields) {
      const AuthToken token{
          .session_id = session_id_1(),
          .plaintext_token = "supersecrettoken",
          .mfa_complete = true,
      };
      EXPECT_EQ(token.session_id, session_id_1());
      EXPECT_EQ(token.plaintext_token, "supersecrettoken");
      EXPECT_TRUE(token.mfa_complete);
    }

    TEST(AuthTypesTest, AuthTokenMfaIncompleteFlag) {
      const AuthToken token{
          .session_id = session_id_1(),
          .plaintext_token = "tok",
          .mfa_complete = false,
      };
      EXPECT_FALSE(token.mfa_complete);
    }

    // ---- SessionContext ----

    TEST(AuthTypesTest, SessionContextHasLabPermissionTrue) {
      const SessionContext ctx{
          .session_id = session_id_1(),
          .user_id = user_id_1(),
          .permissions_by_lab = {{lab_id_1(),
                                  {core::Permission::SampleRead, core::Permission::SampleWrite}}},
          .mfa_complete = true,
      };
      EXPECT_TRUE(ctx.has_for_lab(lab_id_1(), core::Permission::SampleRead));
      EXPECT_TRUE(ctx.has_for_lab(lab_id_1(), core::Permission::SampleWrite));
    }

    TEST(AuthTypesTest, SessionContextHasLabPermissionFalse) {
      const SessionContext ctx{
          .session_id = session_id_1(),
          .user_id = user_id_1(),
          .permissions_by_lab = {{lab_id_1(), {core::Permission::SampleRead}}},
          .mfa_complete = true,
      };
      EXPECT_FALSE(ctx.has_for_lab(lab_id_1(), core::Permission::SampleWrite));
      EXPECT_FALSE(ctx.has_for_lab(lab_id_2(), core::Permission::SampleRead));
    }

    TEST(AuthTypesTest, SessionContextHasGlobalPermission) {
      const SessionContext ctx{
          .session_id = session_id_1(),
          .user_id = user_id_1(),
          .global_permissions = {core::Permission::KeyRotate},
          .mfa_complete = true,
      };
      EXPECT_TRUE(ctx.has_global(core::Permission::KeyRotate));
      EXPECT_FALSE(ctx.has_global(core::Permission::BackupRun));
    }

    TEST(AuthTypesTest, SessionContextCanSeeLabTrue) {
      const SessionContext ctx{
          .session_id = session_id_1(),
          .user_id = user_id_1(),
          .permissions_by_lab = {{lab_id_1(), {}}, {lab_id_2(), {}}},
          .mfa_complete = true,
      };
      EXPECT_TRUE(ctx.can_see_lab(lab_id_1()));
      EXPECT_TRUE(ctx.can_see_lab(lab_id_2()));
    }

    TEST(AuthTypesTest, SessionContextCanSeeLabFalse) {
      const SessionContext ctx{
          .session_id = session_id_1(),
          .user_id = user_id_1(),
          .permissions_by_lab = {{lab_id_1(), {}}},
          .mfa_complete = true,
      };
      const auto other_lab = core::LabId::parse("00000000-0000-0000-0000-000000000099");
      EXPECT_FALSE(ctx.can_see_lab(other_lab));
    }

    TEST(AuthTypesTest, SessionContextEmptyVisibleLabs) {
      const SessionContext ctx{
          .session_id = session_id_1(),
          .user_id = user_id_1(),
          .mfa_complete = true,
      };
      EXPECT_FALSE(ctx.can_see_lab(lab_id_1()));
    }

    // ---- Error hierarchy ----

    TEST(AuthTypesTest, AuthErrorIsRuntimeError) {
      static_assert(std::is_base_of_v<std::runtime_error, AuthError>);
    }

    TEST(AuthTypesTest, InvalidCredentialsCatchesAsAuthError) {
      try {
        throw InvalidCredentials("wrong password");
      } catch (const AuthError& e) {
        EXPECT_EQ(std::string(e.what()), "wrong password");
      } catch (...) {
        FAIL() << "InvalidCredentials not caught as AuthError";
      }
    }

    TEST(AuthTypesTest, AccountLockedHasLockedUntil) {
      const auto until = ts(9'999'999'999LL);
      const AccountLocked err(until);
      EXPECT_EQ(err.locked_until, until);
      EXPECT_THROW(throw AccountLocked(until), AuthError);
    }

    TEST(AuthTypesTest, MfaRequiredCatchesAsAuthError) {
      EXPECT_THROW(throw MfaRequired("totp required"), AuthError);
    }

    TEST(AuthTypesTest, TokenExpiredCatchesAsAuthError) {
      EXPECT_THROW(throw TokenExpired("token expired"), AuthError);
    }

    TEST(AuthTypesTest, TokenRevokedCatchesAsAuthError) {
      EXPECT_THROW(throw TokenRevoked("session revoked"), AuthError);
    }

    TEST(AuthTypesTest, PermissionDeniedCatchesAsAuthError) {
      EXPECT_THROW(throw PermissionDenied("access denied"), AuthError);
    }

    // ---- IAuthProvider interface compiles with a concrete mock ----

    class MockAuthProvider final : public IAuthProvider {
    public:
      AuthToken authenticate(const AuthCredentials& /*creds*/,
                             const ClientInfo& /*client*/) override {
        return AuthToken{.session_id = session_id_1(), .plaintext_token = "tok"};
      }

      SessionContext validate_token(std::string_view /*bearer*/) override {
        return SessionContext{.session_id = session_id_1(), .user_id = user_id_1()};
      }

      void verify_totp(const core::SessionId& /*session_id*/,
                       std::string_view /*totp_code*/) override {}

      void revoke_session(const core::SessionId& /*session_id*/,
                          const storage::MutationContext& /*ctx*/) override {}

      void revoke_all_sessions(const core::UserId& /*uid*/,
                               const storage::MutationContext& /*ctx*/) override {}
    };

    TEST(AuthTypesTest, IAuthProviderMockCompiles) {
      MockAuthProvider provider;
      const AuthCredentials creds = PasswordCredentials{.email = "x@y.com", .password = "p"};
      const ClientInfo client{};
      const AuthToken token = provider.authenticate(creds, client);
      EXPECT_EQ(token.session_id, session_id_1());

      const SessionContext ctx = provider.validate_token("tok");
      EXPECT_EQ(ctx.user_id, user_id_1());
    }

  } // namespace
} // namespace fmgr::auth
