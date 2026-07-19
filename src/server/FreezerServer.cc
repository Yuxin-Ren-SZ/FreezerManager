// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/FreezerServer.h"

#include "kms/KmsFactory.h"
#include "obs/Log.h"
#include "server/BackupScheduler.h"
#include "server/GrpcErrorTranslation.h"
#include "server/MetricsInterceptor.h"
#include "server/RateLimitInterceptor.h"

#include <fmt/format.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/resource_quota.h>

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

} // namespace

namespace fmgr::server {

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

    // Message-size cap (C-10 DoS): reject oversized frames before buffering.
    // Paired with a ResourceQuota that bounds the process-wide buffer pool.
    builder.SetMaxReceiveMessageSize(static_cast<int>(opts_.max_receive_message_bytes));
    grpc::ResourceQuota quota;
    quota.SetMaxThreads(static_cast<int>(opts_.max_receive_message_bytes) / 4096);
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
    std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>>
        interceptor_creators;
    interceptor_creators.push_back(std::make_unique<MetricsInterceptorFactory>());
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

    builder.RegisterService(&auth_svc_);
    builder.RegisterService(&session_svc_);
    builder.RegisterService(&lab_svc_);
    builder.RegisterService(&box_svc_);
    builder.RegisterService(&item_type_svc_);
    builder.RegisterService(&sample_svc_);
    builder.RegisterService(&role_svc_);
    builder.RegisterService(&audit_svc_);
    builder.RegisterService(&share_svc_);

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
