// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_STORAGE_BACKENDFACTORY_H
#define FMGR_STORAGE_BACKENDFACTORY_H

#include "storage/IStorageBackend.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace fmgr::storage {

  // Exactly one backend target must be set. Postgres passwords should be supplied
  // out-of-band via FMGR_PG_PASSWORD/.pgpass rather than embedded in URLs.
  struct BackendOptions {
    std::optional<std::string> sqlite_path;
    std::optional<std::string> postgres_url;
    // Optional guard for CLI/import workflows: sqlite_path must resolve under this
    // directory, preventing accidental `..` or symlink escape.
    std::optional<std::string> data_dir;
  };

  class BackendOptionError : public std::runtime_error {
  public:
    explicit BackendOptionError(const std::string& message) : std::runtime_error(message) {}
  };

  [[nodiscard]] std::unique_ptr<IStorageBackend> open_backend(const BackendOptions& options);

} // namespace fmgr::storage

#endif // FMGR_STORAGE_BACKENDFACTORY_H
