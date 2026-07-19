// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/GrpcChannel.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

  using fmgr::qt::GrpcChannel;
  using fmgr::qt::TlsOptions;

  TEST(GrpcChannelTest, DefaultTarget) {
    GrpcChannel channel;
    EXPECT_EQ(channel.target(), "0.0.0.0:50051");
  }

  TEST(GrpcChannelTest, CustomTarget) {
    GrpcChannel channel("lab.example.org:50052");
    EXPECT_EQ(channel.target(), "lab.example.org:50052");
  }

  TEST(GrpcChannelTest, NotConnectedBeforeConnect) {
    GrpcChannel channel;
    EXPECT_FALSE(channel.isConnected());
    EXPECT_EQ(channel.channel(), nullptr);
  }

  TEST(GrpcChannelTest, ConnectCreatesChannel) {
    GrpcChannel channel;
    // Lazy: gRPC does not dial until the first RPC, so connect() succeeds without
    // a live server.
    EXPECT_TRUE(channel.connect());
    EXPECT_TRUE(channel.isConnected());
    EXPECT_NE(channel.channel(), nullptr);
  }

  TEST(GrpcChannelTest, AuthStubCreatedAfterConnect) {
    GrpcChannel channel;
    channel.connect();
    auto stub = channel.makeAuthStub();
    EXPECT_NE(stub, nullptr);
  }

  TEST(GrpcChannelTest, AuthStubNullWhenNotConnected) {
    GrpcChannel channel;
    EXPECT_EQ(channel.makeAuthStub(), nullptr);
  }

  TEST(GrpcChannelTest, LabAndBoxStubsCreatedAfterConnect) {
    GrpcChannel channel;
    channel.connect();
    EXPECT_NE(channel.makeLabStub(), nullptr);
    EXPECT_NE(channel.makeBoxStub(), nullptr);
  }

  TEST(GrpcChannelTest, LabAndBoxStubsNullWhenNotConnected) {
    GrpcChannel channel;
    EXPECT_EQ(channel.makeLabStub(), nullptr);
    EXPECT_EQ(channel.makeBoxStub(), nullptr);
  }

  TEST(GrpcChannelTest, TlsDisabledByDefault) {
    GrpcChannel channel;
    EXPECT_FALSE(channel.tls().enabled);
  }

  TEST(GrpcChannelTest, TlsWithSystemRootsConnects) {
    // Empty rootCaPath means "use the system trust store" — no file to read, so
    // the (lazy) channel is created.
    GrpcChannel channel("lab.example.org:50051", TlsOptions{true, ""});
    EXPECT_TRUE(channel.connect());
    EXPECT_TRUE(channel.isConnected());
  }

  // The security-critical case: a TLS channel whose configured root CA cannot be
  // read must fail, never quietly fall back to an insecure channel.
  TEST(GrpcChannelTest, TlsWithUnreadableRootCaFailsWithoutFallback) {
    const auto missing = std::filesystem::temp_directory_path() / "fmgr-no-such-ca-bundle.pem";
    std::filesystem::remove(missing);

    GrpcChannel channel("lab.example.org:50051", TlsOptions{true, missing.string()});
    EXPECT_FALSE(channel.connect());
    EXPECT_FALSE(channel.isConnected());
    EXPECT_EQ(channel.channel(), nullptr);
  }

  TEST(GrpcChannelTest, TlsWithEmptyRootCaFileFails) {
    const auto empty_ca = std::filesystem::temp_directory_path() / "fmgr-empty-ca-bundle.pem";
    { std::ofstream(empty_ca, std::ios::binary); }

    GrpcChannel channel("lab.example.org:50051", TlsOptions{true, empty_ca.string()});
    EXPECT_FALSE(channel.connect());
    EXPECT_FALSE(channel.isConnected());

    std::filesystem::remove(empty_ca);
  }

  TEST(GrpcChannelTest, TlsWithReadableRootCaConnects) {
    const auto ca_path = std::filesystem::temp_directory_path() / "fmgr-fake-ca.pem";
    {
      std::ofstream file(ca_path, std::ios::binary);
      // Contents are never parsed until an RPC is attempted; connect() only has
      // to prove the bundle was readable.
      file << "-----BEGIN CERTIFICATE-----\nplaceholder\n-----END CERTIFICATE-----\n";
    }

    GrpcChannel channel("lab.example.org:50051", TlsOptions{true, ca_path.string()});
    EXPECT_TRUE(channel.connect());
    EXPECT_TRUE(channel.isConnected());

    std::filesystem::remove(ca_path);
  }

  // After a failed TLS connect (unreadable CA), a subsequent plaintext connect
  // must work — the failure must not leave the channel in a permanently broken
  // state.  The caller might retry with a corrected config.
  TEST(GrpcChannelTest, PlaintextConnectWorksAfterFailedTls) {
    const auto missing =
        std::filesystem::temp_directory_path() / "fmgr-no-such-ca-bundle.2.pem";
    std::filesystem::remove(missing);

    {
      GrpcChannel channel("lab.example.org:50051", TlsOptions{true, missing.string()});
      EXPECT_FALSE(channel.connect());
    }
    {
      GrpcChannel channel("lab.example.org:50051"); // plaintext
      EXPECT_TRUE(channel.connect());
      EXPECT_TRUE(channel.isConnected());
    }
  }

} // namespace
