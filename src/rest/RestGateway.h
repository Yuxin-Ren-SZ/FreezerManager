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

namespace fmgr::rest {

  class RestGateway {
  public:
    explicit RestGateway(GatewayStubs& stubs) : stubs_(stubs) {}

    // Register every `/api/v1/*` route on the process-wide Drogon app. Call once
    // before drogon::app().run(). The referenced GatewayStubs must outlive the
    // running app.
    void register_routes();

    // Register the unauthenticated GET /api/v1/health (+ /healthz alias) readiness
    // endpoint backed by `probe`. The probe is copied and owned by the handler, so
    // whatever it captures must outlive the running app. 200 when healthy, 503
    // when a dependency is down (PRD §17). Static — it registers on the global
    // Drogon app and needs no gateway state; exposed as a member for call-site
    // symmetry with register_routes().
    static void register_health(obs::HealthProbe probe);

  private:
    GatewayStubs& stubs_;
  };

} // namespace fmgr::rest

#endif // FMGR_REST_RESTGATEWAY_H
