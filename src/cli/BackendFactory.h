// SPDX-License-Identifier: AGPL-3.0-or-later

// Opens a fully-migrated storage backend (SQLite file or Postgres URL) with the
// domain repositories registered, from CLI flags. Higher layers depend only on
// IStorageBackend — the concrete backend choice stays behind this factory.
#ifndef FMGR_CLI_BACKENDFACTORY_H
#define FMGR_CLI_BACKENDFACTORY_H

#include "storage/IStorageBackend.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace fmgr::cli {

  // Exactly one of the two targets must be set; supplying neither or both is a
  // usage error.
  struct BackendOptions {
    std::optional<std::string> sqlite_path;
    std::optional<std::string> postgres_url;
  };

  // Raised on invalid backend selection (neither/both targets).
  class BackendOptionError : public std::runtime_error {
  public:
    explicit BackendOptionError(const std::string& message) : std::runtime_error(message) {}
  };

  // Open + migrate + register repositories. Throws BackendOptionError on bad
  // flags and storage::BackendError on connection/migration failures.
  [[nodiscard]] std::unique_ptr<storage::IStorageBackend>
  open_backend(const BackendOptions& options);

} // namespace fmgr::cli

#endif // FMGR_CLI_BACKENDFACTORY_H
