// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/session.h"

#include "test_helpers.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace fmgr::core {
  namespace {
    using namespace fmgr::test;

    [[nodiscard]] Timestamp ts(std::int64_t micros) {
      return Timestamp::from_unix_micros(micros);
    }

    // ---- Session tests ----

    TEST(SessionTypesTest, SessionDefaultConstruct) {
      const Session session{
          .id = id_from_low<SessionId>(1),
          .user_id = id_from_low<UserId>(2),
          .token_hash = "argon2id$hash",
          .token_prefix = "prefix01",
          .created_at = ts(1000),
          .last_seen_at = ts(2000),
      };
      EXPECT_EQ(session.id, id_from_low<SessionId>(1));
      EXPECT_EQ(session.user_id, id_from_low<UserId>(2));
      EXPECT_EQ(session.token_hash, "argon2id$hash");
      EXPECT_EQ(session.token_prefix, "prefix01");
      EXPECT_EQ(session.created_at.unix_micros(), 1000);
      EXPECT_EQ(session.last_seen_at.unix_micros(), 2000);
      EXPECT_FALSE(session.ip.has_value());
      EXPECT_FALSE(session.user_agent.has_value());
      EXPECT_FALSE(session.revoked_at.has_value());
    }

    TEST(SessionTypesTest, SessionWithOptionalFields) {
      const Session session{
          .id = id_from_low<SessionId>(10),
          .user_id = id_from_low<UserId>(20),
          .token_hash = "hash123",
          .token_prefix = "pre12345",
          .created_at = ts(100),
          .last_seen_at = ts(200),
          .ip = "192.168.1.1",
          .user_agent = "Mozilla/5.0",
          .revoked_at = ts(300),
      };
      EXPECT_TRUE(session.ip.has_value());
      EXPECT_EQ(*session.ip, "192.168.1.1");
      EXPECT_TRUE(session.user_agent.has_value());
      EXPECT_EQ(*session.user_agent, "Mozilla/5.0");
      EXPECT_TRUE(session.revoked_at.has_value());
      EXPECT_EQ(session.revoked_at->unix_micros(), 300);
    }

    TEST(SessionTypesTest, SessionEquality) {
      const Session s1{
          .id = id_from_low<SessionId>(1),
          .user_id = id_from_low<UserId>(2),
          .token_hash = "h",
          .token_prefix = "p",
          .created_at = ts(10),
          .last_seen_at = ts(20),
      };
      Session s2 = s1;
      EXPECT_EQ(s1, s2);
      s2.last_seen_at = ts(999);
      EXPECT_NE(s1, s2);
    }

    TEST(SessionTypesTest, SessionJsonRoundTrip) {
      const Session original{
          .id = id_from_low<SessionId>(5),
          .user_id = id_from_low<UserId>(6),
          .token_hash = "tok_hash",
          .token_prefix = "tok_pref",
          .created_at = ts(1111),
          .last_seen_at = ts(2222),
          .ip = "10.0.0.1",
          .user_agent = std::nullopt,
          .revoked_at = std::nullopt,
      };
      const auto json = nlohmann::json(original);
      const auto decoded = json.get<Session>();
      EXPECT_EQ(original, decoded);
    }

    TEST(SessionTypesTest, SessionJsonRoundTripWithRevoked) {
      const Session original{
          .id = id_from_low<SessionId>(7),
          .user_id = id_from_low<UserId>(8),
          .token_hash = "h2",
          .token_prefix = "p2",
          .created_at = ts(100),
          .last_seen_at = ts(200),
          .ip = std::nullopt,
          .user_agent = "curl/7.68",
          .revoked_at = ts(999),
      };
      const auto decoded = nlohmann::json(original).get<Session>();
      EXPECT_EQ(original, decoded);
      EXPECT_EQ(decoded.revoked_at->unix_micros(), 999);
    }

    // ---- ApiToken tests ----

    TEST(ApiTokenTypesTest, ApiTokenDefaultConstruct) {
      const ApiToken token{
          .id = id_from_low<ApiTokenId>(1),
          .user_id = id_from_low<UserId>(2),
          .name = "My token",
          .token_hash = "hash_val",
          .token_prefix = "fmgr_pat",
          .created_at = ts(500),
      };
      EXPECT_EQ(token.id, id_from_low<ApiTokenId>(1));
      EXPECT_EQ(token.name, "My token");
      EXPECT_EQ(token.scope_json, R"(["*"])");
      EXPECT_FALSE(token.lab_id.has_value());
      EXPECT_FALSE(token.expires_at.has_value());
      EXPECT_FALSE(token.revoked_at.has_value());
    }

    TEST(ApiTokenTypesTest, ApiTokenWithLabScope) {
      const ApiToken token{
          .id = id_from_low<ApiTokenId>(10),
          .user_id = id_from_low<UserId>(20),
          .lab_id = id_from_low<LabId>(30),
          .name = "Jupyter token",
          .scope_json = R"(["sample.read","sample.checkout"])",
          .token_hash = "h",
          .token_prefix = "p",
          .created_at = ts(1000),
          .expires_at = ts(2000),
      };
      EXPECT_TRUE(token.lab_id.has_value());
      EXPECT_EQ(*token.lab_id, id_from_low<LabId>(30));
      EXPECT_EQ(token.scope_json, R"(["sample.read","sample.checkout"])");
      EXPECT_TRUE(token.expires_at.has_value());
      EXPECT_EQ(token.expires_at->unix_micros(), 2000);
      EXPECT_FALSE(token.revoked_at.has_value());
    }

    TEST(ApiTokenTypesTest, ApiTokenEquality) {
      const ApiToken t1{
          .id = id_from_low<ApiTokenId>(1),
          .user_id = id_from_low<UserId>(2),
          .name = "tok",
          .token_hash = "h",
          .token_prefix = "p",
          .created_at = ts(1),
      };
      ApiToken t2 = t1;
      EXPECT_EQ(t1, t2);
      t2.name = "different";
      EXPECT_NE(t1, t2);
    }

    TEST(ApiTokenTypesTest, ApiTokenJsonRoundTrip) {
      const ApiToken original{
          .id = id_from_low<ApiTokenId>(99),
          .user_id = id_from_low<UserId>(88),
          .lab_id = id_from_low<LabId>(77),
          .name = "Automation token",
          .scope_json = R"(["sample.read"])",
          .token_hash = "argon2_out",
          .token_prefix = "fmgr_abc",
          .created_at = ts(10000),
          .expires_at = ts(20000),
          .revoked_at = std::nullopt,
      };
      const auto decoded = nlohmann::json(original).get<ApiToken>();
      EXPECT_EQ(original, decoded);
    }

    TEST(ApiTokenTypesTest, ApiTokenJsonRoundTripRevoked) {
      const ApiToken original{
          .id = id_from_low<ApiTokenId>(50),
          .user_id = id_from_low<UserId>(60),
          .lab_id = std::nullopt,
          .name = "Old token",
          .scope_json = "[]",
          .token_hash = "hh",
          .token_prefix = "pp",
          .created_at = ts(100),
          .expires_at = std::nullopt,
          .revoked_at = ts(999),
      };
      const auto decoded = nlohmann::json(original).get<ApiToken>();
      EXPECT_EQ(original, decoded);
      EXPECT_TRUE(decoded.revoked_at.has_value());
    }

  } // namespace
} // namespace fmgr::core
