// SPDX-License-Identifier: AGPL-3.0-or-later

// REST/JSON gateway: registers `/api/v1/*` HTTP routes on the Drogon app and
// forwards each to the matching gRPC service over the in-process channel.
//
// Routing is verb-style and 1:1 with the gRPC methods (e.g. CreateLab ->
// POST /api/v1/lab/create) so the JSON<->proto mapping stays mechanical. All
// routes are POST and carry the request message as a JSON body; RESTful noun
// routes can be layered on later without touching the forwarding core.
//
// Auth: routes pass the inbound HTTP `Authorization: Bearer <token>` header
// straight through as gRPC `authorization` metadata, so the existing
// AuthMiddleware gate runs unchanged. auth/login + auth/submit-mfa are the only
// routes that work without a bearer (they establish one).
#ifndef FMGR_REST_RESTGATEWAY_H
#define FMGR_REST_RESTGATEWAY_H

#include "obs/Health.h"
#include "rest/GatewayStubs.h"

#include <functional>
#include <optional>
#include <string_view>

namespace fmgr::rest {

  // Decides whether a caller may see privileged operational detail (the detailed
  // /health readiness report or /metrics) when that endpoint is not public.
  // Given the raw `Authorization` header value, or nullopt when it is absent.
  // Returns true to allow. Kept header-type agnostic so it is unit-testable
  // without a live Drogon request.
  using OpsAuthorizer = std::function<bool(std::optional<std::string_view>)>;

  class RestGateway {
  public:
    explicit RestGateway(GatewayStubs& stubs) : stubs_(stubs) {}

    // Register every `/api/v1/*` route on the process-wide Drogon app. Call once
    // before drogon::app().run(). The referenced GatewayStubs must outlive the
    // running app.
    void register_routes();

    // Register the GET /api/v1/health readiness endpoint backed by `probe`. The
    // probe is copied and owned by the handler, so whatever it captures must
    // outlive the running app. 200 when healthy, 503 when a dependency is down
    // (PRD §17). When `public_readiness` is false the detailed report — which can
    // expose operational detail — is gated by `authorizer`; unauthenticated
    // callers get 401. A shallow, always-public liveness endpoint lives at
    // /healthz (see register_liveness). Static — registers on the global Drogon
    // app and needs no gateway state; exposed as a member for call-site symmetry.
    static void register_health(obs::HealthProbe probe, bool public_readiness = true,
                                OpsAuthorizer authorizer = {});

    // Register always-public shallow liveness endpoints (/healthz, /livez) that
    // return 200 `{"status":"ok"}` with no dependency detail. Safe to expose to a
    // load balancer regardless of the readiness exposure policy.
    static void register_liveness();

    // Register the GET /metrics endpoint serving the process-wide obs::metrics()
    // registry in Prometheus text format (PRD §17). When `public_metrics` is
    // false the endpoint is gated by `authorizer`; unauthenticated callers get
    // 401. Bind behind a reverse-proxy ACL or to localhost in production.
    static void register_metrics(bool public_metrics = true, OpsAuthorizer authorizer = {});

    // Whether an operational endpoint may answer this request: public endpoints
    // always may; private ones require the authorizer to accept the bearer
    // header (nullopt when absent). Pure decision, exposed for unit tests.
    [[nodiscard]] static bool ops_endpoint_permitted(bool is_public,
                                                     const OpsAuthorizer& authorizer,
                                                     std::optional<std::string_view> auth_header);

  private:
    GatewayStubs& stubs_;
  };

} // namespace fmgr::rest

#endif // FMGR_REST_RESTGATEWAY_H
