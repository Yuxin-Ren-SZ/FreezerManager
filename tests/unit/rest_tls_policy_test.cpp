// SPDX-License-Identifier: AGPL-3.0-or-later

// Unit coverage for the REST listener TLS policy (review C6).

#include "server/RestTlsPolicy.h"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

  using fmgr::server::is_loopback_host;
  using fmgr::server::RestTlsConfig;
  using fmgr::server::validate_rest_tls_policy;

  RestTlsConfig config(std::string host, bool cert, bool key, bool require_tls) {
    return RestTlsConfig{.host = std::move(host),
                         .port = 8080,
                         .has_cert = cert,
                         .has_key = key,
                         .require_tls = require_tls};
  }

  TEST(RestTlsPolicy, LoopbackHostsRecognized) {
    EXPECT_TRUE(is_loopback_host("127.0.0.1"));
    EXPECT_TRUE(is_loopback_host("::1"));
    EXPECT_TRUE(is_loopback_host("localhost"));
    EXPECT_FALSE(is_loopback_host("0.0.0.0"));
    EXPECT_FALSE(is_loopback_host("::"));
    EXPECT_FALSE(is_loopback_host(""));
    EXPECT_FALSE(is_loopback_host("10.0.0.5"));
  }

  // Half-configured cert/key is rejected regardless of require_tls.
  TEST(RestTlsPolicy, HalfConfiguredCertKeyRejected) {
    EXPECT_THROW(validate_rest_tls_policy(config("127.0.0.1", true, false, false)),
                 std::invalid_argument);
    EXPECT_THROW(validate_rest_tls_policy(config("0.0.0.0", false, true, true)),
                 std::invalid_argument);
  }

  // Under require_tls a public plaintext REST bind is refused.
  TEST(RestTlsPolicy, RequireTlsRejectsPublicPlaintext) {
    EXPECT_THROW(validate_rest_tls_policy(config("0.0.0.0", false, false, true)),
                 std::invalid_argument);
  }

  // Under require_tls a loopback plaintext bind is allowed (reverse proxy).
  TEST(RestTlsPolicy, RequireTlsAllowsLoopbackPlaintext) {
    EXPECT_NO_THROW(validate_rest_tls_policy(config("127.0.0.1", false, false, true)));
  }

  // Under require_tls a fully-configured TLS bind is allowed on any host.
  TEST(RestTlsPolicy, RequireTlsAllowsConfiguredTls) {
    EXPECT_NO_THROW(validate_rest_tls_policy(config("0.0.0.0", true, true, true)));
  }

  // Without require_tls a public plaintext bind is allowed (dev default).
  TEST(RestTlsPolicy, NoRequireTlsAllowsPlaintext) {
    EXPECT_NO_THROW(validate_rest_tls_policy(config("0.0.0.0", false, false, false)));
  }

} // namespace
