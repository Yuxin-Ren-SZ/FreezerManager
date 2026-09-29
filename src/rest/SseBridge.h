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
//
// Shutdown: the worker can outlive the IO loop (a Read parked on a watch
// stream is only released when the gRPC server shuts down, which freezerd does
// after drogon::app().run() returns). So the worker never holds the raw loop
// pointer; it posts through a per-loop SseLoopGuard whose runOnQuit hook
// disarms it — and cancels and releases every live stream — while the loop
// still exists.
#ifndef FMGR_REST_SSEBRIDGE_H
#define FMGR_REST_SSEBRIDGE_H

#include "rest/BrowserSession.h"
#include "rest/RestErrorTranslation.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <grpcpp/grpcpp.h>
#include <trantor/net/EventLoop.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fmgr::rest {

  // Keepalive cadence. SSE comment lines keep idle connections (and any
  // intermediary proxies) from timing out while no events are flowing.
  inline constexpr double k_sse_keepalive_seconds = 15.0;

  namespace detail {

    // Shared between the loop thread (sends, timer, cancel-on-disconnect) and
    // the worker thread (reads). `ctx` lives here so the keepalive timer can
    // TryCancel a Read that is blocked when the client has gone away. `stream`
    // is only touched on the loop thread and is reset when the loop quits.
    struct SseStreamState {
      std::shared_ptr<drogon::ResponseStream> stream;
      std::unique_ptr<grpc::ClientContext> ctx;
      std::atomic<bool> alive{true};
    };

    // One per IO loop thread. `loop` is nulled (under `mu`) by the loop's
    // runOnQuit hook, so a worker that finishes after the loop is gone drops
    // its frames instead of touching freed memory.
    struct SseLoopGuard {
      std::mutex mu;
      trantor::EventLoop* loop = nullptr;
      std::vector<std::weak_ptr<SseStreamState>> streams; // loop thread only

      template <typename F> void post(F&& func) {
        const std::lock_guard<std::mutex> lock(mu);
        if (loop != nullptr) {
          loop->queueInLoop(std::forward<F>(func));
        }
      }
    };

    // Must be called on an IO loop thread.
    inline std::shared_ptr<SseLoopGuard> sse_guard_for_current_loop() {
      thread_local std::shared_ptr<SseLoopGuard> guard;
      if (!guard) {
        guard = std::make_shared<SseLoopGuard>();
        guard->loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        guard->loop->runOnQuit([owned_guard = guard] {
          {
            const std::lock_guard<std::mutex> lock(owned_guard->mu);
            owned_guard->loop = nullptr;
          }
          for (const auto& weak : owned_guard->streams) {
            if (auto state = weak.lock()) {
              state->alive = false;
              state->ctx->TryCancel(); // release a parked Read so the worker exits
              state->stream.reset();   // ~ResponseStream closes on this (live) loop
            }
          }
          owned_guard->streams.clear();
        });
      }
      return guard;
    }

    // Worker-thread half of one watch stream: read until the stream ends or the
    // stream is cancelled, handing every frame to the loop through `guard`.
    template <typename RespT, typename OpenReader, typename FrameFn>
    void sse_read_loop(const std::shared_ptr<SseStreamState>& state,
                       const std::shared_ptr<SseLoopGuard>& guard, trantor::TimerId keepalive,
                       OpenReader open_reader, FrameFn frame_fn) {
      auto reader = open_reader(*state->ctx);
      RespT message;
      while (state->alive.load() && reader->Read(&message)) {
        std::string frame = frame_fn(message);
        guard->post([state, frame = std::move(frame)] {
          if (state->stream && !state->stream->send(frame)) {
            state->alive = false;
            state->ctx->TryCancel();
          }
        });
      }
      const grpc::Status status = reader->Finish();
      guard->post([state, status, keepalive] {
        trantor::EventLoop::getEventLoopOfCurrentThread()->invalidateTimer(keepalive);
        if (!state->stream) {
          return;
        }
        if (state->alive && !status.ok()) {
          const auto err = to_http_error(status);
          state->stream->send("event: error\ndata: " + err.body + "\n\n");
        }
        state->stream->close();
      });
    }

    // Loop-thread half: claim the response stream, register it with this loop's
    // guard, start the keepalive timer and hand the gRPC reads to the worker.
    template <typename RespT, typename OpenReader, typename FrameFn>
    void start_sse_stream(drogon::ResponseStreamPtr raw_stream, const std::string& authz,
                          OpenReader open_reader, FrameFn frame_fn) {
      auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
      auto guard = sse_guard_for_current_loop();
      auto state = std::make_shared<SseStreamState>();
      state->stream = std::shared_ptr<drogon::ResponseStream>(std::move(raw_stream));
      state->ctx = std::make_unique<grpc::ClientContext>();
      if (!authz.empty()) {
        state->ctx->AddMetadata("authorization", authz);
      }
      std::erase_if(guard->streams, [](const auto& weak) { return weak.expired(); });
      guard->streams.push_back(state);

      const trantor::TimerId keepalive = loop->runEvery(k_sse_keepalive_seconds, [state] {
        if (state->alive && state->stream && !state->stream->send(":keepalive\n\n")) {
          state->alive = false;
          state->ctx->TryCancel(); // unblock a parked Read so the worker exits
        }
      });

      std::thread(sse_read_loop<RespT, OpenReader, FrameFn>, state, guard, keepalive, open_reader,
                  frame_fn)
          .detach();
    }

  } // namespace detail

  // Bridge one gRPC server-streaming call to an SSE response.
  //   open_reader: (grpc::ClientContext&) -> std::unique_ptr<grpc::ClientReader<RespT>>
  //   frame_fn:    (const RespT&) -> std::string, a complete SSE frame ("data: ...\n\n")
  // The credential is resolved exactly as it is on every unary route: the
  // Authorization header wins, else the fmgr_session cookie is forwarded as
  // `Bearer …` metadata (G0.1), so the streaming handler runs through the same
  // RBAC gate as every other RPC. There is deliberately no query-parameter
  // fallback: a token in a URL leaks into proxy and access logs.
  template <typename RespT, typename OpenReader, typename FrameFn>
  void stream_sse(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                  OpenReader open_reader, FrameFn frame_fn) {
    const std::string authz = authorization_metadata(browser_request_from(*req));

    auto resp = drogon::HttpResponse::newAsyncStreamResponse(
        [authz, open_reader, frame_fn](drogon::ResponseStreamPtr raw_stream) mutable {
          detail::start_sse_stream<RespT>(std::move(raw_stream), authz, std::move(open_reader),
                                          std::move(frame_fn));
        },
        /*disableKickoffTimeout=*/true);

    resp->setContentTypeString("text/event-stream");
    resp->addHeader("Cache-Control", "no-cache");
    resp->addHeader("Connection", "keep-alive");
    callback(resp);
  }

} // namespace fmgr::rest

#endif // FMGR_REST_SSEBRIDGE_H
