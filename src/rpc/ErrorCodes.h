// SPDX-License-Identifier: AGPL-3.0-or-later

// Wire codes for refusals whose gRPC status code does not identify the state.
//
// A gRPC status code is a classification, not an interface: several distinct
// situations collapse into one code, and a client that must react differently to
// them has nothing machine-readable to branch on. This header is where such a
// state gets a name that both the layer that produces it and the layer that
// translates it can read, so the two cannot drift into two copies of a string.
//
// It has no dependencies on purpose: `server/GrpcErrorTranslation.h` (the
// producer) and `rest/RestErrorTranslation.h` (the translator) sit in different
// libraries, and neither should have to pull in the other's dependency graph to
// spell one constant.
#ifndef FMGR_RPC_ERRORCODES_H
#define FMGR_RPC_ERRORCODES_H

#include <string_view>

namespace fmgr::rpc {

  // A second factor is outstanding (#140, on top of #62).
  //
  // `auth::MfaRequired` deliberately maps to UNAUTHENTICATED: a pending-MFA
  // session is a credential that is not usable yet, no RPC's authentication
  // semantics change, and every existing client keeps working. The cost is that
  // the gRPC code cannot tell a pending second factor from an expired or revoked
  // session — and a client that must *resume* the TOTP prompt rather than
  // dead-end has to know which it is.
  //
  // So the state travels twice, from this one definition:
  //   * `k_mfa_required_marker` prefixes the gRPC status message, which is what a
  //     gRPC client or a log sees;
  //   * the REST gateway turns that prefix into the envelope code
  //     `k_mfa_required_code` next to HTTP 401, which is what the SPA branches on.
  //
  // A test pins each end: tests/unit/error_translation_test.cpp (the prefix),
  // tests/unit/rest_error_translation_test.cpp (the code, and that nothing else
  // gets it), tests/integration/rest_gateway_integration_test.cpp (end to end).
  inline constexpr std::string_view k_mfa_required_marker = "mfa_required: ";
  inline constexpr std::string_view k_mfa_required_code = "MFA_REQUIRED";

} // namespace fmgr::rpc

#endif // FMGR_RPC_ERRORCODES_H
