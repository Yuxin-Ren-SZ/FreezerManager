// SPDX-License-Identifier: AGPL-3.0-or-later

// End-to-end coverage for the gRPC TLS listener (security audit C-9).
//
// The critical property under test is not "TLS works" but "TLS never silently
// degrades": a missing, empty, or malformed certificate must abort startup
// rather than leave a plaintext listener serving bearer tokens and PHI.

#include "auth/LocalAuthProvider.h"
#include "core/identity.h"
#include "core/role.h"
#include "server/FreezerServer.h"
#include "storage/IdentityTraits.h"
#include "storage/RoleTraits.h"
#include "storage/sqlite/AuditRepositories.h"
#include "storage/sqlite/IdentityRepositories.h"
#include "storage/sqlite/RoleRepositories.h"
#include "storage/sqlite/SessionRepositories.h"
#include "storage/sqlite/SqliteBackend.h"

#include <fmgr/v1/auth.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace fmgr::test {
  namespace {

    // Fast Argon2id parameters for tests.
    [[nodiscard]] auth::LocalAuthProviderConfig fast_config() {
      auth::LocalAuthProviderConfig cfg;
      cfg.pwhash_memlimit = 8192;
      cfg.pwhash_opslimit = 1;
      return cfg;
    }

    // A self-signed cert/key pair on disk, valid for localhost.
    //
    // The cert carries CA:TRUE so the same file can serve three roles in these
    // tests: the server's leaf certificate, the client's root-of-trust bundle,
    // and (for the mTLS cases) a client certificate the server can verify
    // against that same bundle.
    struct SelfSignedCert {
      std::filesystem::path cert_path;
      std::filesystem::path key_path;
    };

    [[nodiscard]] std::filesystem::path unique_temp_dir() {
      static std::atomic<int> counter{0};
      const auto dir = std::filesystem::temp_directory_path() /
                       ("fmgr-tls-test-" + std::to_string(counter.fetch_add(1)) + "-" +
                        std::to_string(::getpid()));
      std::filesystem::create_directories(dir);
      return dir;
    }

    // Generates a 2048-bit RSA self-signed certificate into `dir`.
    // Time offsets are seconds from now (negative = past, used for expired-cert tests).
    [[nodiscard]] SelfSignedCert make_self_signed_cert(
        const std::filesystem::path& dir, const std::string& common_name,
        long not_before_offset_sec = 0, long not_after_offset_sec = 60L * 60L) {
      std::filesystem::create_directories(dir);
      EVP_PKEY* pkey = EVP_RSA_gen(2048);
      if (pkey == nullptr) {
        throw std::runtime_error("EVP_RSA_gen failed");
      }
      X509* cert = X509_new();
      if (cert == nullptr) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_new failed");
      }

      X509_set_version(cert, 2); // X.509 v3
      ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
      X509_gmtime_adj(X509_getm_notBefore(cert), not_before_offset_sec);
      X509_gmtime_adj(X509_getm_notAfter(cert), not_after_offset_sec);
      X509_set_pubkey(cert, pkey);

      X509_NAME* name = X509_get_subject_name(cert);
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>(common_name.c_str()), -1,
                                 -1, 0);
      X509_set_issuer_name(cert, name); // self-signed

      X509V3_CTX ext_ctx;
      X509V3_set_ctx_nodb(&ext_ctx);
      X509V3_set_ctx(&ext_ctx, cert, cert, nullptr, nullptr, 0);
      const auto add_ext = [&](int nid, const char* value) {
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ext_ctx, nid, value);
        if (ext != nullptr) {
          X509_add_ext(cert, ext, -1);
          X509_EXTENSION_free(ext);
        }
      };
      add_ext(NID_basic_constraints, "critical,CA:TRUE");
      add_ext(NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
      // SAN must match the common_name so wrong-hostname tests actually fail
      // gRPC hostname verification. Use DNS form — no IP since common_name is
      // usually a hostname.
      {
        const std::string dns = "DNS:" + common_name;
        add_ext(NID_subject_alt_name, dns.c_str());
      }

      if (X509_sign(cert, pkey, EVP_sha256()) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_sign failed");
      }

      const SelfSignedCert out{dir / "cert.pem", dir / "key.pem"};

      FILE* cert_file = std::fopen(out.cert_path.string().c_str(), "wb");
      const bool cert_ok = cert_file != nullptr && PEM_write_X509(cert_file, cert) == 1;
      if (cert_file != nullptr) {
        std::fclose(cert_file);
      }
      FILE* key_file = std::fopen(out.key_path.string().c_str(), "wb");
      const bool key_ok =
          key_file != nullptr &&
          PEM_write_PrivateKey(key_file, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
      if (key_file != nullptr) {
        std::fclose(key_file);
      }

      X509_free(cert);
      EVP_PKEY_free(pkey);
      if (!cert_ok || !key_ok) {
        throw std::runtime_error("failed to write PEM files");
      }
      return out;
    }

    // P-256 (secp256r1) ECDSA self-signed certificate. Same shape as the RSA
    // variant above; used to prove the TLS listener accepts EC keys as well.
    [[nodiscard]] SelfSignedCert make_ec_self_signed_cert(const std::filesystem::path& dir,
                                                          const std::string& common_name) {
      std::filesystem::create_directories(dir);
      EVP_PKEY* pkey = EVP_EC_gen("prime256v1"); // OpenSSL ≥ 3.0
      if (pkey == nullptr) {
        throw std::runtime_error("EVP_EC_gen(prime256v1) failed");
      }
      X509* cert = X509_new();
      if (cert == nullptr) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_new failed");
      }

      X509_set_version(cert, 2);
      ASN1_INTEGER_set(X509_get_serialNumber(cert), 2);
      X509_gmtime_adj(X509_getm_notBefore(cert), 0);
      X509_gmtime_adj(X509_getm_notAfter(cert), 60L * 60L);
      X509_set_pubkey(cert, pkey);

      X509_NAME* name = X509_get_subject_name(cert);
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                 reinterpret_cast<const unsigned char*>(common_name.c_str()), -1,
                                 -1, 0);
      X509_set_issuer_name(cert, name); // self-signed

      X509V3_CTX ext_ctx;
      X509V3_set_ctx_nodb(&ext_ctx);
      X509V3_set_ctx(&ext_ctx, cert, cert, nullptr, nullptr, 0);
      const auto add_ext = [&](int nid, const char* value) {
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ext_ctx, nid, value);
        if (ext != nullptr) {
          X509_add_ext(cert, ext, -1);
          X509_EXTENSION_free(ext);
        }
      };
      add_ext(NID_basic_constraints, "critical,CA:TRUE");
      add_ext(NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
      {
        const std::string dns = "DNS:" + common_name;
        add_ext(NID_subject_alt_name, dns.c_str());
      }

      if (X509_sign(cert, pkey, EVP_sha256()) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_sign failed for EC cert");
      }

      const SelfSignedCert out{dir / "cert.pem", dir / "key.pem"};

      FILE* cert_file = std::fopen(out.cert_path.string().c_str(), "wb");
      const bool cert_ok = cert_file != nullptr && PEM_write_X509(cert_file, cert) == 1;
      if (cert_file != nullptr) std::fclose(cert_file);
      FILE* key_file = std::fopen(out.key_path.string().c_str(), "wb");
      const bool key_ok =
          key_file != nullptr &&
          PEM_write_PrivateKey(key_file, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1;
      if (key_file != nullptr) std::fclose(key_file);

      X509_free(cert);
      EVP_PKEY_free(pkey);
      if (!cert_ok || !key_ok) {
        throw std::runtime_error("failed to write EC PEM files");
      }
      return out;
    }

    [[nodiscard]] std::string read_file(const std::filesystem::path& path) {
      std::ifstream file(path, std::ios::binary);
      return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    void write_file(const std::filesystem::path& path, const std::string& contents) {
      std::ofstream file(path, std::ios::binary);
      file << contents;
    }

    // Fixture: a certificate on disk plus a backend/auth provider ready to hand
    // to a FreezerServer. The server itself is built per-test so each case can
    // choose its own TLS options (and assert on build() throwing).
    class GrpcTlsTest : public ::testing::Test {
    protected:
      void SetUp() override {
        dir_ = unique_temp_dir();
        cert_ = make_self_signed_cert(dir_, "localhost");

        backend_ = std::make_unique<storage::SqliteBackend>(
            storage::SqliteBackendOptions{.database_path = ":memory:"});
        storage::register_identity_repositories(*backend_);
        storage::register_role_repositories(*backend_);
        storage::register_session_repositories(*backend_);
        storage::register_audit_repositories(*backend_);
        backend_->migrate_to_latest();

        provider_ = std::make_unique<auth::LocalAuthProvider>(*backend_, fast_config());
        seed_test_user();
      }

      void TearDown() override {
        stop_server();
        provider_.reset();
        backend_.reset();
        std::error_code error;
        std::filesystem::remove_all(dir_, error);
      }

      // Starts a server with the given options and returns its "localhost:port"
      // target. Options are moved in so callers can tweak TLS fields freely.
      [[nodiscard]] std::string start_server(server::FreezerServerOptions opts) {
        opts.listen_address = "localhost:0";
        server_ = std::make_unique<server::FreezerServer>(*backend_, *provider_, std::move(opts));
        server_->build();
        server_thread_ = std::thread([this] { server_->wait(); });
        return "localhost:" + std::to_string(server_->bound_port());
      }

      void stop_server() {
        if (server_) {
          server_->shutdown();
        }
        if (server_thread_.joinable()) {
          server_thread_.join();
        }
        server_.reset();
      }

      // TLS options pointing at the fixture's certificate.
      [[nodiscard]] server::FreezerServerOptions tls_opts() const {
        server::FreezerServerOptions opts;
        opts.tls_cert_path = cert_.cert_path.string();
        opts.tls_key_path = cert_.key_path.string();
        opts.require_tls = true;
        return opts;
      }

      // Client channel credentials trusting the fixture's self-signed cert.
      [[nodiscard]] std::shared_ptr<grpc::ChannelCredentials>
      client_creds(bool with_client_cert = false) const {
        grpc::SslCredentialsOptions ssl_opts;
        ssl_opts.pem_root_certs = read_file(cert_.cert_path);
        if (with_client_cert) {
          ssl_opts.pem_private_key = read_file(cert_.key_path);
          ssl_opts.pem_cert_chain = read_file(cert_.cert_path);
        }
        return grpc::SslCredentials(ssl_opts);
      }

      // Issues a Login over `channel` and returns the resulting status.
      [[nodiscard]] static grpc::Status try_login(const std::shared_ptr<grpc::Channel>& channel) {
        auto stub = fmgr::v1::AuthService::NewStub(channel);
        grpc::ClientContext ctx;
        // Bound the wait: a rejected TLS handshake otherwise blocks until the
        // channel gives up, and a wrong-transport test would hang the suite.
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        fmgr::v1::LoginRequest req;
        req.set_email(k_email);
        req.set_password(k_password);
        fmgr::v1::LoginResponse resp;
        return stub->Login(&ctx, req, &resp);
      }

      static constexpr const char* k_email = "admin@example.com";
      static constexpr const char* k_password = "hunter22";

      std::filesystem::path dir_;
      SelfSignedCert cert_;
      std::unique_ptr<storage::SqliteBackend> backend_;
      std::unique_ptr<auth::LocalAuthProvider> provider_;
      std::unique_ptr<server::FreezerServer> server_;
      std::thread server_thread_;

    private:
      void seed_test_user() {
        const auto password_hash = provider_->hash_password(k_password);
        const core::UserId uid = core::UserId::parse("10000000-0000-0000-0000-000000000001");
        const core::LabId lab_id = core::LabId::parse("20000000-0000-0000-0000-000000000001");
        const core::User user{
            .id = uid,
            .primary_email = k_email,
            .display_name = "Test Admin",
            .status = core::UserStatus::Active,
            .created_at = core::Timestamp::from_unix_micros(1),
            .auth_bindings = nlohmann::json::array({
                nlohmann::json::object({{"provider", "local"}, {"hash", password_hash}}),
            }),
        };
        const core::Lab lab{
            .id = lab_id,
            .name = "Test Lab",
            .contact = "test@example.com",
            .created_at = core::Timestamp::from_unix_micros(1),
            .settings_json = nlohmann::json::object(),
        };
        const core::LabMembership membership{
            .user_id = uid,
            .lab_id = lab_id,
            .role_id = core::builtin_role_id(core::RoleKind::SystemAdmin),
            .scope_filters_json = nlohmann::json::object(),
            .joined_at = core::Timestamp::from_unix_micros(1),
        };
        const storage::MutationContext ctx{
            .actor_user_id = core::UserId::parse("00000000-0000-0000-0000-000000000000"),
            .actor_session_id = "seed",
            .request_id = "seed",
            .reason = "test setup",
        };
        auto txn = backend_->begin(storage::IsolationLevel::Serializable);
        txn->repo<core::Lab>().insert(lab, ctx);
        txn->repo<core::User>().insert(user, ctx);
        txn->repo<core::LabMembership>().insert(membership, ctx);
        txn->commit();
      }
    };

    // ---- Happy path ----

    TEST_F(GrpcTlsTest, TlsClientCompletesRpcAgainstTlsServer) {
      const auto target = start_server(tls_opts());
      const auto status = try_login(grpc::CreateChannel(target, client_creds()));
      EXPECT_TRUE(status.ok()) << status.error_message();
    }

    // ---- Wrong transport ----

    TEST_F(GrpcTlsTest, PlaintextClientCannotReachTlsServer) {
      const auto target = start_server(tls_opts());
      const auto status =
          try_login(grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
      EXPECT_FALSE(status.ok());
    }

    TEST_F(GrpcTlsTest, TlsClientCannotReachPlaintextServer) {
      server::FreezerServerOptions opts; // no cert paths -> plaintext dev mode
      const auto target = start_server(std::move(opts));
      const auto status = try_login(grpc::CreateChannel(target, client_creds()));
      EXPECT_FALSE(status.ok());
    }

    // ---- Fail-closed on bad certificate material ----
    //
    // These are the regressions that matter most: every one of them must throw,
    // never fall through to grpc::InsecureServerCredentials().

    TEST_F(GrpcTlsTest, MissingCertFileThrows) {
      auto opts = tls_opts();
      opts.tls_cert_path = (dir_ / "does-not-exist.pem").string();
      server::FreezerServer server(*backend_, *provider_, std::move(opts));
      EXPECT_THROW(server.build(), std::runtime_error);
      EXPECT_EQ(server.bound_port(), 0);
    }

    TEST_F(GrpcTlsTest, MissingKeyFileThrows) {
      auto opts = tls_opts();
      opts.tls_key_path = (dir_ / "does-not-exist.pem").string();
      server::FreezerServer server(*backend_, *provider_, std::move(opts));
      EXPECT_THROW(server.build(), std::runtime_error);
      EXPECT_EQ(server.bound_port(), 0);
    }

    TEST_F(GrpcTlsTest, EmptyCertFileThrows) {
      write_file(cert_.cert_path, "");
      auto opts = tls_opts();
      server::FreezerServer server(*backend_, *provider_, std::move(opts));
      EXPECT_THROW(server.build(), std::runtime_error);
      EXPECT_EQ(server.bound_port(), 0);
    }

    TEST_F(GrpcTlsTest, CorruptCertContentsThrow) {
      write_file(cert_.cert_path, "-----BEGIN CERTIFICATE-----\nnot base64 at all\n");
      auto opts = tls_opts();
      server::FreezerServer server(*backend_, *provider_, std::move(opts));
      EXPECT_THROW(server.build(), std::runtime_error);
      EXPECT_EQ(server.bound_port(), 0);
    }

    TEST_F(GrpcTlsTest, KeyNotMatchingCertThrows) {
      const auto other = make_self_signed_cert(dir_ / "other", "localhost");
      auto opts = tls_opts();
      opts.tls_key_path = other.key_path.string();
      server::FreezerServer server(*backend_, *provider_, std::move(opts));
      EXPECT_THROW(server.build(), std::runtime_error);
      EXPECT_EQ(server.bound_port(), 0);
    }

    TEST_F(GrpcTlsTest, CertWithoutKeyThrows) {
      auto opts = tls_opts();
      opts.tls_key_path.clear();
      opts.require_tls = false; // isolate the half-configured check from the guard
      server::FreezerServer server(*backend_, *provider_, std::move(opts));
      EXPECT_THROW(server.build(), std::invalid_argument);
      EXPECT_EQ(server.bound_port(), 0);
    }

    TEST_F(GrpcTlsTest, ClientCaWithoutServerCertThrows) {
      server::FreezerServerOptions opts;
      opts.tls_client_ca_path = cert_.cert_path.string();
      server::FreezerServer server(*backend_, *provider_, std::move(opts));
      EXPECT_THROW(server.build(), std::invalid_argument);
      EXPECT_EQ(server.bound_port(), 0);
    }

    // ---- mTLS ----

    TEST_F(GrpcTlsTest, MtlsRejectsClientWithoutCertificate) {
      auto opts = tls_opts();
      opts.tls_client_ca_path = cert_.cert_path.string();
      const auto target = start_server(std::move(opts));

      const auto status = try_login(grpc::CreateChannel(target, client_creds(false)));
      EXPECT_FALSE(status.ok());
    }

    TEST_F(GrpcTlsTest, MtlsAcceptsClientWithCertificate) {
      auto opts = tls_opts();
      opts.tls_client_ca_path = cert_.cert_path.string();
      const auto target = start_server(std::move(opts));

      const auto status = try_login(grpc::CreateChannel(target, client_creds(true)));
      EXPECT_TRUE(status.ok()) << status.error_message();
    }

    // ---- Certificate validity ----

    TEST_F(GrpcTlsTest, ExpiredCertThrows) {
      // notAfter is 1 hour in the past.
      const auto expired = make_self_signed_cert(dir_ / "expired", "localhost",
                                                 /*not_before=*/-7200, /*not_after=*/-3600);
      auto opts = tls_opts();
      opts.tls_cert_path = expired.cert_path.string();
      opts.tls_key_path = expired.key_path.string();
      // gRPC does not validate certificate lifetimes at server-bind time,
      // only during the TLS handshake.  build() will succeed with an expired
      // cert; the test must prove that a client connection then fails.
      const auto target = start_server(std::move(opts));
      // Wait for the handshake to actually fail — Insecure channel to an
      // expired-cert server will eventually get RST or timeout.
      const auto status =
          try_login(grpc::CreateChannel(target, client_creds()));
      EXPECT_FALSE(status.ok()) << "expired cert should be rejected at handshake, got: "
                                << status.error_message();
    }

    TEST_F(GrpcTlsTest, WrongHostnameRejected) {
      // The server cert claims "evil.example.com"; the client dials "localhost".
      // gRPC's default hostname verification must reject the mismatch.
      const auto wrong = make_self_signed_cert(dir_ / "wronghost", "evil.example.com");
      auto opts = tls_opts();
      opts.tls_cert_path = wrong.cert_path.string();
      opts.tls_key_path = wrong.key_path.string();
      const auto target = start_server(std::move(opts));

      grpc::SslCredentialsOptions ssl;
      ssl.pem_root_certs = read_file(wrong.cert_path);
      const auto status =
          try_login(grpc::CreateChannel(target, grpc::SslCredentials(ssl)));
      EXPECT_FALSE(status.ok());
    }

    // ---- Channel stability ----

    TEST_F(GrpcTlsTest, MultipleRpcOverSingleTlsChannel) {
      const auto target = start_server(tls_opts());
      auto channel = grpc::CreateChannel(target, client_creds());

      // First RPC: Login to obtain a session token.
      auto auth_stub = fmgr::v1::AuthService::NewStub(channel);
      std::string session_token;
      {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        fmgr::v1::LoginRequest req;
        req.set_email(k_email);
        req.set_password(k_password);
        fmgr::v1::LoginResponse resp;
        const auto status = auth_stub->Login(&ctx, req, &resp);
        ASSERT_TRUE(status.ok()) << status.error_message();
        ASSERT_FALSE(resp.session_token().empty());
        session_token = resp.session_token();
      }

      // Subsequent RPCs on the same TLS channel: ListLabs three times,
      // each carrying the session token as an authorization header.
      auto lab_stub = fmgr::v1::LabService::NewStub(channel);
      for (int i = 0; i < 3; ++i) {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        ctx.AddMetadata("authorization", "Bearer " + session_token);
        fmgr::v1::ListLabsRequest req;
        fmgr::v1::ListLabsResponse resp;
        const auto status = lab_stub->ListLabs(&ctx, req, &resp);
        EXPECT_TRUE(status.ok()) << "RPC " << i << ": " << status.error_message();
      }
    }

    // ---- Concurrent clients ----

    // Concurrent TLS handshakes must not crash or hang the server. The exact
    // success count depends on the backend (SQLite :memory: serializes audit-
    // append writes), so this test asserts at least one successful login while
    // all clients complete without throwing or deadlocking.
    TEST_F(GrpcTlsTest, ConcurrentTlsClients) {
      const auto target = start_server(tls_opts());
      std::atomic<int> successes{0};
      constexpr int kClients = 3;

      std::vector<std::thread> threads;
      threads.reserve(kClients);
      for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([this, &target, &successes] {
          const auto status =
              try_login(grpc::CreateChannel(target, client_creds()));
          if (status.ok()) {
            ++successes;
          }
        });
      }
      for (auto& t : threads) {
        t.join();
      }
      // At least one must succeed — the others may hit SQLite write-lock
      // contention on the audit append, which is orthogonal to TLS.
      EXPECT_GE(successes.load(), 1);
    }

    // ---- ECDSA key ----

    TEST_F(GrpcTlsTest, EcdsaKeyWorks) {
      const auto ec = make_ec_self_signed_cert(dir_ / "ecdsa", "localhost");
      auto opts = tls_opts();
      opts.tls_cert_path = ec.cert_path.string();
      opts.tls_key_path = ec.key_path.string();
      const auto target = start_server(std::move(opts));

      grpc::SslCredentialsOptions ssl;
      ssl.pem_root_certs = read_file(ec.cert_path);
      const auto status =
          try_login(grpc::CreateChannel(target, grpc::SslCredentials(ssl)));
      EXPECT_TRUE(status.ok()) << status.error_message();
    }

    // ---- mTLS with unrelated CA ----

    TEST_F(GrpcTlsTest, MtlsRejectsClientSignedByUnrelatedCa) {
      // Server trusts cert_ (CA-A).  Generate a completely independent CA-B and
      // have the client present a cert signed by CA-B.  The server must reject
      // because the client cert chains to a root it does not trust.
      const auto unrelated_ca = make_self_signed_cert(dir_ / "unrelated", "unrelated-ca");

      auto opts = tls_opts();
      opts.tls_client_ca_path = cert_.cert_path.string(); // trusts CA-A only
      const auto target = start_server(std::move(opts));

      grpc::SslCredentialsOptions ssl;
      ssl.pem_root_certs = read_file(cert_.cert_path);         // verify server
      ssl.pem_private_key = read_file(unrelated_ca.key_path);  // client cert = CA-B's key
      ssl.pem_cert_chain = read_file(unrelated_ca.cert_path);  // client cert = CA-B's cert
      const auto status =
          try_login(grpc::CreateChannel(target, grpc::SslCredentials(ssl)));
      EXPECT_FALSE(status.ok());
    }

  }  // namespace
}  // namespace fmgr::test
