// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_MFA_LIFECYCLE_H
#define FMGR_STORAGE_DETAIL_MFA_LIFECYCLE_H

#include "core/identity.h"
#include "core/session.h"
#include "storage/IdentityTraits.h"
#include "storage/MfaSecurityStateTraits.h"
#include "storage/SessionTraits.h"

namespace fmgr::storage {
  class SqliteTransaction;
  class PostgresTransaction;
} // namespace fmgr::storage

namespace fmgr::storage::detail {

  // Only backend-owned lifecycle entry points can construct this capability.
  // Ordinary typed repositories cannot mint proof, state or generations.
  class MfaWriteCapability final {
    friend class fmgr::storage::SqliteTransaction;
    friend class fmgr::storage::PostgresTransaction;
    MfaWriteCapability() = default;
  };

  class MfaStateWriter {
  public:
    virtual ~MfaStateWriter() = default;
    virtual void write_state(const MfaWriteCapability&, const core::MfaSecurityState&,
                             const MutationContext&, bool initialize) = 0;
  };

  class TotpSecretWriter {
  public:
    virtual ~TotpSecretWriter() = default;
    virtual void write_secret(const MfaWriteCapability&, const core::UserId&,
                              const std::optional<std::string>&) = 0;
  };

  class TotpSessionWriter {
  public:
    virtual ~TotpSessionWriter() = default;
    virtual void write_session(const MfaWriteCapability&, const core::Session&,
                               const MutationContext&, const std::string& action) = 0;
  };

  template <typename Entity> [[nodiscard]] nlohmann::json auth_snapshot(const Entity& entity) {
    return nlohmann::json(entity);
  }

  [[nodiscard]] inline nlohmann::json auth_snapshot(const core::User& user) {
    return nlohmann::json{{"id", user.id},
                          {"primary_email", user.primary_email},
                          {"display_name", user.display_name},
                          {"status", user.status},
                          {"created_at", user.created_at},
                          {"default_lab_id", core::json_helpers::opt_to_json(user.default_lab_id)},
                          {"authz_version", user.authz_version}};
  }

  [[nodiscard]] inline nlohmann::json auth_snapshot(const core::Session& session) {
    return nlohmann::json{
        {"id", session.id},
        {"user_id", session.user_id},
        {"mfa_complete", session.mfa_complete},
        {"created_at", session.created_at},
        {"last_seen_at", session.last_seen_at},
        {"revoked_at", core::json_helpers::opt_to_json(session.revoked_at)},
        {"totp_verified_generation",
         core::json_helpers::opt_to_json(session.totp_verified_generation)},
        {"totp_verified_at", core::json_helpers::opt_to_json(session.totp_verified_at)}};
  }

  inline void require_mfa_owner_lock(ITransaction& txn, const core::UserId& user_id) {
    if (txn.auth_user_lock_status(user_id) != AuthUserLockStatus::Locked) {
      throw ConstraintViolation("MFA mutation requires its owning user lock");
    }
  }

  inline void validate_mfa_state(const core::MfaSecurityState& state) {
    if (state.failure_count < 0 || state.failure_count > 17 ||
        (state.last_accepted_step && *state.last_accepted_step < 0) ||
        (state.observed_at && state.observed_at->unix_micros() < 0) ||
        (state.next_allowed_at && state.next_allowed_at->unix_micros() < 0) ||
        ((state.last_accepted_step || state.failure_count != 0 || state.next_allowed_at) &&
         !state.observed_at)) {
      throw ConstraintViolation("invalid MFA security state");
    }
  }

  inline void validate_mfa_update(const core::MfaSecurityState& before,
                                  const core::MfaSecurityState& after) {
    validate_mfa_state(after);
    if (before.user_id != after.user_id || before.totp_generation != after.totp_generation ||
        (before.last_accepted_step &&
         (!after.last_accepted_step || *after.last_accepted_step < *before.last_accepted_step)) ||
        (before.observed_at && (!after.observed_at || *after.observed_at < *before.observed_at))) {
      throw ConstraintViolation("MFA state cannot regress or change enrollment generation");
    }
  }

  [[nodiscard]] inline std::string mfa_state_column(core::MfaSecurityState::Field field) {
    switch (field) {
    case core::MfaSecurityState::Field::UserId:
      return "user_id";
    case core::MfaSecurityState::Field::LastAcceptedStep:
      return "last_accepted_step";
    case core::MfaSecurityState::Field::FailureCount:
      return "failure_count";
    case core::MfaSecurityState::Field::NextAllowedAt:
      return "next_allowed_at_micros";
    case core::MfaSecurityState::Field::ObservedAt:
      return "observed_at_micros";
    case core::MfaSecurityState::Field::TotpGeneration:
      return "totp_generation";
    }
    throw ConstraintViolation("unknown MFA state field");
  }

  [[nodiscard]] inline core::MfaSecurityState
  initialize_mfa_state(ITransaction& txn, const MfaWriteCapability& capability,
                       const core::UserId& user_id, const MutationContext& context) {
    require_mfa_owner_lock(txn, user_id);
    auto& repo = txn.repo<core::MfaSecurityState>();
    if (const auto existing = repo.find_by_id(user_id)) {
      return *existing;
    }
    const auto user = txn.repo<core::User>().find_by_id(user_id);
    if (!user || user->status != core::UserStatus::Active) {
      throw ConstraintViolation("MFA initialization requires an active locked user");
    }
    core::MfaSecurityState state{.user_id = user_id};
    if (user->totp_secret_enc) {
      state.totp_generation = core::Uuid::parse(core::generate_uuid_v4());
    }
    dynamic_cast<MfaStateWriter&>(repo).write_state(capability, state, context, true);
    return state;
  }

  inline void replace_totp_secret(ITransaction& txn, const MfaWriteCapability& capability,
                                  const core::UserId& user_id,
                                  const std::optional<std::string>& secret,
                                  const MutationContext& context, core::Timestamp now) {
    require_mfa_owner_lock(txn, user_id);
    if (secret && secret->empty()) {
      throw ConstraintViolation("TOTP secret cannot be empty");
    }
    const auto before = initialize_mfa_state(txn, capability, user_id, context);
    auto state = before;
    state.totp_generation =
        secret ? std::make_optional(core::Uuid::parse(core::generate_uuid_v4())) : std::nullopt;
    dynamic_cast<TotpSecretWriter&>(txn.repo<core::User>())
        .write_secret(capability, user_id, secret);
    dynamic_cast<MfaStateWriter&>(txn.repo<core::MfaSecurityState>())
        .write_state(capability, state, context, false);
    txn.note_mutation(
        "user", user_id.to_string(), context, "auth.totp_secret_changed",
        AuditSnapshot{.before = nlohmann::json{{"enrolled", before.totp_generation.has_value()},
                                               {"generation", core::json_helpers::opt_to_json(
                                                                  before.totp_generation)}},
                      .after = nlohmann::json{
                          {"enrolled", state.totp_generation.has_value()},
                          {"generation", core::json_helpers::opt_to_json(state.totp_generation)}}});
    auto& sessions = txn.repo<core::Session>();
    const auto query = Query<core::Session>::where(
        field<core::Session, core::UserId>(core::Session::Field::UserId) == user_id);
    for (const auto& candidate : sessions.query(query)) {
      auto session = sessions.find_by_id(candidate.id).value();
      if (session.user_id != user_id || session.revoked_at) {
        continue;
      }
      session.revoked_at = now;
      dynamic_cast<TotpSessionWriter&>(sessions).write_session(capability, session, context,
                                                               "auth.session_revoked");
    }
  }

  inline void complete_totp_session(ITransaction& txn, const MfaWriteCapability& capability,
                                    const core::SessionId& session_id, const core::Uuid& generation,
                                    core::Timestamp verified_at, const MutationContext& context) {
    auto& sessions = txn.repo<core::Session>();
    auto session = sessions.find_by_id(session_id);
    if (!session) {
      throw NotFound("session not found");
    }
    require_mfa_owner_lock(txn, session->user_id);
    const auto user = txn.repo<core::User>().find_by_id(session->user_id);
    const auto state = txn.repo<core::MfaSecurityState>().find_by_id(session->user_id);
    if (!user || user->status != core::UserStatus::Active || !user->totp_secret_enc || !state ||
        !state->totp_generation || *state->totp_generation != generation || session->revoked_at ||
        session->mfa_complete || verified_at < session->last_seen_at ||
        verified_at < session->created_at || verified_at.unix_micros() < 0) {
      throw ConstraintViolation(
          "TOTP completion requires a live pending current-generation session");
    }
    session->mfa_complete = true;
    session->totp_verified_generation = generation;
    session->totp_verified_at = verified_at;
    session->last_seen_at = verified_at;
    dynamic_cast<TotpSessionWriter&>(sessions).write_session(capability, *session, context,
                                                             "auth.mfa_completed");
  }
} // namespace fmgr::storage::detail
#endif
