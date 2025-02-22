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

namespace rgw {

/**
 * @brief Thin wrapper around the gRPC client.
 *
 * Error return codes are based on the following guidance from the service
 * developers:
 *
 * ```
 * Create:
 *    Internal errors: grpc code Internal
 *    Terminated due to context: grpc code DeadlineExceeded
 *    Invalid or missing parameter: grpc code InvalidArgument
 *    Aborted due to being duplicated: grpc code FailedPrecondition
 *    User already owns bucket: grpc code AlreadyExists
 *    Another user owns the bucket: grpc code Aborted
 *    OK: nil
 *
 * Update:
 *    Internal errors: grpc code Internal
 *    Terminated due to context: grpc code DeadlineExceeded
 *    Invalid or missing parameter: grpc code InvalidArgument
 *    Invalid state transition (start a delete for a bucket not yet marked as created in ubns): grpc code InvalidArgument
 *    BucketEntry not found: grpc code NotFound
 *    Aborted due to being duplicated: grpc code FailedPrecondition
 *    Bucket is hosted on another cluster: grpc code FailedPrecondition
 *    OK: nil
 *
 * Delete:
 *    Internal errors: grpc code Internal
 *    Terminated due to context: grpc code DeadlineExceeded
 *    Invalid or missing parameter: grpc code InvalidArgument
 *    Bucket is hosted on another cluster: grpc code FailedPrecondition
 *    OK: nil
 * ```
 *
 * Note that in two instances in Update, we get the same error code for
 * multiple causes. The error message we return will list both potential
 * causes - what else can we do?
 *
 */
class UBNSgRPCClient {
private:
  std::unique_ptr<ubdb::v1::UBDBService::Stub> stub_;

public:
  /**
   * @brief Construct a new UBNSgRPCClient object with no initial stub. You
   * must call set_stub() before using any RPC!
   */
  UBNSgRPCClient() {};

  /**
   * @brief Construct a new UBNSgRPCClient object given a gRPC channel.
   */
  explicit UBNSgRPCClient(std::shared_ptr<::grpc::Channel>) {};
  ~UBNSgRPCClient() {};

  // Can't copy with a unique_ptr.
  UBNSgRPCClient(const UBNSgRPCClient&) = delete;
  UBNSgRPCClient& operator=(const UBNSgRPCClient&) = delete;
  // Move is fine.
  UBNSgRPCClient(UBNSgRPCClient&&) = default;
  UBNSgRPCClient& operator=(UBNSgRPCClient&&) = default;

  /**
   * @brief Set the gRPC stub for this object.
   *
   * @param channel the gRPC channel pointer.
   */
  void set_stub(std::shared_ptr<::grpc::Channel> channel)
  {
    stub_ = ubdb::v1::UBDBService::NewStub(channel);
  }

  /**
   * @brief Call the AddBucketEntry service and return a result suitable for
   * returning to RGW.
   *
   * See the class documentation for the table of gRPC codes to RGW codes.
   *
   * @param req The request object.
   * @return UBNSClientResult The result object.
   */
  UBNSClientResult add_bucket_request(const ubdb::v1::AddBucketEntryRequest& req);

  /**
   * @brief Call the DeleteBucketEntry service and return a result suitable for
   * returning to RGW.
   *
   * See the class documentation for the table of gRPC codes to RGW codes.
   *
   * @param req The request object.
   * @return UBNSClientResult The result object.
   */
  UBNSClientResult delete_bucket_request(const ubdb::v1::DeleteBucketEntryRequest& req);

  /**
   * @brief Call the UpdateBucketEntry service and return a result suitable for
   * returning to RGW.
   *
   * See the class documentation for the table of gRPC codes to RGW codes.
   *
   * @param req The request object.
   * @return UBNSClientResult The result object.
   */
  UBNSClientResult update_bucket_request(const ubdb::v1::UpdateBucketEntryRequest& req);

  /// @brief Return a UBNSClientResult object based on the return from the
  /// AddBucketEntry service.
  UBNSClientResult _add_bucket_xform_result(const grpc::Status& status);
  /// @brief Return a UBNSClientResult object based on the return from the
  /// DeleteBucketEntry service.
  UBNSClientResult _delete_bucket_xform_result(const grpc::Status& status);
  /// @brief Return a UBNSClientResult object based on the return from the
  /// UpdateBucketEntry service.
  UBNSClientResult _update_bucket_xform_result(const grpc::Status& status);

}; // class UBNSgRPCClient

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

  const char** get_tracked_conf_keys() const
  {
    static const char* keys[] = {
      "rgw_ubns_grpc_arg_initial_reconnect_backoff_ms",
      "rgw_ubns_grpc_arg_max_reconnect_backoff_ms",
      "rgw_ubns_grpc_arg_min_reconnect_backoff_ms",
      "rgw_ubns_grpc_uri",
      nullptr
    };
    return keys;
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
   * @brief Call ubdb.v1.AddBucketEntry() and return the result.
   *
   * @param dpp DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @return UBNSClientResult A result object.
   */
  UBNSClientResult add_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner);

  /**
   * @brief Call ubdb.v1.DeleteBucketEntry() and return the result.
   *
   * @param dpp DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @return UBNSClientResult A result object.
   */
  UBNSClientResult delete_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner);

  /**
   * @brief Call ubdb.v1.UpdateBucketEntry() and return the result.
   *
   * @param dpp DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @return UBNSClientResult A result object.
   */
  UBNSClientResult update_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner, UBNSBucketUpdateState state);

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
   * @brief Safely fetch a UBNSgRPCClient object from under the channel shared mutex.
   *
   * @param dpp DoutPrefixProvider.
   * @return std::optional<UBNSgRPCClient> A gRPC client object, or
   * std::nullopt on failure.
   */
  std::optional<UBNSgRPCClient> safe_get_client(const DoutPrefixProvider* dpp);

}; // class UBNSClientImpl

} // namespace rgw
