// SPDX-License-Identifier: AGPL-3.0-or-later

// Server-Sent Events bridge for gRPC server-streaming RPCs.
//
// The Drogon event-loop thread must never block, but gRPC's synchronous
// `ClientReader::Read()` does. So a server-streaming RPC is driven on a
// dedicated worker thread, and each message is pushed to the HTTP client by
// posting `ResponseStream::send()` back onto the connection's event loop via
// `queueInLoop` — `trantor::AsyncStream::send()` is only safe on its own loop
// thread. All stream writes (data frames, keepalive comments, the closing
// frame) therefore execute serialized on the loop thread; the worker thread
// only touches the gRPC reader, the (thread-safe) `ClientContext::TryCancel`,
// and the atomic liveness flag.
#ifndef FMGR_REST_SSEBRIDGE_H
#define FMGR_REST_SSEBRIDGE_H

#include "rest/RestErrorTranslation.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <grpcpp/grpcpp.h>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace fmgr::rest {

  // Keepalive cadence. SSE comment lines keep idle connections (and any
  // intermediary proxies) from timing out while no events are flowing.
  inline constexpr double k_sse_keepalive_seconds = 15.0;

  // Bridge one gRPC server-streaming call to an SSE response.
  //   open_reader: (grpc::ClientContext&) -> std::unique_ptr<grpc::ClientReader<RespT>>
  //   frame_fn:    (const RespT&) -> std::string, a complete SSE frame ("data: ...\n\n")
  // The Authorization header (or an `access_token` query param, for browser
  // EventSource which cannot set headers) is forwarded as gRPC metadata so the
  // streaming handler runs through the same RBAC gate as every other RPC.
  template <typename RespT, typename OpenReader, typename FrameFn>
  void stream_sse(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                  OpenReader open_reader, FrameFn frame_fn) {
    std::string authz = req->getHeader("authorization");
    if (authz.empty()) {
      const std::string token = req->getParameter("access_token");
      if (!token.empty()) {
        authz = "Bearer " + token;
      }
    }

    // Shared between the loop thread (sends, timer, cancel-on-disconnect) and
    // the worker thread (reads). `ctx` lives here so the keepalive timer can
    // TryCancel a Read that is blocked when the client has gone away.
    struct StreamState {
      std::shared_ptr<drogon::ResponseStream> stream;
      std::unique_ptr<grpc::ClientContext> ctx;
      std::atomic<bool> alive{true};
    };

    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [authz, open_reader, frame_fn](drogon::ResponseStreamPtr raw_stream) {
          auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
          auto state = std::make_shared<StreamState>();
          state->stream = std::shared_ptr<drogon::ResponseStream>(std::move(raw_stream));
          state->ctx = std::make_unique<grpc::ClientContext>();
          if (!authz.empty()) {
            state->ctx->AddMetadata("authorization", authz);
          }

          const trantor::TimerId keepalive = loop->runEvery(k_sse_keepalive_seconds, [state] {
            if (state->alive && !state->stream->send(":keepalive\n\n")) {
              state->alive = false;
              state->ctx->TryCancel(); // unblock a parked Read so the worker exits
            }
          });

          std::thread([state, loop, keepalive, open_reader, frame_fn] {
            auto reader = open_reader(*state->ctx);
            RespT message;
            while (state->alive.load() && reader->Read(&message)) {
              std::string frame = frame_fn(message);
              loop->queueInLoop([state, frame = std::move(frame)] {
                if (!state->stream->send(frame)) {
                  state->alive = false;
                  state->ctx->TryCancel();
                }
              });
            }
            const grpc::Status status = reader->Finish();
            loop->queueInLoop([state, status, loop, keepalive] {
              loop->invalidateTimer(keepalive);
              if (state->alive && !status.ok()) {
                const auto err = to_http_error(status);
                state->stream->send("event: error\ndata: " + err.body + "\n\n");
              }
              state->stream->close();
            });
          }).detach();
        },
        /*disableKickoffTimeout=*/true);

    resp->setContentTypeString("text/event-stream");
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("Connection", "keep-alive");
    callback(resp);
  }

} // namespace fmgr::rest

#endif // FMGR_REST_SSEBRIDGE_H
