/**
 * @file test_rgw_grpc.cc
 * @author André Lucas (alucas@akamai.com)
 * @brief Unit tests for gRPC integration in RGW.
 * @version 0.1
 * @date 2023-11-07
 *
 * @copyright Copyright (c) 2023
 *
 */

#include <memory>

#include <absl/random/random.h>
#include <fmt/format.h>
#include <gmock/gmock.h>
#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <gtest/gtest.h>

#include "test_rgw_grpc_util.h"

#include "rgw/test/v1/test_rgw_grpc.grpc.pb.h"

using namespace ::rgw::test::v1;

/**
 * @brief Minimal gRPC client wrapper for rgw::test::v1::RgwGrpcTestService.
 *
 * Initialised with a grpc::Channel.
 */
class TestClient {
private:
  std::unique_ptr<RgwGrpcTestService::Stub> stub_;

public:
  TestClient(std::shared_ptr<grpc::Channel> channel)
      : stub_(RgwGrpcTestService::NewStub(channel))
  {
  }

  std::optional<std::string> Ping(const std::string message)
  {
    grpc::ClientContext context;
    PingRequest req;
    PingResponse resp;
    req.set_message("foo");
    grpc::Status status = stub_->Ping(&context, req, &resp);
    if (!status.ok()) {
      return std::nullopt;
    }
    return std::make_optional(resp.message());
  }
};

/**
 * @brief Minimal implementation of rgw::test::v1::RgwGrpcTestService.
 *
 */
class TestImpl final : public RgwGrpcTestService::Service {
  grpc::Status Ping(grpc::ServerContext* context, const PingRequest* request, PingResponse* response) override
  {
    response->set_message(request->message());
    return grpc::Status::OK;
  }
};

/**
 * @brief Test fixture. Most work is delegated to GRPCTestServer<TestImpl>.
 */
class TestGrpcService : public ::testing::Test {
private:
  GRPCTestServer<TestImpl> server_;

protected:
  void TearDown() { server_.stop(); }
  GRPCTestServer<TestImpl>& server() { return server_; }
};

TEST_F(TestGrpcService, Null)
{
}

// Make sure server().start() is idempotent.
TEST_F(TestGrpcService, MetaStart)
{
  server().start();
  for (int n = 0; n < 1000; n++) {
    server().start();
  }
  server().stop();
}

// Make sure server().stop() is idempotent.
TEST_F(TestGrpcService, MetaStop)
{
  server().start();
  for (int n = 0; n < 1000; n++) {
    server().stop();
  }
}

TEST_F(TestGrpcService, PingWorksWithServer)
{
  server().start();
  auto channel = grpc::CreateChannel(server().address(), grpc::InsecureChannelCredentials());
  TestClient client { channel };
  auto message = client.Ping("foo");
  EXPECT_TRUE(message.has_value()) << "Ping failed";
  if (message.has_value()) {
    EXPECT_EQ(*message, "foo");
  }
  server().stop();
}

TEST_F(TestGrpcService, PingFailsWithNoServer)
{
  auto channel = grpc::CreateChannel(server().address(), grpc::InsecureChannelCredentials());
  TestClient client { channel };
  auto message = client.Ping("foo");
  EXPECT_FALSE(message.has_value()) << "Ping succeeded when it should have failed";
}
