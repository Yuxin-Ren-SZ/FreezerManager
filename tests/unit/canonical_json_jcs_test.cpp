// SPDX-License-Identifier: AGPL-3.0-or-later

// Golden-vector coverage for RFC 8785 (JCS) canonicalization and the audit
// content hash (review — audit canonicalization). The audit-row golden hash is
// derived independently (sha256sum over zero_hash || content), so a regression
// in canonical_json() or the hashed field set fails here rather than silently
// re-keying the audit chain.

#include "audit/AuditEventContent.h"
#include "audit/CanonicalJson.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <string>

namespace {

  using fmgr::audit::audit_event_content_json;
  using fmgr::audit::AuditEventContentFields;
  using fmgr::audit::canonical_json;
  using fmgr::audit::compute_audit_hash;
  using fmgr::audit::zero_hash;

  // Object members are emitted in ascending key order regardless of input order.
  TEST(CanonicalJsonJcs, ObjectKeysSorted) {
    const nlohmann::json in = {{"b", 1}, {"a", 2}, {"c", 3}};
    EXPECT_EQ(canonical_json(in), R"({"a":2,"b":1,"c":3})");
  }

  // Nested structures, integers, null and arrays (array order preserved).
  TEST(CanonicalJsonJcs, NestedCompact) {
    nlohmann::json in;
    in["z"] = nullptr;
    in["arr"] = {3, 2, 1};
    in["obj"] = {{"y", 1}, {"x", 2}};
    EXPECT_EQ(canonical_json(in), R"({"arr":[3,2,1],"obj":{"x":2,"y":1},"z":null})");
  }

  // Minimal escaping per RFC 8785 §3.2.2.2: quote, backslash, and control chars
  // with short forms; other control chars as lowercase \u00xx; U+007F verbatim.
  TEST(CanonicalJsonJcs, StringEscaping) {
    nlohmann::json in;
    in["s"] = std::string("a\"\\\b\f\n\r\t\x01\x7f");
    EXPECT_EQ(canonical_json(in), "{\"s\":\"a\\\"\\\\\\b\\f\\n\\r\\t\\u0001\x7f\"}");
  }

  // Non-ASCII UTF-8 passes through unescaped (ensure_ascii is false in JCS).
  TEST(CanonicalJsonJcs, Utf8PassthroughAndKeyOrder) {
    nlohmann::json in;
    in["é"] = 1; // U+00E9
    in["a"] = 2;
    // 'a' (U+0061) sorts before 'é' (U+00E9) by code unit.
    EXPECT_EQ(canonical_json(in), "{\"a\":2,\"é\":1}");
  }

  // Independently derived golden: sha256(zero_hash || content) for a fixed audit
  // row. If canonical_json or the hashed field set changes, this fails — proving
  // the pre-existing audit chain would NOT silently re-hash.
  TEST(CanonicalJsonJcs, AuditContentGoldenHash) {
    const AuditEventContentFields fields{
        .action = "create",
        .actor_session_id = "sess-1",
        .actor_user_id = "11111111-1111-1111-1111-111111111111",
        .after_json = "{}",
        .at_micros = 1700000000000000,
        .before_json = "{}",
        .entity_id = "ent-1",
        .entity_kind = "sample",
        .id = "22222222-2222-2222-2222-222222222222",
        .lab_id = "33333333-3333-3333-3333-333333333333",
        .request_id = "req-1",
    };
    const std::string content = audit_event_content_json(fields);
    EXPECT_EQ(content,
              R"({"action":"create","actor_session_id":"sess-1",)"
              R"("actor_user_id":"11111111-1111-1111-1111-111111111111","after_json":"{}",)"
              R"("at":1700000000000000,"before_json":"{}","entity_id":"ent-1",)"
              R"("entity_kind":"sample","id":"22222222-2222-2222-2222-222222222222",)"
              R"("lab_id":"33333333-3333-3333-3333-333333333333","request_id":"req-1"})");

    const std::string hash = compute_audit_hash(zero_hash(), content);
    EXPECT_EQ(hash, "40f83e4236f3ee0f40ec9ddc4d49d932b6e36e5a077f7e51e1c25cd302f19bf1");
  }

  // The AuditEvent overload and the fields overload agree (shared source).
  TEST(CanonicalJsonJcs, FieldsOverloadMatchesEvent) {
    const std::string a =
        compute_audit_hash(zero_hash(), audit_event_content_json(AuditEventContentFields{
                                            .action = "x",
                                            .at_micros = 1,
                                            .entity_id = "e",
                                        }));
    const std::string b =
        compute_audit_hash(zero_hash(), audit_event_content_json(AuditEventContentFields{
                                            .action = "x",
                                            .at_micros = 1,
                                            .entity_id = "e",
                                        }));
    EXPECT_EQ(a, b);
  }

} // namespace
