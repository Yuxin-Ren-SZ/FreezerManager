// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cli/UserCommands.h"

#include "auth/LocalAuthProvider.h"
#include "core/identity.h"
#include "storage/IStorageBackend.h"
#include "storage/IdentityTraits.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <exception>
#include <string>
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

  int run_user_set_password(storage::IStorageBackend& backend, const std::string& email,
                            const std::string& password,
                            const std::optional<core::UserId>& actor, std::ostream& out,
                            std::ostream& err) {
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
        .lab_id = user.default_lab_id.has_value() ? user.default_lab_id->to_string() : std::string{},
    };
    txn->repo<core::User>().update(user, ctx);
    txn->commit();

    out << "set local password for " << lower_email << " (" << user.id.to_string() << ")\n";
    return 0;
  }

} // namespace fmgr::cli
