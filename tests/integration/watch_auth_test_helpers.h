// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef FMGR_TEST_WATCH_AUTH_TEST_HELPERS_H
#define FMGR_TEST_WATCH_AUTH_TEST_HELPERS_H

#include "auth/IAuthProvider.h"
#include "core/identity.h"
#include "core/role.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/SessionTraits.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace fmgr::test {

  // Test-only seam: stop the next background check before its authoritative read.
  // Ordinary admission delegates unchanged. Bounded waits also release the server
  // if a test assertion exits early, so fixture shutdown cannot deadlock.
  class WatchAuthBarrier final : public auth::IAuthProvider {
  public:
    explicit WatchAuthBarrier(auth::IAuthProvider& provider) : provider_(provider) {}

    void arm() {
      std::scoped_lock lock(mutex_);
      armed_ = true;
      arrived_ = false;
      released_ = false;
    }
    [[nodiscard]] bool wait_for_check() {
      std::unique_lock lock(mutex_);
      return cv_.wait_for(lock, std::chrono::seconds(5), [&] { return arrived_; });
    }
    void release() {
      std::scoped_lock lock(mutex_);
      released_ = true;
      cv_.notify_all();
    }

    auth::AuthToken authenticate(const auth::AuthCredentials& creds,
                                 const auth::ClientInfo& client) override {
      return provider_.authenticate(creds, client);
    }
    auth::SessionContext validate_token(std::string_view token) override {
      return provider_.validate_token(token);
    }
    auth::SessionContext revalidate_token(std::string_view token) override {
      {
        std::unique_lock lock(mutex_);
        if (armed_) {
          armed_ = false;
          arrived_ = true;
          cv_.notify_all();
          cv_.wait_for(lock, std::chrono::seconds(5), [&] { return released_; });
        }
      }
      return provider_.revalidate_token(token);
    }
    void verify_totp(const core::SessionId& id, std::string_view code) override {
      provider_.verify_totp(id, code);
    }
    void revoke_session(const core::SessionId& id, const storage::MutationContext& ctx) override {
      provider_.revoke_session(id, ctx);
    }
    void revoke_all_sessions(const core::UserId& id, const storage::MutationContext& ctx) override {
      provider_.revoke_all_sessions(id, ctx);
    }
    auth::ApiTokenResult create_api_token(const core::UserId& user, const std::string& name,
                                          const std::string& scope, std::optional<core::LabId> lab,
                                          std::optional<core::Timestamp> expires,
                                          const storage::MutationContext& ctx) override {
      return provider_.create_api_token(user, name, scope, lab, expires, ctx);
    }
    void revoke_api_token(const core::ApiTokenId& id,
                          const storage::MutationContext& ctx) override {
      provider_.revoke_api_token(id, ctx);
    }

  private:
    auth::IAuthProvider& provider_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool armed_{false};
    bool arrived_{false};
    bool released_{false};
  };

  enum class WatchInvalidation {
    Revocation,
    IdleExpiry,
    AbsoluteExpiry,
    Membership,
    Permission,
    Mfa,
    DisabledUser
  };

  inline void invalidate_watch_authority(storage::IStorageBackend& backend,
                                         auth::IAuthProvider& provider,
                                         const auth::SessionContext& session,
                                         const core::LabId& lab, core::RoleKind role,
                                         core::Permission permission, WatchInvalidation change) {
    const storage::MutationContext mutation{
        .actor_user_id = session.user_id,
        .actor_session_id = session.session_id.to_string(),
        .request_id = "watch-invalidation",
        .reason = "synthetic watch authorization regression",
        .lab_id = lab.to_string(),
    };
    if (change == WatchInvalidation::Revocation) {
      provider.revoke_session(session.session_id, mutation);
      return;
    }
    const bool changes_session = change == WatchInvalidation::IdleExpiry ||
                                 change == WatchInvalidation::AbsoluteExpiry ||
                                 change == WatchInvalidation::Mfa;
    auto txn = changes_session ? backend.begin_auth_transaction({session.user_id})
                               : backend.begin(storage::IsolationLevel::Serializable);
    if (change == WatchInvalidation::Membership) {
      txn->repo<core::LabMembership>().soft_delete(
          core::LabMembershipId{.user_id = session.user_id, .lab_id = lab}, mutation);
    } else if (change == WatchInvalidation::Permission) {
      txn->repo<core::RolePermission>().soft_delete(
          core::RolePermissionId{.role_id = core::builtin_role_id(role), .permission = permission},
          mutation);
    } else if (change == WatchInvalidation::DisabledUser) {
      auto user = txn->repo<core::User>().find_by_id(session.user_id).value();
      user.status = core::UserStatus::Disabled;
      txn->repo<core::User>().update(user, mutation);
    } else {
      auto row = txn->repo<core::Session>().find_by_id(session.session_id).value();
      const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      if (change == WatchInvalidation::IdleExpiry) {
        row.last_seen_at = core::Timestamp::from_unix_micros(now - 13LL * 3600 * 1'000'000);
      } else if (change == WatchInvalidation::AbsoluteExpiry) {
        row.created_at = core::Timestamp::from_unix_micros(now - 8LL * 24 * 3600 * 1'000'000);
      } else {
        row.mfa_complete = false;
      }
      txn->repo<core::Session>().update(row, mutation);
    }
    txn->commit();
  }

} // namespace fmgr::test
#endif
