// SPDX-License-Identifier: AGPL-3.0-or-later

#include "server/FreezerServer.h"

#include "kms/KmsFactory.h"
#include "obs/Log.h"
#include "server/BackupScheduler.h"
#include "server/GrpcErrorTranslation.h"
#include "server/MetricsInterceptor.h"
#include "server/RateLimitInterceptor.h"

#include <fmt/format.h>
#include <grpc/grpc_security_constants.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/resource_quota.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_credentials_options.h>

#include <vector>

#include <memory>
#include <stdexcept>
#include <string>

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

  // Build the gRPC server credentials from the TLS options.
  //  - Empty cert path  -> InsecureServerCredentials (dev mode; the require_tls
  //    guard in build() has already rejected this path for production).
  //  - Cert path set     -> TLS 1.3 via a FileWatcherCertificateProvider so a
  //    rotated cert/key/CA on disk is hot-reloaded for new handshakes without
  //    dropping in-flight connections (PRD §6). mTLS is applied per opts.mtls.
  std::shared_ptr<grpc::ServerCredentials>
  make_server_credentials(const fmgr::server::FreezerServerOptions& opts) {
    if (opts.tls_cert_path.empty()) {
      return grpc::InsecureServerCredentials();
    }

    namespace exp = grpc::experimental;
    using fmgr::server::MtlsMode;

    if (opts.mtls == MtlsMode::Require && opts.tls_client_ca_path.empty()) {
      throw std::invalid_argument(
          "mTLS require mode needs tls_client_ca_path to verify client certificates");
    }

    // FileWatcherCertificateProvider re-reads the files on its poll interval and
    // hot-swaps the identity/root pairs for subsequent handshakes.
    auto provider = std::make_shared<exp::FileWatcherCertificateProvider>(
        opts.tls_key_path, opts.tls_cert_path, opts.tls_client_ca_path,
        opts.tls_reload_interval_sec);

    exp::TlsServerCredentialsOptions tls_opts(provider);
    // PRD §6: TLS 1.3 only — no downgrade to 1.2 or below. gRPC/BoringSSL then
    // restricts the suite set to the TLS 1.3 AEAD ciphers.
    tls_opts.set_min_tls_version(grpc_tls_version::TLS1_3);
    tls_opts.set_max_tls_version(grpc_tls_version::TLS1_3);
    tls_opts.watch_identity_key_cert_pairs();

    switch (opts.mtls) {
    case MtlsMode::Off:
      tls_opts.set_cert_request_type(GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
      break;
    case MtlsMode::Request:
      tls_opts.watch_root_certs();
      tls_opts.set_cert_request_type(GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_AND_VERIFY);
      break;
    case MtlsMode::Require:
      tls_opts.watch_root_certs();
      tls_opts.set_cert_request_type(GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY);
      break;
    }

    return exp::TlsServerCredentials(tls_opts);
  }

  const char* mtls_mode_name(fmgr::server::MtlsMode mode) {
    switch (mode) {
    case fmgr::server::MtlsMode::Off:
      return "off";
    case fmgr::server::MtlsMode::Request:
      return "request";
    case fmgr::server::MtlsMode::Require:
      return "require";
    }
    return "off";
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

    if (!opts_.tls_cert_path.empty()) {
      fmgr::obs::log_lifecycle(fmgr::obs::Level::Info,
                               fmt::format("TLS 1.3 enabled: cert={} mtls={} reload={}s",
                                           opts_.tls_cert_path, mtls_mode_name(opts_.mtls),
                                           opts_.tls_reload_interval_sec),
                               "tls.enabled");
    }
    builder.AddListeningPort(opts_.listen_address, make_server_credentials(opts_), &bound_port_);

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
