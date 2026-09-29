// SPDX-License-Identifier: AGPL-3.0-or-later

#include "rpc/RpcMethodTracker.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace fmgr::rpc {

  namespace {

    struct Tracker {
      std::mutex mutex;
      std::unordered_map<const void*, std::string> methods;
    };

    Tracker& tracker() {
      static Tracker s_tracker;
      return s_tracker;
    }

  } // namespace

  void RpcMethodTracker::note(const void* context, const std::string& method) {
    auto& state = tracker();
    std::scoped_lock lock(state.mutex);
    state.methods.insert_or_assign(context, method);
  }

  void RpcMethodTracker::forget(const void* context) {
    auto& state = tracker();
    std::scoped_lock lock(state.mutex);
    state.methods.erase(context);
  }

  std::string RpcMethodTracker::lookup(const void* context) {
    auto& state = tracker();
    std::scoped_lock lock(state.mutex);
    const auto entry = state.methods.find(context);
    if (entry == state.methods.end()) {
      return {};
    }
    return entry->second;
  }

  RpcMethodTrackerInterceptor::RpcMethodTrackerInterceptor(grpc::experimental::ServerRpcInfo* info)
      : context_(info->server_context()) {
    const char* method = info->method();
    if (method != nullptr) {
      RpcMethodTracker::note(context_, method);
    }
  }

  RpcMethodTrackerInterceptor::~RpcMethodTrackerInterceptor() {
    RpcMethodTracker::forget(context_);
  }

  void
  RpcMethodTrackerInterceptor::Intercept(grpc::experimental::InterceptorBatchMethods* methods) {
    methods->Proceed();
  }

  grpc::experimental::Interceptor* RpcMethodTrackerInterceptorFactory::CreateServerInterceptor(
      grpc::experimental::ServerRpcInfo* info) {
    return new RpcMethodTrackerInterceptor(info);
  }

} // namespace fmgr::rpc
