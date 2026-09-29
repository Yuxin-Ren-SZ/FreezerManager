// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/FreezerServer.h"

#include "kms/KmsFactory.h"
#include "obs/Log.h"
#include "rpc/AuthMiddleware.h"
#include "rpc/RpcMethodTracker.h"
#include "server/BackupScheduler.h"
#include "server/GrpcErrorTranslation.h"
#include "server/MetricsInterceptor.h"
#include "server/RateLimitInterceptor.h"

#include <fmt/format.h>
#include <google/protobuf/descriptor.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/resource_quota.h>

#include <array>
#include <vector>

#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

  // Build the master-key provider via the shared factory (OS keyring, else env,
  // else none). Returns null (PHI storage disabled) when no key is configured;
  // a malformed-but-present key throws and aborts startup (fail-fast).
  std::unique_ptr<fmgr::kms::IKmsProvider> make_kms() {
    auto kms = fmgr::kms::make_default_kms();
    if (kms == nullptr) {
      fmgr::obs::log_lifecycle(fmgr::obs::Level::Warn,
                               "PHI field encryption disabled: no master KEK configured "
                               "(set CREDENTIALS_DIRECTORY/master_kek or FMGR_MASTER_KEK)",
                               "kms.disabled");
    }
    return kms;
  }

  // Read a PEM file whole, or throw. Every failure mode here (missing file,
  // unreadable, empty) must abort startup: silently continuing would downgrade
  // the listener to plaintext and put bearer tokens and PHI on the wire.
  std::string read_pem_or_throw(const std::string& path, std::string_view what) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      throw std::runtime_error(fmt::format("cannot open TLS {} file: {}", what, path));
    }
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
      throw std::runtime_error(fmt::format("error reading TLS {} file: {}", what, path));
    }
    if (contents.empty()) {
      throw std::runtime_error(fmt::format("TLS {} file is empty: {}", what, path));
    }
    return contents;
  }

  // The gRPC services this server serves. Each name is used twice: to build
  // served_service_full_names() — the list the startup coverage check and its
  // test enumerate — and to pair a service with its implementation in build().
  // Declaring them once keeps the two from drifting, and the pair array in
  // build() is sized by this list, so adding a service without listing it here
  // does not compile.
  constexpr std::string_view k_auth_service = "fmgr.v1.AuthService";
  constexpr std::string_view k_session_service = "fmgr.v1.SessionService";
  constexpr std::string_view k_lab_service = "fmgr.v1.LabService";
  constexpr std::string_view k_box_service = "fmgr.v1.BoxService";
  constexpr std::string_view k_item_type_service = "fmgr.v1.ItemTypeService";
  constexpr std::string_view k_sample_service = "fmgr.v1.SampleService";
  constexpr std::string_view k_role_service = "fmgr.v1.RoleService";
  constexpr std::string_view k_audit_service = "fmgr.v1.AuditService";
  constexpr std::string_view k_share_service = "fmgr.v1.ShareService";

  constexpr std::array<std::string_view, 9> k_served_service_full_names{{
      k_auth_service,
      k_session_service,
      k_lab_service,
      k_box_service,
      k_item_type_service,
      k_sample_service,
      k_role_service,
      k_audit_service,
      k_share_service,
  }};

} // namespace

namespace fmgr::server {

  std::span<const std::string_view> FreezerServer::served_service_full_names() {
    return k_served_service_full_names;
  }

  std::vector<std::string> FreezerServer::served_rpc_names() {
    std::vector<std::string> rpc_names;
    const auto* pool = google::protobuf::DescriptorPool::generated_pool();
    for (const auto name : k_served_service_full_names) {
      const google::protobuf::ServiceDescriptor* service =
          pool->FindServiceByName(std::string(name));
      if (service == nullptr) {
        throw std::logic_error(
            fmt::format("no generated descriptor for served service '{}'; the service list and the "
                        "generated proto code disagree",
                        name));
      }
      for (int index = 0; index < service->method_count(); ++index) {
        rpc_names.push_back(
            fmt::format("/{}/{}", service->full_name(), service->method(index)->name()));
      }
    }
    return rpc_names;
  }

  FreezerServer::FreezerServer(storage::IStorageBackend& backend, auth::IAuthProvider& auth,
                               FreezerServerOptions opts)
      : opts_(std::move(opts)), backend_(backend), kms_(make_kms()), auth_svc_(auth, backend),
        session_svc_(auth, backend), lab_svc_(auth, backend), box_svc_(auth, backend),
        item_type_svc_(auth, backend), sample_svc_(auth, backend, kms_.get()),
        role_svc_(auth, backend), audit_svc_(auth, backend), share_svc_(auth, backend) {}

  FreezerServer::~FreezerServer() {
    if (backup_scheduler_) {
      backup_scheduler_->stop();
    }
    if (grpc_server_) {
      grpc_server_->Shutdown();
    }
  }

  void FreezerServer::build() {
    // Production guard: never fall back to a plaintext listener when TLS is
    // required. Checked before any port is bound so a misconfiguration aborts
    // startup loudly instead of serving tokens/PHI in the clear.
    if (opts_.require_tls && (opts_.tls_cert_path.empty() || opts_.tls_key_path.empty())) {
      throw std::invalid_argument(
          "TLS is required (require_tls) but tls_cert_path/tls_key_path are not configured");
    }

    // Half-configured TLS is always an error, require_tls or not: a deployment
    // that set one path clearly meant to enable TLS, so serving plaintext would
    // silently do the opposite of what the operator asked for.
    if (opts_.tls_cert_path.empty() != opts_.tls_key_path.empty()) {
      throw std::invalid_argument(
          "TLS is half-configured: tls_cert_path and tls_key_path must both be set");
    }
    if (!opts_.tls_client_ca_path.empty() && opts_.tls_cert_path.empty()) {
      throw std::invalid_argument(
          "tls_client_ca_path (mTLS) requires tls_cert_path/tls_key_path to be set");
    }

    grpc::EnableDefaultHealthCheckService(true);

    grpc::ServerBuilder builder;

    // Message-size caps (C-10 DoS): reject oversized frames before buffering,
    // in both directions.
    builder.SetMaxReceiveMessageSize(static_cast<int>(opts_.max_receive_message_bytes));
    builder.SetMaxSendMessageSize(static_cast<int>(opts_.max_send_message_bytes));

    // ResourceQuota bounds the process-wide buffer pool (bytes) and the gRPC
    // thread count (a count) — two independent limits, from two independent
    // options. See C-13: deriving one from the other is a unit error.
    grpc::ResourceQuota quota;
    quota.Resize(static_cast<std::size_t>(opts_.max_grpc_memory_bytes));
    quota.SetMaxThreads(opts_.max_grpc_threads);
    builder.SetResourceQuota(quota);

    // Process-wide error masking (C-11 infoleak): when enabled, INTERNAL errors
    // return a generic message to clients; real detail logged server-side only.
    set_mask_internal_errors(opts_.mask_internal_errors);

    // Global request throttle (C-10 DoS). AuthServiceImpl's per-IP login limiter
    // is handed the auth-tier config separately; this interceptor installs the
    // data-tier gate that all authenticated RPCs pass through via AuthMiddleware.
    rate_limiter_ = std::make_unique<RateLimitInterceptor>(opts_.rate_limit);
    if (opts_.rate_limit.enabled) {
      fmgr::obs::log_lifecycle(
          fmgr::obs::Level::Info,
          fmt::format("rate limiting enabled: auth_capacity={} data_capacity={}",
                      opts_.rate_limit.auth.capacity, opts_.rate_limit.data.capacity),
          "ratelimit.enabled");
    }

    // Per-RPC metrics (count by method+code, unary latency histogram) feed the
    // process-wide obs::metrics() registry exposed at /metrics (PRD §17).
    // RpcMethodTrackerInterceptorFactory records which RPC each ServerContext is
    // serving, which is how AuthMiddleware::authorize() (through
    // extract_bearer) can check a handler's permission against the registration
    // for its RPC (#60): gRPC's ServerContext exposes no method name of its own.
    std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>>
        interceptor_creators;
    interceptor_creators.push_back(std::make_unique<MetricsInterceptorFactory>());
    interceptor_creators.push_back(std::make_unique<rpc::RpcMethodTrackerInterceptorFactory>());
    builder.experimental().SetInterceptorCreators(std::move(interceptor_creators));

    if (opts_.tls_cert_path.empty()) {
      builder.AddListeningPort(opts_.listen_address, grpc::InsecureServerCredentials(),
                               &bound_port_);
    } else {
      // Load the PEMs before binding. read_pem_or_throw aborts startup on any
      // read failure — there is deliberately no insecure fallback path here
      // (security audit C-9).
      grpc::SslServerCredentialsOptions ssl_opts(
          opts_.tls_client_ca_path.empty()
              ? GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE
              : GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
      ssl_opts.pem_key_cert_pairs.push_back(grpc::SslServerCredentialsOptions::PemKeyCertPair{
          read_pem_or_throw(opts_.tls_key_path, "private key"),
          read_pem_or_throw(opts_.tls_cert_path, "certificate")});
      if (!opts_.tls_client_ca_path.empty()) {
        ssl_opts.pem_root_certs = read_pem_or_throw(opts_.tls_client_ca_path, "client CA");
      }

      builder.AddListeningPort(opts_.listen_address, grpc::SslServerCredentials(ssl_opts),
                               &bound_port_);
      fmgr::obs::log_lifecycle(fmgr::obs::Level::Info,
                               fmt::format("TLS enabled: cert={} mtls={}", opts_.tls_cert_path,
                                           opts_.tls_client_ca_path.empty() ? "off" : "required"),
                               "tls.enabled");
    }

    // Each served service paired with its implementation, using the same name
    // constants the coverage check below enumerates, so the two cannot drift.
    struct ServedService {
      std::string_view full_name;
      grpc::Service* impl;
    };
    const std::array<ServedService, k_served_service_full_names.size()> served_services{{
        {.full_name = k_auth_service, .impl = &auth_svc_},
        {.full_name = k_session_service, .impl = &session_svc_},
        {.full_name = k_lab_service, .impl = &lab_svc_},
        {.full_name = k_box_service, .impl = &box_svc_},
        {.full_name = k_item_type_service, .impl = &item_type_svc_},
        {.full_name = k_sample_service, .impl = &sample_svc_},
        {.full_name = k_role_service, .impl = &role_svc_},
        {.full_name = k_audit_service, .impl = &audit_svc_},
        {.full_name = k_share_service, .impl = &share_svc_},
    }};
    for (const auto& service : served_services) {
      builder.RegisterService(service.impl);
    }

    // Fail closed (#60): a served RPC that is not in the permission registry has a
    // gate nobody can look up, so refuse to start rather than serve it. The
    // integration suite asserts the stronger property — the registry holds
    // exactly these RPCs, no more.
    rpc::AuthMiddleware::verify_registry_covers(served_rpc_names());

    grpc_server_ = builder.BuildAndStart();
    if (!grpc_server_) {
      throw std::runtime_error("failed to start gRPC server on " + opts_.listen_address);
    }
    // gRPC reports a failed bind as port 0 rather than a null server. Malformed
    // cert/key material fails here (it parses inside the credentials, not in
    // read_pem_or_throw), so treat it as a hard startup failure: a server that
    // came up on no port at all must not look like a successful start.
    if (bound_port_ == 0) {
      grpc_server_.reset();
      throw std::runtime_error(
          "failed to bind " + opts_.listen_address +
          (opts_.tls_cert_path.empty() ? "" : " (check TLS certificate and key are valid PEM)"));
    }

    // Optional in-process scheduled-backup runner (PRD §14). Needs a backup KEK
    // distinct from the master KEK; without one, encrypted backups are off, so we
    // warn and skip rather than abort the server.
    if (opts_.backup_schedule.has_value()) {
      auto backup_kms = kms::make_backup_kms();
      if (backup_kms == nullptr) {
        fmgr::obs::log_lifecycle(fmgr::obs::Level::Warn,
                                 "scheduled backups disabled: no backup KEK configured "
                                 "(set CREDENTIALS_DIRECTORY/backup_kek or FMGR_BACKUP_KEK)",
                                 "backup.disabled");
      } else {
        backup_scheduler_ = std::make_unique<BackupScheduler>(backend_, std::move(backup_kms),
                                                              opts_.backup_schedule.value());
        backup_scheduler_->start();
        fmgr::obs::log_lifecycle(
            fmgr::obs::Level::Info,
            fmt::format("scheduled backups enabled: dir={}", opts_.backup_schedule->backup_dir),
            "backup.enabled");
      }
    }
  }

  void FreezerServer::wait() {
    if (grpc_server_) {
      grpc_server_->Wait();
    }
  }

  void FreezerServer::start() {
    build();
    wait();
  }

  void FreezerServer::shutdown() {
    if (backup_scheduler_) {
      backup_scheduler_->stop();
    }
    if (grpc_server_) {
      grpc_server_->Shutdown();
    }
  }

  int FreezerServer::bound_port() const {
    return bound_port_;
  }

  std::shared_ptr<grpc::Channel> FreezerServer::in_process_channel() {
    if (!grpc_server_) {
      throw std::runtime_error("in_process_channel() called before build()");
    }
    return grpc_server_->InProcessChannel(grpc::ChannelArguments{});
  }

} // namespace fmgr::server
