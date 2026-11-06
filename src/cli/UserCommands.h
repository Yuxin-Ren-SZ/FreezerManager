// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_CLI_USERCOMMANDS_H
#define FMGR_CLI_USERCOMMANDS_H

#include <istream>
#include <optional>
#include <ostream>
#include <string>

#include "core/identity.h"

namespace fmgr::storage {
  class IStorageBackend;
}

namespace fmgr::cli {

  // Enrol a local password for an existing user: hashes the password (Argon2id via
  // LocalAuthProvider::hash_password) and writes the `{"provider":"local","hash":…}`
  // entry into the user's auth_bindings that LocalAuthProvider::authenticate
  // expects. Re-enrol overwrites any existing local binding.
  //
  // Looks the user up by the lowercased email (authenticate lowercases the input
  // before lookup, so this guarantees the password is reachable at login). When
  // `actor` is empty the target user is recorded as the audit actor (self-enrol).
  //
  // Returns 0 on success; 1 (with a message on `err`) if the password is too short
  // or no user has that email.
  int run_user_set_password(storage::IStorageBackend& backend, const std::string& email,
                            const std::string& password, const std::optional<core::UserId>& actor,
                            std::ostream& out, std::ostream& err);

  // Trusted local provisioning only. Secret input is bounded stdin or a regular
  // owner-only file, never an argv value. NULL removes enrollment through the
  // same audited lifecycle; every explicit replacement revokes active sessions.
  [[nodiscard]] std::string read_totp_secret_input(const std::string& path, std::istream& input);
  int run_user_set_totp(storage::IStorageBackend& backend, const std::string& email,
                        const std::optional<std::string>& secret,
                        const std::optional<core::UserId>& actor, std::ostream& out,
                        std::ostream& err);

} // namespace fmgr::cli

#endif // FMGR_CLI_USERCOMMANDS_H
