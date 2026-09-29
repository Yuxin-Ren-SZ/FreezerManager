// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rest/RestGateway.h"

#include "core/uuid.h"
#include "obs/Log.h"
#include "obs/Metrics.h"
#include "rest/BrowserSession.h"
#include "rest/JsonProtoMapping.h"
#include "rest/RestErrorTranslation.h"
#include "rest/SseBridge.h"

#include <drogon/drogon.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdint>
#include <functional>
#include <string>
#include <system_error>
#include <utility>

namespace fmgr::rest {
  namespace {

    using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

    drogon::HttpResponsePtr json_response(int status, std::string body) {
      auto resp = drogon::HttpResponse::newHttpResponse();
      resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
      resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
      resp->setBody(std::move(body));
      return resp;
    }

    drogon::HttpResponsePtr error_response(int status, const std::string& code,
                                           const std::string& message) {
      const nlohmann::json body{{"code", code}, {"message", message}};
      return json_response(status, body.dump());
    }

    // Success response for one route. The generic case is the proto message as
    // JSON; the overloads below decorate the two browser-session routes that do
    // more than forward (G0.1). Overload resolution picks them by response type,
    // which is why the `FMGR_ROUTE` macro can stay five arguments wide — the
    // route checker in src/web/scripts/check-routes.mjs parses exactly that shape.
    template <typename RespT>
    [[nodiscard]] drogon::HttpResponsePtr success_response(std::string_view /*path*/,
                                                           const RespT& response,
                                                           const BrowserSessionConfig& /*config*/) {
      return json_response(200, message_to_json(response));
    }

    // Login: hand the token to the browser as an HttpOnly cookie and keep it out
    // of the body, which is the whole point of the browser session. The CSRF
    // cookie is minted here, freshly, on every login.
    [[nodiscard]] drogon::HttpResponsePtr success_response(std::string_view path,
                                                           const fmgr::v1::LoginResponse& response,
                                                           const BrowserSessionConfig& config) {
      if (path != k_browser_login_path) {
        // POST /api/v1/auth/login keeps its existing shape for scripts and I1:
        // the token is in the body and no cookie is set.
        return json_response(200, message_to_json(response));
      }
      const nlohmann::json body{
          {"session_id", response.session_id()},
          {"user_id", response.user_id()},
          {"mfa_required", response.mfa_required()},
      };
      auto resp = json_response(200, body.dump());
      resp->addCookie(session_cookie(response.session_token(), config));
      resp->addCookie(csrf_cookie(generate_csrf_token(), config));
      return resp;
    }

    // Logout: the RPC has already revoked the session server-side; the response
    // additionally expires both cookies so the jar stops carrying them.
    [[nodiscard]] drogon::HttpResponsePtr success_response(std::string_view path,
                                                           const fmgr::v1::LogoutResponse& response,
                                                           const BrowserSessionConfig& config) {
      if (path != k_browser_logout_path) {
        return json_response(200, message_to_json(response));
      }
      auto resp = json_response(200, message_to_json(response));
      resp->addCookie(expired_session_cookie(config));
      resp->addCookie(expired_csrf_cookie(config));
      return resp;
    }

    // Core forwarding step shared by every route:
    //   CSRF/Origin gate -> 403 without touching gRPC
    //   parse JSON body -> proto request
    //   propagate the bearer credential (header, else session cookie) as gRPC metadata
    //   invoke the in-process gRPC handler
    //   map gRPC status / proto response back to an HTTP JSON response
    template <typename ReqT, typename RespT, typename Rpc>
    void forward(std::string_view path, const drogon::HttpRequestPtr& req, Callback&& callback,
                 Rpc&& rpc, const BrowserSessionConfig& session_config) {
      const BrowserRequest browser = browser_request_from(*req);

      // Correlation id (C-12, PRD §17): reuse a caller-supplied X-Request-Id, else
      // mint one, forward it to the gRPC handler so it reaches the audit row, and
      // echo it back so the client can correlate the response with its logs. Minted
      // before the gate so a rejected request is traceable too.
      const std::string& inbound_rid = req->getHeader("x-request-id");
      const std::string request_id = !inbound_rid.empty() ? inbound_rid : core::generate_uuid_v4();

      const auto respond = [&callback, &request_id](const drogon::HttpResponsePtr& resp) {
        resp->addHeader("X-Request-Id", request_id);
        callback(resp);
      };

      if (const auto denial = csrf_denial(browser, session_config)) {
        obs::log_event(obs::Level::Warn, *denial,
                       obs::LogFields{.request_id = request_id, .event = "rest.csrf_denied"});
        respond(error_response(403, "PERMISSION_DENIED", *denial));
        return;
      }

      ReqT request;
      try {
        json_to_message(std::string(req->getBody()), request);
      } catch (const BadJson& err) {
        respond(error_response(400, "INVALID_ARGUMENT", err.what()));
        return;
      }

      grpc::ClientContext client_ctx;
      // Drogon header lookups are case-insensitive. The value is the full
      // "Bearer <token>" string the handler's extract_bearer() expects — from the
      // Authorization header when there is one, else from the session cookie.
      const std::string authz = authorization_metadata(browser);
      if (!authz.empty()) {
        client_ctx.AddMetadata("authorization", authz);
      }
      client_ctx.AddMetadata("x-request-id", request_id);

      RespT response;
      const grpc::Status status = rpc(client_ctx, request, &response);
      if (!status.ok()) {
        const auto err = to_http_error(status);
        respond(json_response(err.status_code, err.body));
        return;
      }
      respond(success_response(path, response, session_config));
    }

  } // namespace

  void RestGateway::register_routes() {
    auto& app = drogon::app();
    GatewayStubs& s = stubs_;
    // Cookie policy for this process. Read here rather than per request so the
    // environment is sampled once, at startup.
    const BrowserSessionConfig session_config = BrowserSessionConfig::from_env();

    // Bind one POST route to one stub method. The trailing lambda adapts the
    // stub's (ctx*, req&, resp*) signature to forward()'s (ctx&, req&, resp*)
    // callable. `stub` is a GatewayStubs member; `Method` its RPC.
#define FMGR_ROUTE(path, stub, Method, ReqT, RespT)                                                \
  app.registerHandler(                                                                             \
      path,                                                                                        \
      [&s, session_config](const drogon::HttpRequestPtr& req, Callback&& callback) {               \
        forward<fmgr::v1::ReqT, fmgr::v1::RespT>(                                                  \
            path, req, std::move(callback),                                                        \
            [&s](grpc::ClientContext& client_ctx, const fmgr::v1::ReqT& rpc_req,                   \
                 fmgr::v1::RespT* rpc_resp) {                                                      \
              return s.stub->Method(&client_ctx, rpc_req, rpc_resp);                               \
            },                                                                                     \
            session_config);                                                                       \
      },                                                                                           \
      {drogon::Post})

    // ---- AuthService (login/submit-mfa work without a bearer) ----
    FMGR_ROUTE("/api/v1/auth/login", auth, Login, LoginRequest, LoginResponse);
    FMGR_ROUTE("/api/v1/auth/submit-mfa", auth, SubmitMfa, SubmitMfaRequest, SubmitMfaResponse);
    FMGR_ROUTE("/api/v1/auth/logout", auth, Logout, LogoutRequest, LogoutResponse);
    FMGR_ROUTE("/api/v1/auth/api-token/create", auth, CreateApiToken, CreateApiTokenRequest,
               CreateApiTokenResponse);
    FMGR_ROUTE("/api/v1/auth/api-token/list", auth, ListApiTokens, ListApiTokensRequest,
               ListApiTokensResponse);
    FMGR_ROUTE("/api/v1/auth/api-token/revoke", auth, RevokeApiToken, RevokeApiTokenRequest,
               RevokeApiTokenResponse);

    // ---- AuthService, browser session (G0.1) ----
    // The same three RPCs, for a client that cannot hold a token: the token goes
    // into an HttpOnly cookie instead of the body, and cookie-authenticated
    // mutations must pass the CSRF gate above.
    FMGR_ROUTE("/api/v1/auth/browser/login", auth, Login, LoginRequest, LoginResponse);
    FMGR_ROUTE("/api/v1/auth/browser/submit-mfa", auth, SubmitMfa, SubmitMfaRequest,
               SubmitMfaResponse);
    FMGR_ROUTE("/api/v1/auth/browser/logout", auth, Logout, LogoutRequest, LogoutResponse);

    // ---- SessionService ----
    FMGR_ROUTE("/api/v1/session/list", session, ListSessions, ListSessionsRequest,
               ListSessionsResponse);
    FMGR_ROUTE("/api/v1/session/revoke", session, RevokeSession, RevokeSessionRequest,
               RevokeSessionResponse);

    // ---- LabService ----
    FMGR_ROUTE("/api/v1/lab/get", lab, GetLab, GetLabRequest, GetLabResponse);
    FMGR_ROUTE("/api/v1/lab/list", lab, ListLabs, ListLabsRequest, ListLabsResponse);
    FMGR_ROUTE("/api/v1/lab/create", lab, CreateLab, CreateLabRequest, CreateLabResponse);
    FMGR_ROUTE("/api/v1/lab/update", lab, UpdateLab, UpdateLabRequest, UpdateLabResponse);
    FMGR_ROUTE("/api/v1/lab/enable-phi", lab, EnablePhi, EnablePhiRequest, EnablePhiResponse);
    FMGR_ROUTE("/api/v1/lab/members/list", lab, ListMembers, ListMembersRequest,
               ListMembersResponse);
    FMGR_ROUTE("/api/v1/lab/members/invite", lab, InviteMember, InviteMemberRequest,
               InviteMemberResponse);
    FMGR_ROUTE("/api/v1/lab/members/revoke", lab, RevokeMembership, RevokeMembershipRequest,
               RevokeMembershipResponse);

    // ---- SampleService ----
    FMGR_ROUTE("/api/v1/sample/list", sample, ListSamples, ListSamplesRequest, ListSamplesResponse);
    FMGR_ROUTE("/api/v1/sample/get", sample, GetSample, GetSampleRequest, GetSampleResponse);
    FMGR_ROUTE("/api/v1/sample/create", sample, CreateSample, CreateSampleRequest,
               CreateSampleResponse);
    FMGR_ROUTE("/api/v1/sample/update", sample, UpdateSample, UpdateSampleRequest,
               UpdateSampleResponse);
    FMGR_ROUTE("/api/v1/sample/delete", sample, SoftDeleteSample, SoftDeleteSampleRequest,
               SoftDeleteSampleResponse);
    FMGR_ROUTE("/api/v1/sample/move", sample, MoveSample, MoveSampleRequest, MoveSampleResponse);
    FMGR_ROUTE("/api/v1/sample/checkout", sample, CheckoutSample, CheckoutSampleRequest,
               CheckoutSampleResponse);
    FMGR_ROUTE("/api/v1/sample/export", sample, ExportSamplesCsv, ExportSamplesCsvRequest,
               ExportSamplesCsvResponse);
    FMGR_ROUTE("/api/v1/sample/import", sample, ImportSamples, ImportSamplesRequest,
               ImportSamplesResponse);

    // ---- BoxService (layout: freezers, containers, container/box types, boxes) ----
    FMGR_ROUTE("/api/v1/freezer/list", box, ListFreezers, ListFreezersRequest,
               ListFreezersResponse);
    FMGR_ROUTE("/api/v1/freezer/get", box, GetFreezer, GetFreezerRequest, GetFreezerResponse);
    FMGR_ROUTE("/api/v1/freezer/create", box, CreateFreezer, CreateFreezerRequest,
               CreateFreezerResponse);
    FMGR_ROUTE("/api/v1/freezer/update", box, UpdateFreezer, UpdateFreezerRequest,
               UpdateFreezerResponse);
    FMGR_ROUTE("/api/v1/freezer/archive", box, ArchiveFreezer, ArchiveFreezerRequest,
               ArchiveFreezerResponse);
    FMGR_ROUTE("/api/v1/storage-container/list", box, ListStorageContainers,
               ListStorageContainersRequest, ListStorageContainersResponse);
    FMGR_ROUTE("/api/v1/storage-container/create", box, CreateStorageContainer,
               CreateStorageContainerRequest, CreateStorageContainerResponse);
    FMGR_ROUTE("/api/v1/storage-container/update", box, UpdateStorageContainer,
               UpdateStorageContainerRequest, UpdateStorageContainerResponse);
    FMGR_ROUTE("/api/v1/storage-container/archive", box, ArchiveStorageContainer,
               ArchiveStorageContainerRequest, ArchiveStorageContainerResponse);
    FMGR_ROUTE("/api/v1/container-type/list", box, ListContainerTypes, ListContainerTypesRequest,
               ListContainerTypesResponse);
    FMGR_ROUTE("/api/v1/container-type/create", box, CreateContainerType,
               CreateContainerTypeRequest, CreateContainerTypeResponse);
    FMGR_ROUTE("/api/v1/box-type/list", box, ListBoxTypes, ListBoxTypesRequest,
               ListBoxTypesResponse);
    FMGR_ROUTE("/api/v1/box-type/create", box, CreateBoxType, CreateBoxTypeRequest,
               CreateBoxTypeResponse);
    FMGR_ROUTE("/api/v1/box/list", box, ListBoxes, ListBoxesRequest, ListBoxesResponse);
    FMGR_ROUTE("/api/v1/box/get", box, GetBox, GetBoxRequest, GetBoxResponse);
    FMGR_ROUTE("/api/v1/box/create", box, CreateBox, CreateBoxRequest, CreateBoxResponse);
    FMGR_ROUTE("/api/v1/box/update", box, UpdateBox, UpdateBoxRequest, UpdateBoxResponse);
    FMGR_ROUTE("/api/v1/box/archive", box, ArchiveBox, ArchiveBoxRequest, ArchiveBoxResponse);

    // ---- ItemTypeService (item types + custom-field definitions) ----
    FMGR_ROUTE("/api/v1/item-type/list", item_type, ListItemTypes, ListItemTypesRequest,
               ListItemTypesResponse);
    FMGR_ROUTE("/api/v1/item-type/get", item_type, GetItemType, GetItemTypeRequest,
               GetItemTypeResponse);
    FMGR_ROUTE("/api/v1/item-type/create", item_type, CreateItemType, CreateItemTypeRequest,
               CreateItemTypeResponse);
    FMGR_ROUTE("/api/v1/item-type/update", item_type, UpdateItemType, UpdateItemTypeRequest,
               UpdateItemTypeResponse);
    FMGR_ROUTE("/api/v1/item-type/archive", item_type, ArchiveItemType, ArchiveItemTypeRequest,
               ArchiveItemTypeResponse);
    FMGR_ROUTE("/api/v1/custom-field-def/list", item_type, ListCustomFieldDefinitions,
               ListCfdsRequest, ListCfdsResponse);
    FMGR_ROUTE("/api/v1/custom-field-def/create", item_type, CreateCustomFieldDefinition,
               CreateCfdRequest, CreateCfdResponse);
    FMGR_ROUTE("/api/v1/custom-field-def/update", item_type, UpdateCustomFieldDefinition,
               UpdateCfdRequest, UpdateCfdResponse);
    FMGR_ROUTE("/api/v1/custom-field-def/archive", item_type, ArchiveCustomFieldDefinition,
               ArchiveCfdRequest, ArchiveCfdResponse);

    // ---- RoleService (custom roles + permission grants) ----
    FMGR_ROUTE("/api/v1/role/list", role, ListRoles, ListRolesRequest, ListRolesResponse);
    FMGR_ROUTE("/api/v1/role/get", role, GetRole, GetRoleRequest, GetRoleResponse);
    FMGR_ROUTE("/api/v1/role/create", role, CreateRole, CreateRoleRequest, CreateRoleResponse);
    FMGR_ROUTE("/api/v1/role/update", role, UpdateRole, UpdateRoleRequest, UpdateRoleResponse);
    FMGR_ROUTE("/api/v1/role/archive", role, ArchiveRole, ArchiveRoleRequest, ArchiveRoleResponse);
    FMGR_ROUTE("/api/v1/role/permissions/list", role, ListRolePermissions,
               ListRolePermissionsRequest, ListRolePermissionsResponse);
    FMGR_ROUTE("/api/v1/role/permissions/grant", role, GrantPermission, GrantPermissionRequest,
               GrantPermissionResponse);
    FMGR_ROUTE("/api/v1/role/permissions/revoke", role, RevokePermission, RevokePermissionRequest,
               RevokePermissionResponse);

    // ---- AuditService ----
    FMGR_ROUTE("/api/v1/audit/list", audit, ListAuditEvents, ListAuditEventsRequest,
               ListAuditEventsResponse);
    FMGR_ROUTE("/api/v1/audit/get", audit, GetAuditEvent, GetAuditEventRequest,
               GetAuditEventResponse);
    FMGR_ROUTE("/api/v1/audit/verify", audit, VerifyAuditChain, VerifyAuditChainRequest,
               VerifyAuditChainResponse);
    FMGR_ROUTE("/api/v1/audit/export", audit, ExportAuditLog, ExportAuditLogRequest,
               ExportAuditLogResponse);

    // Live audit feed (server-streaming → SSE). GET so browser EventSource can
    // consume it; filters arrive as query params. Resume is by the `since`
    // cursor, carried in `Last-Event-ID` on reconnect (each frame's `id:` is the
    // event's at-micros) or an explicit `?since=<micros>`.
    app.registerHandler("/api/v1/audit/watch",
                        [&s](const drogon::HttpRequestPtr& req, Callback&& callback) {
                          fmgr::v1::WatchAuditFeedRequest rpc_req;
                          if (const auto v = req->getParameter("lab_id"); !v.empty()) {
                            rpc_req.set_lab_id(v);
                          }
                          if (const auto v = req->getParameter("entity_kind"); !v.empty()) {
                            rpc_req.set_entity_kind(v);
                          }
                          if (const auto v = req->getParameter("entity_id"); !v.empty()) {
                            rpc_req.set_entity_id(v);
                          }
                          std::string since = req->getHeader("last-event-id");
                          if (since.empty()) {
                            since = req->getParameter("since");
                          }
                          // Parse without exceptions: an unparseable cursor just
                          // starts the feed from "now".
                          std::int64_t since_micros = 0;
                          const char* begin = since.data();
                          const char* end = begin + since.size();
                          if (const auto [ptr, ec] = std::from_chars(begin, end, since_micros);
                              ec == std::errc() && ptr == end && !since.empty()) {
                            rpc_req.mutable_since()->set_unix_micros(since_micros);
                          }
                          auto* audit = s.audit.get();
                          stream_sse<fmgr::v1::AuditEvent>(
                              req, std::move(callback),
                              [audit, rpc_req](grpc::ClientContext& client_ctx) {
                                return audit->WatchAuditFeed(&client_ctx, rpc_req);
                              },
                              [](const fmgr::v1::AuditEvent& event) {
                                return "id: " + std::to_string(event.at().unix_micros()) +
                                       "\ndata: " + message_to_json(event) + "\n\n";
                              });
                        },
                        {drogon::Get});

    // Live sample feed (server-streaming → SSE). Same shape as the audit watch
    // route: GET so browser EventSource can consume it, lab/box/item-type
    // filters as query params, resume by `since` carried in `Last-Event-ID`
    // (each frame's `id:` is the sample's last_modified_at micros) or `?since=`.
    app.registerHandler("/api/v1/sample/watch",
                        [&s](const drogon::HttpRequestPtr& req, Callback&& callback) {
                          fmgr::v1::WatchSampleListRequest rpc_req;
                          if (const auto v = req->getParameter("lab_id"); !v.empty()) {
                            rpc_req.set_lab_id(v);
                          }
                          if (const auto v = req->getParameter("box_id"); !v.empty()) {
                            rpc_req.set_box_id(v);
                          }
                          if (const auto v = req->getParameter("item_type_id"); !v.empty()) {
                            rpc_req.set_item_type_id(v);
                          }
                          std::string since = req->getHeader("last-event-id");
                          if (since.empty()) {
                            since = req->getParameter("since");
                          }
                          // Parse without exceptions: an unparseable cursor just
                          // starts the feed from "now".
                          std::int64_t since_micros = 0;
                          const char* begin = since.data();
                          const char* end = begin + since.size();
                          if (const auto [ptr, ec] = std::from_chars(begin, end, since_micros);
                              ec == std::errc() && ptr == end && !since.empty()) {
                            rpc_req.mutable_since()->set_unix_micros(since_micros);
                          }
                          auto* sample = s.sample.get();
                          stream_sse<fmgr::v1::Sample>(
                              req, std::move(callback),
                              [sample, rpc_req](grpc::ClientContext& client_ctx) {
                                return sample->WatchSampleList(&client_ctx, rpc_req);
                              },
                              [](const fmgr::v1::Sample& sample) {
                                return "id: " +
                                       std::to_string(sample.last_modified_at().unix_micros()) +
                                       "\ndata: " + message_to_json(sample) + "\n\n";
                              });
                        },
                        {drogon::Get});

    // ---- ShareService (cross-lab share-request workflow) ----
    FMGR_ROUTE("/api/v1/share/list", share, ListShareRequests, ListShareRequestsRequest,
               ListShareRequestsResponse);
    FMGR_ROUTE("/api/v1/share/get", share, GetShareRequest, GetShareRequestRequest,
               GetShareRequestResponse);
    FMGR_ROUTE("/api/v1/share/create", share, CreateShareRequest, CreateShareRequestRequest,
               CreateShareRequestResponse);
    FMGR_ROUTE("/api/v1/share/approve", share, ApproveShareRequest, ApproveShareRequestRequest,
               ApproveShareRequestResponse);
    FMGR_ROUTE("/api/v1/share/reject", share, RejectShareRequest, RejectShareRequestRequest,
               RejectShareRequestResponse);
    FMGR_ROUTE("/api/v1/share/revoke", share, RevokeShareRequest, RevokeShareRequestRequest,
               RevokeShareRequestResponse);

#undef FMGR_ROUTE
  }

  void RestGateway::register_health(obs::HealthProbe probe) {
    // The handler outlives this call, so own the probe via a shared_ptr the
    // lambda copies. Each route gets a fresh handler passed as an rvalue —
    // drogon binds a reference to an lvalue handler, so a named local would
    // dangle once this function returns. Drogon dispatches GET; no bearer is
    // required (LB probe).
    auto shared = std::make_shared<obs::HealthProbe>(std::move(probe));
    const auto make_handler = [shared] {
      return [shared](const drogon::HttpRequestPtr&,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        const auto report = obs::check_health(*shared);
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(report.http_status));
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setBody(report.json_body);
        callback(resp);
      };
    };
    auto& app = drogon::app();
    app.registerHandler("/api/v1/health", make_handler(), {drogon::Get});
    app.registerHandler("/healthz", make_handler(), {drogon::Get});
  }

  void RestGateway::register_metrics() {
    drogon::app().registerHandler(
        "/metrics",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
          auto resp = drogon::HttpResponse::newHttpResponse();
          resp->setStatusCode(drogon::k200OK);
          // Prometheus text exposition format 0.0.4.
          resp->setContentTypeString("text/plain; version=0.0.4; charset=utf-8");
          resp->setBody(obs::metrics().render());
          callback(resp);
        },
        {drogon::Get});
  }

} // namespace fmgr::rest
