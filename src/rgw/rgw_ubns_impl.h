/**
 * @file rgw_ubns_impl.h
 * @author André Lucas (alucas@akamai.com)
 * @brief Unique Bucket Naming System (UBNS) private declarations.
 * @version 0.1
 * @date 2024-03-20
 *
 * @copyright Copyright (c) 2024
 *
 * Declarations for UBNSClientImpl and related classes.
 *
 * TRY REALLY HARD to not include this anywhere except rgw_ubns.cc and
 * rgw_ubns_impl.cc. In particular, don't add it to rgw_ubns.h no matter how
 * tempting that seems.
 *
 * This file pulls in the gRPC headers and we don't want that everywhere.
 */

#pragma once

#include <chrono>
#include <memory>
#include <shared_mutex>
#include <string>

#include <fmt/format.h>
#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>

#include "common/config_obs.h"
#include "rgw_ubns.h"

#include "ubdb/v1/ubdb.grpc.pb.h"

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_composed.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/dispatch.hpp>

#include "common/async/blocked_completion.h"

// Avoid including rgw_common.h here -- it transitively pulls in opentelemetry
// headers that fail to parse with GCC 11 / C++20.
#ifndef ERR_INTERNAL_ERROR
#define ERR_INTERNAL_ERROR 2200
#endif

namespace rgw {

/**
 * @brief Bridge a single gRPC callback-async call to any Asio completion
 * token.
 *
 * @param ctx The grpc::ClientContext for this call. Must outlive the
 *   returned async operation (in practice: a coroutine-frame local that's
 *   still in scope across the co_await). Used for two things:
 *   - If the completion handler has a connected associated cancellation
 *     slot, that slot is wired to call ctx.TryCancel() when a cancellation
 *     signal is emitted. This makes async_grpc_call() a well-behaved,
 *     cancellation-aware composed operation: if the enclosing coroutine's
 *     cancellation state is ever triggered (e.g. by a caller wrapping the
 *     call with a per-operation timeout/cancellation adapter), the
 *     in-flight gRPC call is told to stop instead of being silently
 *     orphaned. gRPC still delivers a normal completion afterwards (with a
 *     CANCELLED status) through the usual callback below -- TryCancel()
 *     does not bypass or race the completion path.
 *   - Not otherwise touched here; callers are expected to have already
 *     called ctx.set_deadline() themselves before invoking this function,
 *     since that's the primary deadline enforcement mechanism (native to
 *     gRPC, and in effect regardless of whether anything is connected to
 *     the cancellation slot).
 * @tparam StartFn A callable with signature
 *   void(std::function<void(grpc::Status)>).
 *   It should call stub->async()->SomeMethod(&ctx, &req, &resp, the_callback).
 * @tparam CompletionToken Any Asio completion token for void(grpc::Status).
 *
 * The gRPC library fires the callback on a gRPC-internal thread.  We
 * dispatch the result back to the executor associated with the
 * completion handler before invoking it, which ensures the Asio
 * coroutine (or any other handler) is resumed on its own executor.
 */
template <typename StartFn,
          boost::asio::completion_token_for<void(grpc::Status)> CompletionToken>
auto async_grpc_call(grpc::ClientContext& ctx, StartFn start_fn, CompletionToken&& token)
{
  return boost::asio::async_initiate<CompletionToken, void(grpc::Status)>(
      [&ctx, sf = std::move(start_fn)](auto handler) mutable {
        auto ex = boost::asio::get_associated_executor(handler);

        auto slot = boost::asio::get_associated_cancellation_slot(handler);
        if (slot.is_connected()) {
          slot.assign([&ctx](boost::asio::cancellation_type_t /*type*/) {
            // Ask gRPC to abort the in-flight call. This does not
            // complete the operation itself -- gRPC will still invoke
            // our completion callback below exactly once, just with a
            // CANCELLED (or similar) status instead of the call's normal
            // outcome.
            ctx.TryCancel();
          });
        }

        // gRPC's callback argument is std::function<void(grpc::Status)>, which
        // requires a copy-constructible target.  Asio completion handlers are
        // typically move-only.  Wrap the handler in a shared_ptr so the gRPC
        // callback lambda — which captures it by shared ownership — satisfies
        // std::function's copy-constructibility requirement.
        auto sh = std::make_shared<decltype(handler)>(std::move(handler));
        sf([ex, sh](grpc::Status s) mutable {
          // gRPC fires this on a gRPC-internal thread; dispatch the result
          // back to the handler's executor before invoking it.
          boost::asio::dispatch(ex,
              [sh = std::move(sh), s = std::move(s)]() mutable {
                std::move(*sh)(std::move(s));
              });
        });
      }, token);
}

/**
 * @brief Ceph configuration observer for UBNSClientImpl.
 *
 * Implemented as a template to make it possible to easily test the observer.
 *
 * Class T has to implement:
 * - get_default_channel_args(CephContext* cct) -> grpc::ChannelArguments
 * - set_channel_args(CephContext* cct, grpc::ChannelArguments args) -> void
 * - set_channel(CephContext* cct, const std::string& uri) -> void
 *
 * @tparam T A class implementing the functionality of the above-listed
 * methods. Typically UBNSClientImpl.
 */
template <typename T>
class UBNSConfigObserver final : public md_config_obs_t {
public:
  /**
   * @brief Construct a new UBNS Config Observer object with a
   * backreference to the owning template class, typically UBNSClientImpl.
   *
   * @param impl The UBNSClientImpl-like object.
   */
  explicit UBNSConfigObserver(T& impl)
      : impl_(impl)
  {
  }

  // Don't allow a default construction without the helper_.
  UBNSConfigObserver() = delete;

  /**
   * @brief Destructor. Remove the observer from the Ceph configuration system.
   *
   */
  ~UBNSConfigObserver()
  {
    if (cct_ && observer_added_) {
      cct_->_conf.remove_observer(this);
    }
  }

  void init(CephContext* cct)
  {
    cct_ = cct;
    cct_->_conf.add_observer(this);
    observer_added_ = true;
  }
  // Config observer. See notes in src/common/config_obs.h and for
  // ceph::md_config_obs_impl.

  std::vector<std::string> get_tracked_keys() const noexcept override
  {
    return {
      "rgw_ubns_grpc_arg_initial_reconnect_backoff_ms",
      "rgw_ubns_grpc_arg_max_reconnect_backoff_ms",
      "rgw_ubns_grpc_arg_min_reconnect_backoff_ms",
      "rgw_ubns_grpc_uri"
    };
  }

  void handle_conf_change(const ConfigProxy& conf,
      const std::set<std::string>& changed)
  {
    // You should bundle any gRPC arguments changes into this first block.
    if (changed.count("rgw_ubns_grpc_arg_initial_reconnect_backoff_ms") || changed.count("rgw_ubns_grpc_arg_max_reconnect_backoff_ms") || changed.count("rgw_ubns_grpc_arg_min_reconnect_backoff_ms")) {
      auto args = impl_.get_default_channel_args(cct_);
      impl_.set_channel_args(cct_, args);
    }
    // The gRPC channel change needs to come after the arguments setting, if any.
    if (changed.count("rgw_ubns_grpc_uri")) {
      impl_.set_channel(cct_, conf->rgw_ubns_grpc_uri);
    }
  }

private:
  T& impl_;
  CephContext* cct_ = nullptr;
  bool observer_added_ = false;
}; // class UBNSConfigObserver

/**
 * @brief Implementation class for the UBNS client.
 *
 * This class is created at UBNSClient construction, and houses all useful
 * functionality for UBNS.
 *
 * It has a few basic functions:
 * - Manage the persistent gRPC channel, supporting changes.
 * - Handle runtime configuration changes.
 * - Perform relevant gRPC calls to implement the UBNS API.
 *
 * The configuration observer is implemented as a member (\p config_obs_) of
 * type UBNSConfigObserver<UBNSCLientImpl>. It's templated this way to make it
 * easier to unit test the config observer.
 */
class UBNSClientImpl {

private:
  UBNSConfigObserver<UBNSClientImpl> config_obs_;
  std::string cluster_id_;
  // The gRPC channel pointer needs to be behind a mutex. Changing channel_,
  // channel_args_ or channel_uri_ must be under a unique lock of m_channel_.
  std::shared_mutex m_channel_;
  std::shared_ptr<grpc::Channel> channel_;
  std::optional<grpc::ChannelArguments> channel_args_;
  std::string channel_uri_;
  bool mtls_enabled_ = true; // Set only during init().

public:
  using chan_lock_t = std::shared_mutex;

  UBNSClientImpl()
      : config_obs_ { *this } {};
  ~UBNSClientImpl() {};

  UBNSClientImpl(const UBNSClientImpl&) = delete;
  UBNSClientImpl& operator=(const UBNSClientImpl&) = delete;
  UBNSClientImpl(UBNSClientImpl&&) = delete;
  UBNSClientImpl& operator=(UBNSClientImpl&&) = delete;

  /**
   * @brief Initialise the UBNS client.
   *
   * @param cct The context, for logging.
   * @param grpc_uri The URI. May be empty, in which case will be fetched from
   * config. (This is intended for use by unit tests.)
   * @return true Success.
   * @return false Failure. This is likely terminal as it means we failed to
   * create some data structures. It won't fail because a connection failed.
   */
  bool init(CephContext* cct, const std::string& grpc_uri);
  void shutdown();

  /**
   * @brief Asynchronously call ubdb.v1.AddBucketEntry() and deliver a
   * UBNSClientResult via the given completion token.
   *
   * Accepts any Asio completion token (use_awaitable, use_blocked,
   * yield_context, deferred, …).  When called with
   * ceph::async::use_blocked the behaviour is identical to the former
   * synchronous implementation.
   *
   * @param dpp        DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @param cluster_id  The cluster ID.
   * @param owner       The owner.
   * @param token       Any Asio completion token for void(UBNSClientResult).
   */
  template <boost::asio::completion_token_for<void(UBNSClientResult)> CT>
  auto add_bucket_entry(const DoutPrefixProvider* dpp,
                        const std::string& bucket_name,
                        const std::string& cluster_id,
                        const std::string& owner,
                        CT&& token)
  {
    // Log before entering the coroutine: GCC-12 ICEs on ldpp_dout inside
    // coroutine lambda bodies (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=103790).
    ldpp_dout(dpp, 20) << "UBNSClientImpl::add_bucket_entry" << dendl;
    ldpp_dout(dpp, 5)
        << fmt::format(FMT_STRING("UBNS: sending gRPC AddBucketRequest"
                                  "(bucket={},cluster={},owner={})"),
                       bucket_name, cluster_id, owner)
        << dendl;
    // Read the per-call deadline here, outside the coroutine, for the same
    // reason logging happens out here (see comment above): keep config/dpp
    // access out of the coroutine body where practical.
    const int deadline_ms = dpp->get_cct()->_conf->rgw_ubns_grpc_call_deadline_ms;
    return boost::asio::async_initiate<CT, void(UBNSClientResult)>(
        boost::asio::co_composed<void(UBNSClientResult)>(
            // Capture this-ptr and string args by value.  UBNSClientImpl is
            // neither copyable nor movable so it cannot be passed as an extra
            // async_initiate arg (deferred would try to decay-copy it).
            [this, dpp, bucket_name, cluster_id, owner, deadline_ms](auto /*state*/) -> void
            {
              // The whole body is wrapped in try/catch, not just the parts
              // that "obviously" can throw (e.g. allocation in NewStub(),
              // protobuf field setters, fmt::format in xform_result).  This
              // matters more here than in a plain synchronous call chain:
              // once we've suspended on the co_await below, resumption is
              // driven by dispatch() from gRPC's own completion callback,
              // which may run on a foreign (gRPC-internal) thread, or be
              // posted to run inside some unrelated io_context::run() call.
              // boost::asio::co_composed's promise::unhandled_exception()
              // simply rethrows -- it does NOT route the exception through
              // the normal completion-handler channel -- so an uncaught
              // exception here would escape into whichever context resumed
              // us, not back to our original caller's stack as it would in
              // the old fully-synchronous implementation. Catch everything
              // here and convert it into a normal UBNSClientResult so it's
              // always delivered safely through co_return.
              //
              // Note: co_return is deliberately kept out of the try/catch
              // entirely (single exit point via the 'result' local below).
              // GCC 12 has an internal compiler error (segfault) on a
              // co_return placed directly inside a catch handler for some
              // completion-token instantiations (e.g. CT =
              // ceph::async::use_blocked_t) -- the same general class of
              // GCC 12 coroutine bug as the ldpp_dout one noted above.
              UBNSClientResult result;
              try {
                auto channel = this->safe_get_channel(dpp);
                if (!channel) {
                  result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                      "Internal error (could not fetch gRPC channel)");
                } else {
                  auto stub = ubdb::v1::UBDBService::NewStub(channel);
                  ubdb::v1::AddBucketEntryRequest req;
                  req.set_bucket(bucket_name);
                  req.set_cluster(cluster_id);
                  req.set_owner(owner);
                  grpc::ClientContext ctx;
                  ctx.set_deadline(std::chrono::system_clock::now()
                      + std::chrono::milliseconds(deadline_ms));
                  ubdb::v1::AddBucketEntryResponse resp;
                  grpc::Status status = co_await async_grpc_call(ctx,
                      [&stub, &ctx, &req, &resp](auto cb) {
                        stub->async()->AddBucketEntry(&ctx, &req, &resp,
                                                       std::move(cb));
                      }, boost::asio::deferred);
                  result = this->_add_bucket_xform_result(status);
                }
              } catch (const std::exception& e) {
                // Deliberately not fmt::format(FMT_STRING(...)) here: that
                // combination (fmt::format with a FMT_STRING compile-time
                // format literal, inside a catch handler, inside a
                // co_composed coroutine instantiated for
                // ceph::async::use_blocked_t) triggers a GCC 12 internal
                // compiler error (segfault) -- the same general class of
                // bug as the ldpp_dout restriction noted above. Plain
                // string concatenation avoids it entirely.
                result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                    std::string("UBNS: AddBucketEntry: caught exception: ") + e.what());
              } catch (...) {
                result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                    "UBNS: AddBucketEntry: caught unknown exception");
              }
              co_return result;
            }),
        token);
  }

  /**
   * @brief Asynchronously call ubdb.v1.DeleteBucketEntry() and deliver a
   * UBNSClientResult via the given completion token.
   *
   * @param dpp        DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @param cluster_id  The cluster ID.
   * @param owner       The owner.
   * @param token       Any Asio completion token for void(UBNSClientResult).
   */
  template <boost::asio::completion_token_for<void(UBNSClientResult)> CT>
  auto delete_bucket_entry(const DoutPrefixProvider* dpp,
                           const std::string& bucket_name,
                           const std::string& cluster_id,
                           const std::string& owner,
                           CT&& token)
  {
    ldpp_dout(dpp, 20) << "UBNSClientImpl::delete_bucket_entry" << dendl;
    ldpp_dout(dpp, 5)
        << fmt::format(FMT_STRING("UBNS: sending gRPC DeleteBucketRequest"
                                  "(bucket={},cluster={},owner={})"),
                       bucket_name, cluster_id, owner)
        << dendl;
    const int deadline_ms = dpp->get_cct()->_conf->rgw_ubns_grpc_call_deadline_ms;
    return boost::asio::async_initiate<CT, void(UBNSClientResult)>(
        boost::asio::co_composed<void(UBNSClientResult)>(
            [this, dpp, bucket_name, cluster_id, owner, deadline_ms](auto /*state*/) -> void
            {
              // See add_bucket_entry() above for why the whole body needs
              // to be inside this try/catch, and why co_return is kept out
              // of the try/catch itself (single exit via 'result').
              UBNSClientResult result;
              try {
                auto channel = this->safe_get_channel(dpp);
                if (!channel) {
                  result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                      "Internal error (could not fetch gRPC channel)");
                } else {
                  auto stub = ubdb::v1::UBDBService::NewStub(channel);
                  ubdb::v1::DeleteBucketEntryRequest req;
                  req.set_bucket(bucket_name);
                  req.set_cluster(cluster_id);
                  req.set_owner(owner);
                  grpc::ClientContext ctx;
                  ctx.set_deadline(std::chrono::system_clock::now()
                      + std::chrono::milliseconds(deadline_ms));
                  ubdb::v1::DeleteBucketEntryResponse resp;
                  grpc::Status status = co_await async_grpc_call(ctx,
                      [&stub, &ctx, &req, &resp](auto cb) {
                        stub->async()->DeleteBucketEntry(&ctx, &req, &resp,
                                                           std::move(cb));
                      }, boost::asio::deferred);
                  result = this->_delete_bucket_xform_result(status);
                }
              } catch (const std::exception& e) {
                // See add_bucket_entry() above: avoid fmt::format(FMT_STRING)
                // inside a coroutine catch handler (GCC 12 ICE).
                result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                    std::string("UBNS: DeleteBucketEntry: caught exception: ") + e.what());
              } catch (...) {
                result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                    "UBNS: DeleteBucketEntry: caught unknown exception");
              }
              co_return result;
            }),
        token);
  }

  /**
   * @brief Asynchronously call ubdb.v1.UpdateBucketEntry() and deliver a
   * UBNSClientResult via the given completion token.
   *
   * @param dpp        DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @param cluster_id  The cluster ID.
   * @param owner       The owner.
   * @param state       The new bucket state.
   * @param token       Any Asio completion token for void(UBNSClientResult).
   */
  template <boost::asio::completion_token_for<void(UBNSClientResult)> CT>
  auto update_bucket_entry(const DoutPrefixProvider* dpp,
                           const std::string& bucket_name,
                           const std::string& cluster_id,
                           const std::string& owner,
                           UBNSBucketUpdateState state,
                           CT&& token)
  {
    ldpp_dout(dpp, 20) << "UBNSClientImpl::update_bucket_entry" << dendl;
    ldpp_dout(dpp, 1)
        << fmt::format(FMT_STRING("UBNS: sending gRPC UpdateBucketRequest"
                                  "(bucket={},cluster={},owner={},state={})"),
                       bucket_name, cluster_id, owner, to_str(state))
        << dendl;
    const int deadline_ms = dpp->get_cct()->_conf->rgw_ubns_grpc_call_deadline_ms;
    return boost::asio::async_initiate<CT, void(UBNSClientResult)>(
        boost::asio::co_composed<void(UBNSClientResult)>(
            [this, dpp, bucket_name, cluster_id, owner, state, deadline_ms](auto /*state_coro*/) -> void
            {
              // See add_bucket_entry() above for why the whole body needs
              // to be inside this try/catch, and why co_return is kept out
              // of the try/catch itself (single exit via 'result').
              UBNSClientResult result;
              try {
                auto channel = this->safe_get_channel(dpp);
                if (!channel) {
                  result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                      "Internal error (could not fetch gRPC channel)");
                } else {
                  auto stub = ubdb::v1::UBDBService::NewStub(channel);
                  ubdb::v1::UpdateBucketEntryRequest req;
                  req.set_bucket(bucket_name);
                  req.set_cluster(cluster_id);
                  req.set_owner(owner);
                  ubdb::v1::BucketState rpc_state;
                  switch (state) {
                  case rgw::UBNSBucketUpdateState::UNSPECIFIED:
                    rpc_state = ubdb::v1::BucketState::BUCKET_STATE_UNSPECIFIED;
                    break;
                  case rgw::UBNSBucketUpdateState::CREATED:
                    rpc_state = ubdb::v1::BucketState::BUCKET_STATE_CREATED;
                    break;
                  case rgw::UBNSBucketUpdateState::DELETING:
                    rpc_state = ubdb::v1::BucketState::BUCKET_STATE_DELETING;
                    break;
                  }
                  req.set_state(rpc_state);
                  grpc::ClientContext ctx;
                  ctx.set_deadline(std::chrono::system_clock::now()
                      + std::chrono::milliseconds(deadline_ms));
                  ubdb::v1::UpdateBucketEntryResponse resp;
                  grpc::Status status = co_await async_grpc_call(ctx,
                      [&stub, &ctx, &req, &resp](auto cb) {
                        stub->async()->UpdateBucketEntry(&ctx, &req, &resp,
                                                           std::move(cb));
                      }, boost::asio::deferred);
                  result = this->_update_bucket_xform_result(status);
                }
              } catch (const std::exception& e) {
                // See add_bucket_entry() above: avoid fmt::format(FMT_STRING)
                // inside a coroutine catch handler (GCC 12 ICE).
                result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                    std::string("UBNS: UpdateBucketEntry: caught exception: ") + e.what());
              } catch (...) {
                result = UBNSClientResult::error(ERR_INTERNAL_ERROR,
                    "UBNS: UpdateBucketEntry: caught unknown exception");
              }
              co_return result;
            }),
        token);
  }

  std::string cluster_id() const
  {
    if (cluster_id_.empty()) {
      throw new std::runtime_error("UBNSClientImpl: cluster ID not set");
    }
    return cluster_id_;
  }

  /**
   * @brief Set the gRPC channel URI.
   *
   * This is used by init() and by the config observer. If no channel
   * arguments have been set via set_channel_args(), this will set them to the
   * default values (via get_default_channel_args).
   *
   * Do not call from an RGWOp unless you _know_ you've not taken a lock on
   * m_config_!
   *
   * @param cct The Ceph context for logging and config.
   * @param grpc_uri The URI of the gRPC server. If empty, use the configured
   * value. Non-empty is intended for testing.
   * @return true on success.
   * @return false on failure.
   */
  bool _set_insecure_channel(CephContext* const cct, const std::string& grpc_uri);

  /**
   * @brief Set up the gRPC channel in mTLS mode.
   *
   * This will use configuration variables to configure the channel. We assume
   * that the configuration has already been validated, but this can still
   * fail for any number of reasons.
   *
   * @param cct The Ceph Context for logging and config.
   * @param grpc_uri The URI of the gRPC server. If empty, use the configured
   * value. Non-empty is intended for testing.
   * @return true on success.
   * @return false on failure.
   */
  bool _set_mtls_channel(CephContext* const cct, const std::string& grpc_uri);

  /**
   * @brief Set up the gRPC channel.
   *
   * Based on the value of mtls_enabled_, set up the gRPC channel. Calls
   * either _set_insecure_channel() or _set_mtls_channel() as appropriate.
   *
   * @param cct The Ceph Context for logging and config.
   * @param grpc_uri The URI of the gRPC server. If empty, use the configured
   * value. Non-empty is intended for testing.
   * @return true on success.
   * @return false on failure.
   */
  bool set_channel(CephContext* const cct, const std::string& grpc_uri)
  {
    if (mtls_enabled_) {
      return _set_mtls_channel(cct, grpc_uri);
    } else {
      return _set_insecure_channel(cct, grpc_uri);
    }
  }

  /**
   * @brief Get our default grpc::ChannelArguments value.
   *
   * When calling set_channel_args(), you should first call this function to
   * get application defaults, and then modify the settings you need.
   *
   * Currently the backoff timers are set here, based on configuration
   * variables. These are runtime-alterable, but have sensible defaults.
   *
   * @return grpc::ChannelArguments A default set of channel arguments.
   */
  grpc::ChannelArguments get_default_channel_args(CephContext* const cct);

  /**
   * @brief Set custom gRPC channel arguments. Intended for testing.
   *
   * You should modify the default channel arguments obtained with
   * get_default_channel_args(). Don't start from scratch.
   *
   * Keep this simple. If you set vtable args you'll need to worry about the
   * lifetime of those is longer than the UBNSHelperImpl object that will
   * store a copy of the ChannelArguments object.
   *
   * Do not call from an RGWOp unless you _know_ you've not taken a lock on
   * m_config_!
   *
   * @param args A populated grpc::ChannelArguments object.
   */
  void set_channel_args(CephContext* const cct, grpc::ChannelArguments& args)
  {
    std::unique_lock l { m_channel_ };
    channel_args_ = std::make_optional(args);
  }

private:
  /**
   * @brief Safely fetch a copy of the current gRPC channel pointer under the
   * channel shared mutex.
   *
   * @param dpp DoutPrefixProvider.
   * @return std::shared_ptr<grpc::Channel> The channel pointer, or nullptr
   * on failure (e.g. channel not yet set up).
   */
  std::shared_ptr<grpc::Channel> safe_get_channel(const DoutPrefixProvider* dpp);

  /// @brief Return a UBNSClientResult object based on the return from the
  /// AddBucketEntry service.
  UBNSClientResult _add_bucket_xform_result(const grpc::Status& status);
  /// @brief Return a UBNSClientResult object based on the return from the
  /// DeleteBucketEntry service.
  UBNSClientResult _delete_bucket_xform_result(const grpc::Status& status);
  /// @brief Return a UBNSClientResult object based on the return from the
  /// UpdateBucketEntry service.
  UBNSClientResult _update_bucket_xform_result(const grpc::Status& status);

}; // class UBNSClientImpl

} // namespace rgw
