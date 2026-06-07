// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef FMGR_STORAGE_DETAIL_SESSIONCOLUMNS_H
#define FMGR_STORAGE_DETAIL_SESSIONCOLUMNS_H

#include "core/session.h"
#include "storage/IStorageBackend.h"

#include <string>

// Backend-neutral column-name mapping and pure validation for the auth-session
// entities (Session, ApiToken). Column names are identical across both schemas.
namespace fmgr::storage::detail {

  [[nodiscard]] inline std::string session_column_name(core::Session::Field field) {
    switch (field) {
    case core::Session::Field::Id:
      return "id";
    case core::Session::Field::UserId:
      return "user_id";
    case core::Session::Field::TokenHash:
      return "token_hash";
    case core::Session::Field::TokenPrefix:
      return "token_prefix";
    case core::Session::Field::CreatedAt:
      return "created_at_micros";
    case core::Session::Field::LastSeenAt:
      return "last_seen_at_micros";
    case core::Session::Field::Ip:
      return "ip";
    case core::Session::Field::UserAgent:
      return "user_agent";
    case core::Session::Field::RevokedAt:
      return "revoked_at_micros";
    case core::Session::Field::MfaComplete:
      return "mfa_complete";
    }
    throw ConstraintViolation("unknown session field");
  }

  [[nodiscard]] inline std::string api_token_column_name(core::ApiToken::Field field) {
    switch (field) {
    case core::ApiToken::Field::Id:
      return "id";
    case core::ApiToken::Field::UserId:
      return "user_id";
    case core::ApiToken::Field::LabId:
      return "lab_id";
    case core::ApiToken::Field::Name:
      return "name";
    case core::ApiToken::Field::ScopeJson:
      return "scope_json";
    case core::ApiToken::Field::TokenHash:
      return "token_hash";
    case core::ApiToken::Field::TokenPrefix:
      return "token_prefix";
    case core::ApiToken::Field::CreatedAt:
      return "created_at_micros";
    case core::ApiToken::Field::ExpiresAt:
      return "expires_at_micros";
    case core::ApiToken::Field::RevokedAt:
      return "revoked_at_micros";
    }
    throw ConstraintViolation("unknown api_token field");
  }

  inline void validate_session(const core::Session& session) {
    if (session.token_hash.empty()) {
      throw ConstraintViolation("session token_hash is required");
    }
    if (session.token_prefix.empty()) {
      throw ConstraintViolation("session token_prefix is required");
    }
  }

  inline void validate_api_token(const core::ApiToken& token) {
    if (token.token_hash.empty()) {
      throw ConstraintViolation("api_token token_hash is required");
    }
    if (token.token_prefix.empty()) {
      throw ConstraintViolation("api_token token_prefix is required");
    }
    if (token.name.empty()) {
      throw ConstraintViolation("api_token name is required");
    }
  }

} // namespace fmgr::storage::detail

#endif // FMGR_STORAGE_DETAIL_SESSIONCOLUMNS_H
