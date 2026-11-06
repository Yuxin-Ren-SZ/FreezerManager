// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/UserCommands.h"

#include "auth/LocalAuthProvider.h"
#include "auth/Totp.h"
#include "core/identity.h"
#include "storage/IStorageBackend.h"
#include "storage/IdentityTraits.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <exception>
#include <fcntl.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace fmgr::cli {

  namespace {

    [[nodiscard]] std::string to_lower(std::string text) {
      for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return text;
    }

  } // namespace

  // email/password are distinct required inputs with unambiguous call sites; a swap
  // would fail fast (auth hash mismatch), so a strong-typedef wrapper is unwarranted.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  int run_user_set_password(storage::IStorageBackend& backend, const std::string& email,
                            const std::string& password, const std::optional<core::UserId>& actor,
                            std::ostream& out, std::ostream& err) {
    // authenticate() lowercases the email before lookup, so enrol against the same
    // normalized form to guarantee the password is reachable at login.
    const std::string lower_email = to_lower(email);

    // Hash first: a too-short password fails before we touch the row.
    auth::LocalAuthProvider provider(backend);
    std::string hash;
    try {
      hash = provider.hash_password(password);
    } catch (const std::exception& error) {
      err << "error: " << error.what() << '\n';
      return 1;
    }

    auto txn = backend.begin(storage::IsolationLevel::Serializable);
    const auto users = txn->repo<core::User>().query(storage::Query<core::User>::where(
        storage::field<core::User, std::string>(core::User::Field::PrimaryEmail) == lower_email));
    if (users.empty()) {
      err << "error: no user with email " << lower_email << '\n';
      return 1;
    }
    core::User user = users.front();

    // Drop any existing local binding so re-enrol overwrites it rather than leaving
    // a stale hash the authenticate loop would pick first.
    nlohmann::json bindings = nlohmann::json::array();
    for (const auto& binding : user.auth_bindings) {
      const bool is_local = binding.contains("provider") && binding.at("provider") == "local";
      if (!is_local) {
        bindings.push_back(binding);
      }
    }
    bindings.push_back({{"provider", "local"}, {"hash", hash}});
    user.auth_bindings = std::move(bindings);

    const core::UserId actor_id = actor.has_value() ? *actor : user.id;
    const storage::MutationContext ctx{
        .actor_user_id = actor_id,
        .actor_session_id = "freezerctl-set-password",
        .request_id = "",
        .reason = "cli_set_password",
        .lab_id =
            user.default_lab_id.has_value() ? user.default_lab_id->to_string() : std::string{},
    };
    txn->repo<core::User>().update(user, ctx);
    txn->commit();

    out << "set local password for " << lower_email << " (" << user.id.to_string() << ")\n";
    return 0;
  }

  namespace {
    [[nodiscard]] std::string read_secret_line(std::istream& input) {
      std::string secret;
      char byte = 0;
      while (input.get(byte)) {
        if (byte == '\n') {
          break;
        }
        if (secret.size() >= 512) {
          throw std::runtime_error("invalid TOTP secret input");
        }
        secret.push_back(byte);
      }
      if (input.bad() || secret.empty() || input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("invalid TOTP secret input");
      }
      return secret;
    }
  } // namespace

  std::string read_totp_secret_input(const std::string& path, std::istream& input) {
    if (path.empty() || path == "-") {
      return read_secret_line(input);
    }
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0) {
      throw std::runtime_error("cannot open protected TOTP secret file");
    }
    try {
      struct stat info {};
      if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
          (info.st_mode & 0077) != 0) {
        throw std::runtime_error("TOTP secret file must be a regular owner-only file");
      }
      std::string bytes;
      std::array<char, 514> buffer{};
      for (;;) {
        const auto count = ::read(descriptor, buffer.data(), buffer.size());
        if (count < 0) {
          throw std::runtime_error("cannot read protected TOTP secret file");
        }
        if (count == 0) {
          break;
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
        if (bytes.size() > 513) {
          throw std::runtime_error("invalid TOTP secret input");
        }
      }
      std::istringstream stream(bytes);
      auto secret = read_secret_line(stream);
      ::close(descriptor);
      return secret;
    } catch (...) {
      ::close(descriptor);
      throw;
    }
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  int run_user_set_totp(storage::IStorageBackend& backend, const std::string& email,
                        const std::optional<std::string>& secret,
                        const std::optional<core::UserId>& actor, std::ostream& out,
                        std::ostream& err) {
    try {
      if (secret) {
        (void)auth::totp_generate(*secret, 0);
      }
      const auto normalized = to_lower(email);
      auto lookup = backend.begin(storage::IsolationLevel::ReadCommitted);
      const auto users = lookup->repo<core::User>().query(storage::Query<core::User>::where(
          storage::field<core::User, std::string>(core::User::Field::PrimaryEmail) == normalized));
      lookup->commit();
      if (users.size() != 1) {
        throw std::runtime_error("TOTP provisioning failed");
      }
      const auto user_id = users.front().id;
      auto txn = backend.begin_auth_transaction({user_id});
      const auto user = txn->repo<core::User>().find_by_id(user_id);
      if (!user || user->primary_email != normalized || user->status != core::UserStatus::Active) {
        throw std::runtime_error("TOTP provisioning failed");
      }
      const storage::MutationContext context{.actor_user_id = actor.value_or(user_id),
                                             .actor_session_id = "freezerctl-set-totp",
                                             .request_id = "",
                                             .reason = "trusted_totp_provisioning"};
      txn->replace_totp_secret(user_id, secret, context);
      txn->commit();
      out << "TOTP provisioning completed\n";
      return 0;
    } catch (const std::exception&) {
      // Stored credentials and engine exceptions never escape this boundary.
      err << "error: TOTP provisioning failed\n";
      return 1;
    }
  }

} // namespace fmgr::cli
