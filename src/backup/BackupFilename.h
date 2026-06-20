// SPDX-License-Identifier: AGPL-3.0-or-later

// Backup file naming convention. A backup's creation time is encoded in its
// filename so the scheduler can enumerate a backup directory and apply the
// retention policy (RetentionPolicy) without decrypting a single file:
//
//   fmgr-YYYYMMDDThhmmssZ.fmgrbak      (UTC, second granularity)
//
// e.g. fmgr-20260619T031500Z.fmgrbak. The timestamp is always UTC ("Z"); the
// microsecond component is truncated to whole seconds, which is plenty for a
// backup cadence measured in minutes.
#ifndef FMGR_BACKUP_BACKUPFILENAME_H
#define FMGR_BACKUP_BACKUPFILENAME_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fmgr::backup {

  // Build the canonical backup filename for a creation time given in microseconds
  // since the Unix epoch (UTC). The sub-second part is truncated.
  [[nodiscard]] std::string make_backup_filename(std::int64_t created_micros);

  // Parse the creation time (microseconds since the Unix epoch, second-aligned)
  // out of a backup filename produced by make_backup_filename. Returns nullopt if
  // `name` does not match the convention. A leading directory path is rejected —
  // pass the filename component only.
  [[nodiscard]] std::optional<std::int64_t> parse_backup_timestamp(std::string_view name);

} // namespace fmgr::backup

#endif // FMGR_BACKUP_BACKUPFILENAME_H
