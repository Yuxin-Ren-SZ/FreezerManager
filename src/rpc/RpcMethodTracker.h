// SPDX-License-Identifier: AGPL-3.0-or-later

// Per-RPC record of which gRPC method a server-side call is serving.
//
// AuthMiddleware::authorize() checks the permission a handler enforces against
// the permission its RPC registered (#60). To do that it needs the RPC's full
// method name, and gRPC's ServerContext does not expose it: the name is only
// known to experimental::ServerRpcInfo, which server interceptors see. So the
// server installs RpcMethodTrackerInterceptorFactory (FreezerServer::build()),
// the interceptor records (context → method) for the lifetime of each RPC, and
// the handler's extract_bearer(ctx) reads it back at its gate.
//
// The record is keyed by the ServerContextBase subobject address, not by thread:
// interceptors and handlers are not required to share a thread, and a
// thread-local would also go stale across pooled worker threads.
//
// That identity is load-bearing. The interceptor stores
// `ServerRpcInfo::server_context()`; extract_bearer() looks up
// `static_cast<const ServerContextBase*>(&ctx)`. If a gRPC upgrade ever made
// those two addresses diverge, the lookup would find nothing and the gate would
// skip its registry check — this path fails **open**, not closed. Nothing in the
// library promises the two are the same object, so the #60 acceptance test
// (ServerIntegrationTest.RegisteredPermissionDisagreeingWithEnforcedPermissionIsRefused)
// is the tripwire: it plants the disagreement and drives a live server, so it
// fails if the interceptor stops being installed or stops being found.
//
// A context with no record is not inside a served RPC (unit tests, tooling): the
// gate then skips the registry check rather than inventing an RPC to check
// against.
#ifndef FMGR_RPC_RPCMETHODTRACKER_H
#define FMGR_RPC_RPCMETHODTRACKER_H

#include <grpcpp/support/interceptor.h>
#include <grpcpp/support/server_interceptor.h>

#include <string>

namespace fmgr::rpc {

  class RpcMethodTracker {
  public:
    // Record `method` as the RPC served by `context`. Overwrites a previous entry
    // for the same address: the server pools ServerContext objects, and a new RPC
    // always records before its handler runs.
    static void note(const void* context, const std::string& method);
    static void forget(const void* context);
    // The method `context` is serving, or an empty string when there is none.
    // Returns a copy: the entry is erased when the RPC ends, possibly while
    // another thread is looking it up.
    [[nodiscard]] static std::string lookup(const void* context);
  };

  // Keeps RpcMethodTracker in step with one RPC's lifetime. Does no work of its
  // own at interception points; it only has to let each batch Proceed.
  class RpcMethodTrackerInterceptor final : public grpc::experimental::Interceptor {
  public:
    explicit RpcMethodTrackerInterceptor(grpc::experimental::ServerRpcInfo* info);
    ~RpcMethodTrackerInterceptor() override;

    RpcMethodTrackerInterceptor(const RpcMethodTrackerInterceptor&) = delete;
    RpcMethodTrackerInterceptor& operator=(const RpcMethodTrackerInterceptor&) = delete;
    RpcMethodTrackerInterceptor(RpcMethodTrackerInterceptor&&) = delete;
    RpcMethodTrackerInterceptor& operator=(RpcMethodTrackerInterceptor&&) = delete;

    void Intercept(grpc::experimental::InterceptorBatchMethods* methods) override;

  private:
    const void* context_;
  };

  class RpcMethodTrackerInterceptorFactory final
      : public grpc::experimental::ServerInterceptorFactoryInterface {
  public:
    grpc::experimental::Interceptor*
    CreateServerInterceptor(grpc::experimental::ServerRpcInfo* info) override;
  };

} // namespace fmgr::rpc

#endif // FMGR_RPC_RPCMETHODTRACKER_H
