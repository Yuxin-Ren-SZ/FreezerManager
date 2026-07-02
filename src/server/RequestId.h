// SPDX-License-Identifier: AGPL-3.0-or-later

// Correlation-id extraction for RPC handlers (security backlog C-12, PRD §17).
// The caller (REST gateway, Qt client, or a server-to-server peer) may supply an
// `x-request-id` gRPC metadata entry so a request can be traced across the log
// pipeline and the audit chain. When absent, we mint a fresh CSPRNG id so every
// mutation still carries a stable, unguessable correlation token.
#ifndef FMGR_SERVER_REQUESTID_H
#define FMGR_SERVER_REQUESTID_H

#include "core/uuid.h"

#include <grpcpp/grpcpp.h>

#include <string>

namespace fmgr::server {

  inline constexpr const char* k_request_id_metadata_key = "x-request-id";

  // Return the caller-supplied x-request-id, or a freshly generated v4 UUID when
  // none was sent. gRPC metadata keys are lowercase by spec, so the lookup is
  // exact. Empty values are treated as absent.
  [[nodiscard]] inline std::string request_id_from(const grpc::ServerContext& ctx) {
    const auto& metadata = ctx.client_metadata();
    const auto it = metadata.find(k_request_id_metadata_key);
    if (it != metadata.end() && it->second.length() > 0) {
      return {it->second.data(), it->second.length()};
    }
    return core::generate_uuid_v4();
  }

} // namespace fmgr::server

#endif // FMGR_SERVER_REQUESTID_H
