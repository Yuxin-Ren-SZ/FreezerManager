// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rest/RestGateway.h"

#include "rest/JsonProtoMapping.h"
#include "rest/RestErrorTranslation.h"

#include <drogon/drogon.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <string>
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

    // Core forwarding step shared by every route:
    //   parse JSON body -> proto request
    //   propagate the bearer header as gRPC metadata
    //   invoke the in-process gRPC handler
    //   map gRPC status / proto response back to an HTTP JSON response
    template <typename ReqT, typename RespT, typename Rpc>
    void forward(const drogon::HttpRequestPtr& req, Callback&& callback, Rpc&& rpc) {
      ReqT request;
      try {
        json_to_message(std::string(req->getBody()), request);
      } catch (const BadJson& err) {
        callback(error_response(400, "INVALID_ARGUMENT", err.what()));
        return;
      }

      grpc::ClientContext client_ctx;
      // Drogon header lookups are case-insensitive; the value is the full
      // "Bearer <token>" string the handler's extract_bearer() expects.
      const std::string& authz = req->getHeader("authorization");
      if (!authz.empty()) {
        client_ctx.AddMetadata("authorization", authz);
      }

      RespT response;
      const grpc::Status status = rpc(client_ctx, request, &response);
      if (!status.ok()) {
        const auto err = to_http_error(status);
        callback(json_response(err.status_code, err.body));
        return;
      }
      callback(json_response(200, message_to_json(response)));
    }

  } // namespace

  void RestGateway::register_routes() {
    auto& app = drogon::app();
    GatewayStubs& s = stubs_;

    // Bind one POST route to one stub method. The trailing lambda adapts the
    // stub's (ctx*, req&, resp*) signature to forward()'s (ctx&, req&, resp*)
    // callable. `stub` is a GatewayStubs member; `Method` its RPC.
#define FMGR_ROUTE(path, stub, Method, ReqT, RespT)                                                \
  app.registerHandler(path,                                                                        \
                      [&s](const drogon::HttpRequestPtr& req, Callback&& callback) {               \
                        forward<fmgr::v1::ReqT, fmgr::v1::RespT>(                                  \
                            req, std::move(callback),                                              \
                            [&s](grpc::ClientContext& client_ctx, const fmgr::v1::ReqT& rpc_req,   \
                                 fmgr::v1::RespT* rpc_resp) {                                      \
                              return s.stub->Method(&client_ctx, rpc_req, rpc_resp);               \
                            });                                                                    \
                      },                                                                           \
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

#undef FMGR_ROUTE
  }

} // namespace fmgr::rest
