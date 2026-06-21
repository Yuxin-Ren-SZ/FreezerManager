// SPDX-License-Identifier: AGPL-3.0-or-later

#include "qt/GrpcChannel.h"

#include <gtest/gtest.h>

namespace {

using fmgr::qt::GrpcChannel;

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

}  // namespace
