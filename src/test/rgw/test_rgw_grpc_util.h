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
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fmt/format.h>
#include <functional>
#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/server_callback.h>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

/**
 * @brief A minimal single-worker-thread task executor.
 *
 * Callback-based (async) gRPC test service implementations can use this to
 * complete an RPC -- i.e. call `reactor->Finish(status)` -- asynchronously,
 * on a thread that's dedicated to this executor and therefore guaranteed to
 * be distinct from both the calling gRPC-library thread and the
 * GRPCTestServer's own accept/Wait() thread.
 *
 * This matters for tests: gRPC's own internal completion-queue threads are
 * an implementation detail whose identity and timing tests can't observe or
 * control. Posting the completion through this executor instead gives tests
 * a real, deterministic "the result arrived on a foreign thread" scenario to
 * exercise -- e.g. to verify that client-side code correctly resumes on its
 * own associated executor rather than silently continuing on whatever
 * thread happened to deliver the result.
 */
class GRPCAsyncExecutor {
public:
  GRPCAsyncExecutor()
      : thread_([this] { run(); })
  {
  }

  ~GRPCAsyncExecutor()
  {
    {
      std::unique_lock l { m_ };
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  GRPCAsyncExecutor(const GRPCAsyncExecutor&) = delete;
  GRPCAsyncExecutor& operator=(const GRPCAsyncExecutor&) = delete;
  GRPCAsyncExecutor(GRPCAsyncExecutor&&) = delete;
  GRPCAsyncExecutor& operator=(GRPCAsyncExecutor&&) = delete;

  /**
   * @brief Schedule fn to run asynchronously on this executor's dedicated
   * worker thread.
   *
   * @param fn The function to run. Called exactly once, on the worker
   * thread, in the order posted.
   */
  void post(std::function<void()> fn)
  {
    {
      std::unique_lock l { m_ };
      queue_.push_back(std::move(fn));
    }
    cv_.notify_all();
  }

  /**
   * @brief Return the id of this executor's dedicated worker thread.
   *
   * Useful for tests that want to assert a completion really did happen on
   * a thread other than the one that received the RPC.
   */
  std::thread::id thread_id() const noexcept
  {
    return thread_.get_id();
  }

private:
  void run()
  {
    for (;;) {
      std::function<void()> fn;
      {
        std::unique_lock l { m_ };
        cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stop_) {
            return;
          }
          continue;
        }
        fn = std::move(queue_.front());
        queue_.pop_front();
      }
      fn();
    }
  }

  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool stop_ = false;
  // Must be initialised last: the constructor starts the thread running
  // run(), which touches m_/cv_/queue_/stop_ above.
  std::thread thread_;
};

/**
 * @brief A stop-and-startable gRPC server for testing.
 *
 * @tparam T A gRPC server implementation class. May be a classic
 * synchronous grpc::Service, or a callback-based service (deriving from a
 * generated ...::CallbackService). If T is constructible from a
 * GRPCAsyncExecutor&, that executor (see async_executor()) is passed to it
 * so it can complete RPCs asynchronously on a dedicated thread; otherwise T
 * is default-constructed, unchanged from before.
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
  std::string host_;
  std::string address_;
  std::unique_ptr<grpc::Server> server_;
  // Available for T to use for asynchronous RPC completion; see
  // GRPCAsyncExecutor and async_executor() below.
  GRPCAsyncExecutor async_executor_;

public:
  /**
   * @brief Construct a new GRPCTestServer object. Don't start the server,
   *
   * Some tests don't want the server to be running right away.
   */
  GRPCTestServer()
  {
    set_address("127.0.0.1", random_port());
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
    host_ = host;
    // Client-facing target: use the resolver-less "ipv4:" scheme rather
    // than "dns:". The target is always a literal loopback IP address that
    // needs no resolution, and "dns:" makes gRPC spin up its asynchronous
    // (c-ares-based) resolver machinery regardless -- which runs its own
    // background threads with state that isn't necessarily torn down
    // synchronously when a grpc::Channel using it is destroyed. Across the
    // many short-lived channels this test suite creates in quick succession
    // (one or more per TEST_F, now genuinely running concurrently under
    // boost::asio::yield_context instead of serializing everything via
    // blocking calls), that leftover resolver state raced with new
    // channels/servers and crashed inside gRPC's own internals. "ipv4:"
    // bypasses the resolver entirely -- there's nothing left to race.
    address_ = fmt::format("ipv4:{}:{}", host_, port_);
  }
  uint16_t port() { return port_; }

  /**
   * @brief Return the plain "host:port" listen address for
   * grpc::ServerBuilder::AddListeningPort().
   *
   * Unlike the client-facing address() above, AddListeningPort() does not
   * accept a resolver-style "scheme:" prefix (e.g. "ipv4:") -- it wants a
   * bare host:port (or host:0 to have the OS choose a port).
   */
  std::string listen_address() { return fmt::format("{}:{}", host_, port_); }

  /**
   * @brief Return the executor available for T to use to complete RPCs
   * asynchronously, on a thread dedicated to this server and distinct from
   * gRPC's own internal threads.
   *
   * Safe to call before start(): the executor's worker thread is created
   * along with the GRPCTestServer object itself.
   */
  GRPCAsyncExecutor& async_executor() { return async_executor_; }

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
      // If T can be constructed from our GRPCAsyncExecutor&, do so, so it
      // can complete RPCs asynchronously on our dedicated thread. Otherwise
      // fall back to default-construction, unchanged from before -- this
      // keeps existing default-constructible test services working with no
      // changes required.
      if constexpr (std::is_constructible_v<T, GRPCAsyncExecutor&>) {
        T service(async_executor_);
        run_server(service);
      } else {
        T service;
        run_server(service);
      }
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
  }

  static constexpr uint16_t port_base = 58000;
  static constexpr uint16_t port_range = 2000;

  /**
   * @brief Return a port that hasn't been handed out before by this process.
   *
   * gRPC keeps a *process-global* subchannel pool and connectivity/name
   * resolution bookkeeping for each address, keyed on host:port (plus
   * channel args). That bookkeeping isn't necessarily torn down
   * synchronously when a grpc::Server/grpc::Channel using that address is
   * destroyed -- some of it finishes asynchronously on gRPC's own internal
   * threads. If two GRPCTestServer instances in the same process pick the
   * *same* port -- entirely possible with independent random draws from a
   * range this small, especially across the many fixtures a full test
   * binary run creates -- the new server/channel for that address can race
   * with gRPC's still-in-progress cleanup of the old one, up to and
   * including crashing inside GlobalSubchannelPool/SubchannelKey.
   *
   * This was effectively masked before this test suite exercised the fully
   * asynchronous UBNS path (via boost::asio::yield_context): the previous
   * synchronous/blocking usage serialized enough gRPC activity that the
   * race rarely had a chance to manifest. Under real async use it's a real
   * bug, so rather than just reduce the odds, eliminate them: combine a
   * per-process random starting point (so concurrent test binaries on the
   * same host still tend to pick different ranges) with a monotonically
   * increasing counter, guaranteeing this process never hands out the same
   * port twice.
   */
  static uint16_t random_port()
  {
    static const uint16_t start_offset = [] {
      absl::BitGen bitgen;
      return absl::Uniform(bitgen, 0u, port_range);
    }();
    static std::atomic<uint16_t> counter { 0 };
    uint16_t offset = counter.fetch_add(1, std::memory_order_relaxed);
    return port_base + static_cast<uint16_t>((start_offset + offset) % port_range);
  }

private:
  /// Build, start and Wait() on a grpc::Server for the given service
  /// instance. Common to both the default-constructed and
  /// executor-constructed cases in start() above.
  void run_server(T& service)
  {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_address(), grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    server_ = builder.BuildAndStart();
    if (!server_) {
      fmt::print(stderr, "Failed to BuildAndStart() for {}\n", listen_address());
      // Must clear this or server().start() will hang.
      initialising = false;
      return;
    }
    running = true;
    initialising = false;
    fmt::print(stderr, "Calling server_->Wait() for {}\n", address());
    server_->Wait();
    running = false;
  }
};
