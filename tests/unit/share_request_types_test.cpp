// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/share_request.h"

#include "core/enums.h"
#include "core/ids.h"
#include "core/timestamp.h"
#include "core/uuid.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>

namespace fmgr::core {
  namespace {

    // ---- helpers ----

    ShareRequestId make_sr_id(std::string_view text) {
      return ShareRequestId::parse(text);
    }

    UserId make_user_id(std::string_view text) {
      return UserId::parse(text);
    }

    LabId make_lab_id(std::string_view text) {
      return LabId::parse(text);
    }

    Timestamp ts(std::int64_t micros) {
      return Timestamp::from_unix_micros(micros);
    }

    // ---- ShareRequestStatus tests ----

    TEST(ShareRequestStatusTest, AllValuesRoundTripThroughString) {
      EXPECT_EQ(parse_share_request_status(to_string(ShareRequestStatus::Pending)),
                ShareRequestStatus::Pending);
      EXPECT_EQ(parse_share_request_status(to_string(ShareRequestStatus::Approved)),
                ShareRequestStatus::Approved);
      EXPECT_EQ(parse_share_request_status(to_string(ShareRequestStatus::Rejected)),
                ShareRequestStatus::Rejected);
      EXPECT_EQ(parse_share_request_status(to_string(ShareRequestStatus::Revoked)),
                ShareRequestStatus::Revoked);
    }

    TEST(ShareRequestStatusTest, StringValuesAreSnakeCase) {
      EXPECT_EQ(to_string(ShareRequestStatus::Pending), "pending");
      EXPECT_EQ(to_string(ShareRequestStatus::Approved), "approved");
      EXPECT_EQ(to_string(ShareRequestStatus::Rejected), "rejected");
      EXPECT_EQ(to_string(ShareRequestStatus::Revoked), "revoked");
    }

    TEST(ShareRequestStatusTest, JsonRoundTrip) {
      const auto status = ShareRequestStatus::Approved;
      nlohmann::json json = status;
      EXPECT_EQ(json.get<std::string>(), "approved");
      EXPECT_EQ(json.get<ShareRequestStatus>(), status);
    }

    TEST(ShareRequestStatusTest, RejectsInvalidString) {
      using namespace fmgr::core;
      EXPECT_THROW((void)parse_share_request_status("invalid"), std::invalid_argument);
      EXPECT_THROW((void)parse_share_request_status(""), std::invalid_argument);
    }

    // ---- ShareApprovalRole tests ----

    TEST(ShareApprovalRoleTest, AllValuesRoundTripThroughString) {
      EXPECT_EQ(parse_share_approval_role(to_string(ShareApprovalRole::SourceAdmin)),
                ShareApprovalRole::SourceAdmin);
      EXPECT_EQ(parse_share_approval_role(to_string(ShareApprovalRole::TargetAdmin)),
                ShareApprovalRole::TargetAdmin);
      EXPECT_EQ(parse_share_approval_role(to_string(ShareApprovalRole::SystemAdmin)),
                ShareApprovalRole::SystemAdmin);
    }

    TEST(ShareApprovalRoleTest, StringValuesAreSnakeCase) {
      EXPECT_EQ(to_string(ShareApprovalRole::SourceAdmin), "source_admin");
      EXPECT_EQ(to_string(ShareApprovalRole::TargetAdmin), "target_admin");
      EXPECT_EQ(to_string(ShareApprovalRole::SystemAdmin), "system_admin");
    }

    TEST(ShareApprovalRoleTest, JsonRoundTrip) {
      const auto role = ShareApprovalRole::TargetAdmin;
      nlohmann::json json = role;
      EXPECT_EQ(json.get<std::string>(), "target_admin");
      EXPECT_EQ(json.get<ShareApprovalRole>(), role);
    }

    TEST(ShareApprovalRoleTest, RejectsInvalidString) {
      using namespace fmgr::core;
      EXPECT_THROW((void)parse_share_approval_role("invalid"), std::invalid_argument);
      EXPECT_THROW((void)parse_share_approval_role(""), std::invalid_argument);
    }

    // ---- ShareRequestApprovalId tests ----

    TEST(ShareRequestApprovalIdTest, CompositeKeyOrdering) {
      const auto sr1 = make_sr_id("00000000-0000-0000-0000-000000000001");
      const auto sr2 = make_sr_id("00000000-0000-0000-0000-000000000002");

      const ShareRequestApprovalId a{sr1, ShareApprovalRole::SourceAdmin};
      const ShareRequestApprovalId b{sr1, ShareApprovalRole::TargetAdmin};
      const ShareRequestApprovalId c{sr2, ShareApprovalRole::SourceAdmin};

      EXPECT_LT(a, b);
      EXPECT_LT(a, c);
      EXPECT_LT(b, c);
      EXPECT_EQ(a, a);
    }

    TEST(ShareRequestApprovalIdTest, UsableAsMapKey) {
      const auto sr = make_sr_id("00000000-0000-0000-0000-000000000001");
      std::map<ShareRequestApprovalId, int> m;
      m[{sr, ShareApprovalRole::SourceAdmin}] = 1;
      m[{sr, ShareApprovalRole::TargetAdmin}] = 2;
      m[{sr, ShareApprovalRole::SystemAdmin}] = 3;
      EXPECT_EQ(m.size(), 3u);
      EXPECT_EQ(m.at({sr, ShareApprovalRole::TargetAdmin}), 2);
    }

    TEST(ShareRequestApprovalIdTest, JsonRoundTrip) {
      const auto sr = make_sr_id("00000000-0000-0000-0000-000000000001");
      const ShareRequestApprovalId id{sr, ShareApprovalRole::SystemAdmin};

      nlohmann::json json = id;
      const auto restored = json.get<ShareRequestApprovalId>();
      EXPECT_EQ(restored, id);
    }

    // ---- ShareRequestApproval tests ----

    TEST(ShareRequestApprovalTest, IdMethodReturnsCompositeKey) {
      const auto sr = make_sr_id("00000000-0000-0000-0000-000000000001");
      const auto user = make_user_id("00000000-0000-0000-0000-000000000002");
      const ShareRequestApproval approval{
          .share_request_id = sr,
          .approver_role = ShareApprovalRole::SourceAdmin,
          .approver_user_id = user,
          .decided_at = ts(1000),
          .note = std::nullopt,
      };
      const auto expected = ShareRequestApprovalId{sr, ShareApprovalRole::SourceAdmin};
      EXPECT_EQ(approval.id(), expected);
    }

    TEST(ShareRequestApprovalTest, JsonRoundTripWithNote) {
      const auto sr = make_sr_id("00000000-0000-0000-0000-000000000001");
      const auto user = make_user_id("00000000-0000-0000-0000-000000000002");
      const ShareRequestApproval original{
          .share_request_id = sr,
          .approver_role = ShareApprovalRole::TargetAdmin,
          .approver_user_id = user,
          .decided_at = ts(2000000),
          .note = "LGTM",
      };
      nlohmann::json json = original;
      const auto restored = json.get<ShareRequestApproval>();
      EXPECT_EQ(restored, original);
    }

    TEST(ShareRequestApprovalTest, JsonRoundTripNullNote) {
      const auto sr = make_sr_id("00000000-0000-0000-0000-000000000001");
      const auto user = make_user_id("00000000-0000-0000-0000-000000000002");
      const ShareRequestApproval original{
          .share_request_id = sr,
          .approver_role = ShareApprovalRole::SystemAdmin,
          .approver_user_id = user,
          .decided_at = ts(3000000),
          .note = std::nullopt,
      };
      nlohmann::json json = original;
      EXPECT_TRUE(json.at("note").is_null());
      const auto restored = json.get<ShareRequestApproval>();
      EXPECT_EQ(restored, original);
    }

    // ---- ShareRequest tests ----

    TEST(ShareRequestTest, JsonRoundTripAllFieldsSet) {
      const ShareRequest original{
          .id = make_sr_id("00000000-0000-0000-0000-000000000001"),
          .source_lab_id = make_lab_id("00000000-0000-0000-0000-000000000010"),
          .target_lab_id = make_lab_id("00000000-0000-0000-0000-000000000011"),
          .requested_by = make_user_id("00000000-0000-0000-0000-000000000020"),
          .scope_json = R"({"project_ids":["abc"]})",
          .status = ShareRequestStatus::Pending,
          .created_at = ts(1000000),
          .decided_at = ts(2000000),
      };
      nlohmann::json json = original;
      const auto restored = json.get<ShareRequest>();
      EXPECT_EQ(restored, original);
    }

    TEST(ShareRequestTest, JsonRoundTripNullDecidedAt) {
      const ShareRequest original{
          .id = make_sr_id("00000000-0000-0000-0000-000000000001"),
          .source_lab_id = make_lab_id("00000000-0000-0000-0000-000000000010"),
          .target_lab_id = make_lab_id("00000000-0000-0000-0000-000000000011"),
          .requested_by = make_user_id("00000000-0000-0000-0000-000000000020"),
          .scope_json = "{}",
          .status = ShareRequestStatus::Pending,
          .created_at = ts(1000000),
          .decided_at = std::nullopt,
      };
      nlohmann::json json = original;
      EXPECT_TRUE(json.at("decided_at").is_null());
      const auto restored = json.get<ShareRequest>();
      EXPECT_EQ(restored, original);
    }

    TEST(ShareRequestTest, DefaultStatusIsPending) {
      const ShareRequest sr{
          .id = make_sr_id("00000000-0000-0000-0000-000000000001"),
          .source_lab_id = make_lab_id("00000000-0000-0000-0000-000000000010"),
          .target_lab_id = make_lab_id("00000000-0000-0000-0000-000000000011"),
          .requested_by = make_user_id("00000000-0000-0000-0000-000000000020"),
          .created_at = ts(1000000),
      };
      EXPECT_EQ(sr.status, ShareRequestStatus::Pending);
    }

  } // namespace
} // namespace fmgr::core
