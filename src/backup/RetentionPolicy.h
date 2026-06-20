// SPDX-License-Identifier: AGPL-3.0-or-later

// Grandfather-Father-Son (GFS) backup retention (PRD §14, default 30 daily /
// 12 monthly / 7 yearly).
//
// Given the set of backups present in a directory (each with its creation time,
// recovered from the filename via BackupFilename — no decryption needed), this
// decides which ones to prune. A backup is *kept* when it is the newest backup
// in its UTC day and that day is among the `daily` most-recent days that hold a
// backup, OR likewise for its month (`monthly`) or year (`yearly`). Every other
// backup is selected for deletion.
//
// The newest backup overall is always kept, regardless of the policy counts, so
// a misconfigured (e.g. all-zero) policy can never delete the latest snapshot.
//
// Pure and side-effect free: it returns the list to delete; the caller performs
// the filesystem removal. This makes the data-safety-critical decision unit- and
// property-testable in isolation.
#ifndef FMGR_BACKUP_RETENTIONPOLICY_H
#define FMGR_BACKUP_RETENTIONPOLICY_H

#include <cstdint>
#include <string>
#include <vector>

namespace fmgr::backup {

  struct RetentionPolicy {
    int daily{30};
    int monthly{12};
    int yearly{7};
  };

  struct BackupFile {
    std::string path;
    std::int64_t created_micros{};
  };

  // Return the subset of `files` that the policy prunes (i.e. does NOT retain),
  // in the same relative order as the input. `now_micros` anchors "most recent";
  // files dated after `now_micros` are always retained. Files are matched by
  // position, so duplicate paths/timestamps are handled independently.
  [[nodiscard]] std::vector<BackupFile> select_for_deletion(const std::vector<BackupFile>& files,
                                                            const RetentionPolicy& policy,
                                                            std::int64_t now_micros);

} // namespace fmgr::backup

#endif // FMGR_BACKUP_RETENTIONPOLICY_H
