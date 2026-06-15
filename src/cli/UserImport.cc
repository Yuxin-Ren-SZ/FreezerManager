// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/UserImport.h"

#include "core/enums.h"
#include "core/uuid.h"

#include <optional>
#include <string>

namespace fmgr::cli {

  namespace {

    [[nodiscard]] core::User row_to_user(const HeaderIndex& header,
                                         const std::vector<std::string>& row, core::Timestamp now) {
      const auto email = cell(header, row, "primary_email");
      if (!email.has_value()) {
        throw RowError{"primary_email: required and must be non-empty"};
      }
      const auto display_name = cell(header, row, "display_name");
      if (!display_name.has_value()) {
        throw RowError{"display_name: required and must be non-empty"};
      }

      core::User user{};
      user.id = core::UserId::parse(core::generate_uuid_v4());
      user.primary_email = email.value();
      user.display_name = display_name.value();
      user.status = core::UserStatus::Active;
      user.created_at = now;

      if (const auto text = cell(header, row, "default_lab_id"); text.has_value()) {
        user.default_lab_id = parse_id<core::LabId>(text.value(), "default_lab_id");
      }
      return user;
    }

  } // namespace

  EntityImportReport<core::User>
  build_user_import(const std::vector<std::vector<std::string>>& records, core::LabId /*lab_id*/,
                    core::Timestamp now) {
    EntityImportReport<core::User> report;
    if (records.empty()) {
      report.header_error = "empty CSV: no header row";
      return report;
    }
    const HeaderIndex header = index_header(records.front());
    if (!header.contains("primary_email") || !header.contains("display_name")) {
      report.header_error = "header must contain 'primary_email' and 'display_name' columns";
      return report;
    }

    for (std::size_t index = 1; index < records.size(); ++index) {
      EntityImportRow<core::User> result;
      result.row_number = index;
      try {
        result.entity = row_to_user(header, records.at(index), now);
        result.ok = true;
      } catch (const RowError& error) {
        result.error = error.message;
      }
      report.rows.push_back(std::move(result));
    }
    return report;
  }

} // namespace fmgr::cli
