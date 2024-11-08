/**
 * @file test_rgw_grpc_util.h
 * @author André Lucas (alucas@akamai.com)
 * @brief Utilities for gRPC integration in RGW.
 * @version 0.1
 * @date 2023-11-21
 *
 * @copyright Copyright (c) 2023
 *
 */

#include <absl/random/random.h>
#include <atomic>
#include <boost/algorithm/hex.hpp>
#include <boost/regex.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * @brief A stop-and-startable gRPC server for testing.
 *
 * @tparam T A gRPC server implementation class.
 */
template <typename T>
class GRPCTestServer final {

protected:
  std::thread server_thread_;
  // Used to prevent fast startup/shutdown problems. (The Null test.)
  std::atomic<bool> initialising = false;
  // True if the server is actually running (in Wait()).
  std::atomic<bool> running = false;
  uint16_t port_;
  std::string address_;
  std::unique_ptr<grpc::Server> server_;

  std::shared_ptr<T> service_instance_;

public:
  /**
   * @brief Construct a new GRPCTestServer object. Don't start the server,
   *
   * Some tests don't want the server to be running right away.
   */
  GRPCTestServer()
  {
    set_address("dns:127.0.0.1", random_port());
  }

  // Copying and moving this server makes no sense.
  GRPCTestServer(const GRPCTestServer&) = delete;
  GRPCTestServer(GRPCTestServer&&) = delete;
  GRPCTestServer& operator=(const GRPCTestServer&) = delete;
  GRPCTestServer&& operator=(const GRPCTestServer&&) = delete;

  /**
   * @brief Destroy the GRPCTestServer object and stop any running server.
   *
   */
  virtual ~GRPCTestServer()
  {
    stop();
  }

  std::string address() { return address_; }
  void set_address(const std::string& host, uint16_t port)
  {
    port_ = port;
    address_ = fmt::format("dns:127.0.0.1:{}", port_);
  }
  uint16_t port() { return port_; }

  /**
   * @brief Start a gRPC server for T in a thread.
   *
   * Set some atomics in the instance so we can keep track of startup
   * progress.
   *
   * It's safe to call this multiple times.
   */
  void start()
  {
    if (initialising || running) {
      return;
    }
    initialising = true;
    server_thread_ = std::thread([this]() {
      // T service;
      service_instance_ = std::make_shared<T>();
      grpc::ServerBuilder builder;
      builder.AddListeningPort(address(), grpc::InsecureServerCredentials());
      builder.RegisterService(service_instance_.get());
      server_ = builder.BuildAndStart();
      if (!server_) {
        fmt::print(stderr, "Failed to BuildAndStart() for {}\n", address());
        // Must clear this or server().start() will hang.
        initialising = false;
        return;
      }
      running = true;
      initialising = false;
      fmt::print(stderr, "Calling server_->Wait() for {}\n", address());
      server_->Wait();
      running = false;
    });
    while (initialising)
      ;
  }

  /**
   * @brief Stop the server if it's running and join the server thread.
   *
   * It's safe to call this multiple times.
   */
  void stop()
  {
    while (initialising)
      ;
    if (running && server_) {
      server_->Shutdown();
    }
    if (server_thread_.joinable()) {
      server_thread_.join();
    }
    service_instance_.reset();
  }

  /**
   * @brief Get a shared pointer to the service instance. This makes it easy
   * to call methods on your service class.
   *
   * Throws a runtime_error if the server is not running.
   *
   * @return std::shared_ptr<T> pointer to the running service instance.
   */
  std::shared_ptr<T> instance()
  {
    if (!running) {
      throw new std::runtime_error("no running service instance");
    }
    return service_instance_;
  }

  static constexpr uint16_t port_base = 58000;
  static constexpr uint16_t port_range = 2000;

  static uint16_t random_port()
  {
    absl::BitGen bitgen;
    uint16_t rand = absl::Uniform(bitgen, 0u, port_range);
    return port_base + rand;
  }
};
