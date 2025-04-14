/**
 * @file rgw_ubns.h
 * @author André Lucas (alucas@akamai.com)
 * @brief Unique Bucket Naming System (UBNS) implementation.
 * @version 0.1
 * @date 2024-03-20
 *
 * @copyright Copyright (c) 2024
 *
 */

/*****************************************************************************
 * Try REALLY REALLY hard to not include "rgw_ubns_impl.h" here.
 *****************************************************************************/

#pragma once

#include <iosfwd>
#include <memory>
#include <string>

#include <fmt/format.h>

#include "common/dout.h"

namespace rgw {

class UBNSClientImpl; // Forward declaration.

/**
 * @brief Return type for gRPC operations on the UBNS client.
 *
 * This is a sanitized return type that doesn't require the inclusion of any
 * gRPC headers. It allows us to pass error codes and messages back to the RGW
 * operations without requiring explosions in compilation time and complexity.
 */
class UBNSClientResult {
private:
  int code_;
  std::string message_;
  bool success_;

private:
  /**
   * @brief Construct a failure-type Result object.
   *
   * RGWOp seems to work with a mix of error code types. Some are are standard
   * UNIX errors, e.g. EEXIST, EPERM, etc. These are defined by including
   * <errno.h>, and are positive integers. Others are RGW-specific errors,
   * e.g. ERR_BUCKET_EXISTS, defined in rgw_common.h.
   *
   * We're integrating with existing code. The safest thing to do is look at
   * existing code, match as close as you can the errors you return, and test
   * carefully.
   *
   * The error message is just for the logs. In the RGWOp classes we don't
   * have direct control over the error responses, we simply set a code.
   *
   * @param code The error code. Should be a positive integer of a type
   * recognised by RGW as explained above.
   * @param message A human-readable error message. Won't be seen by the user
   * but will be logged.
   */
  explicit UBNSClientResult(int code, const std::string& message)
      : code_ { code }
      , message_ { message }
      , success_ { false }
  {
  }

public:
  /**
   * @brief Construct an empty Result object.
   *
   * Use only if you have to, e.g. as a default constructor outside a switch
   * block.
   *
   * For clarity, prefer to use the static success() method to create an
   * object indicating success, or a failure() method to create an object
   * indicating failure.
   *
   */
  UBNSClientResult()
      : code_ { 0 }
      , message_ {}
      , success_ { true }
  {
  }

  // Copies and moves are fine.
  UBNSClientResult(const UBNSClientResult&) = default;
  UBNSClientResult& operator=(const UBNSClientResult&) = default;
  UBNSClientResult(UBNSClientResult&&) = default;
  UBNSClientResult& operator=(UBNSClientResult&&) = default;

  /**
   * @brief Return a success-type result.
   *
   * @return Result A result indicating success.
   */
  static UBNSClientResult success()
  {
    return UBNSClientResult();
  }

  /**
   * @brief Return an error-type result.
   *
   * @param code The error code.
   * @param message The human-readable error message.
   * @return Result A result object constructed with the given fields.
   */
  static UBNSClientResult error(int code, const std::string& message)
  {
    return UBNSClientResult(code, message);
  }

  /**
   * @brief Return true if the operation was successful.
   *
   * @return true The operation was successful.
   * @return false The operation failed.
   */
  bool ok() const { return success_; }

  /**
   * @brief Return true if the operation failed.
   *
   * @return true The operation failed. A message and code will be
   * available for further context.
   * @return false The operation was successful.
   */
  bool err() const { return !ok(); }

  /**
   * @brief Return the error code, or zero if the operation was successful.
   *
   * @return int The error code, or zero if the operation was successful.
   */
  int code() const
  {
    if (ok()) {
      return 0;
    } else {
      return code_;
    }
  }

  /**
   * @brief Return the error message, or an empty string if the operation
   * was successful.
   *
   * @return const std::string& The error message, empty on operation
   * success.
   */
  const std::string& message() const { return message_; }

  /// Return a string representation of this object.
  std::string to_string() const;
  friend std::ostream& operator<<(std::ostream& os, const UBNSClientResult& ep);
}; // class UBNSClientResult

/**
 * @brief Supported states for UpdateBucketEntry gRPC calls.
 *
 * They're here so the callers don't have to include the gRPC headers
 * just to issue UpdateBucketEntry calls to the UBNS client.
 */
enum class UBNSBucketUpdateState {
  UNSPECIFIED,
  CREATED,
  DELETING
}; // enum class UBNSBucketUpdateState

/**
 * @brief Return a string representation of a UBNSBucketUpdateState enum.
 *
 * @param state The state.
 * @return std::string A string representation of the state.
 */
std::string to_str(UBNSBucketUpdateState state);

/**
 * @brief UBNS client class. A shell for UBNSClientImpl.
 *
 * A pointer to an object of this class, created at startup, is passed around
 * inside RGW. This class passes all useful methods through to its internal
 * UBNSClientImpl object, created at construction, that performs all useful
 * work.
 *
 * This allows us to keep the gRPC headers away from most of RGW. It helps
 * compilation time, and hopes to limit future compatibility problems by
 * minimising the codebase's exposure to gRPC, a large and complex codebase
 * with its own dependencies and quirks.
 *
 * This means that almost nothing involving the implementation class
 * (excepting the unique_ptr) can appear in this header - it has to be in the
 * .cc file, where the implementation's header *is* included. In particular,
 * any attempt to call a method on impl_ will result in 'incomplete type'
 * errors. This is intentional; anything else would mean that the
 * implementation would have to be included, and that would defeat the
 * purpose.
 */
class UBNSClient {
private:
  std::unique_ptr<UBNSClientImpl> impl_;

public:
  UBNSClient();
  ~UBNSClient();

  UBNSClient(const UBNSClient&) = delete;
  UBNSClient& operator=(const UBNSClient&) = delete;
  UBNSClient(UBNSClient&&) = delete;
  UBNSClient& operator=(UBNSClient&&) = delete;

  /**
   * @brief Initialise the UBNS client. This will create a gRPC channel, so we
   * need access to configuration.
   *
   * @param cct Ceph Context, used for config and logging.
   * @param grpc_uri The URI of the gRPC server. If empty, will use
   * configuration. (Non-empty is intended for testing.)
   * @return true Success, UBNS may be used.
   * @return false Failure, the probably best action is to fail startup.
   */
  bool init(CephContext* cct, const std::string& grpc_uri = "");

  /**
   * @brief Shut down the UBNS client and free resources.
   */
  void shutdown();

  /**
   * @brief Return type for RPC operations.
   */

  /**
   * @brief Pass to the implementation's add_bucket_entry().
   *
   * @param dpp DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @return UBNSClient::Result the implementation's result object.
   */
  UBNSClientResult add_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner);

  /**
   * @brief Pass to the implementation's delete_bucket_entry().
   *
   * @param dpp DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @return UBNSClient::Result the implementation's result object.
   */
  UBNSClientResult delete_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner);

  /**
   * @brief Pass to the implementation's update_bucket_entry().
   *
   * @param dpp DoutPrefixProvider.
   * @param bucket_name The bucket name.
   * @return UBNSClient::Result the implementation's result object.
   */
  UBNSClientResult update_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner, UBNSBucketUpdateState state);

  /**
   * @brief Return the configured cluster ID.
   *
   * @return const std::string& The cluster ID from configuration.
   * @throws std::runtime_error If the cluster ID is not set (meaning init()
   * has not been called).
   */
  std::string cluster_id() const;
};

/**
 * @brief Check that all relevant UBNS parameters are set in the
 * configuration. Only call this if rgw_ubns_enabled is true.
 *
 * If the configuration is not validated, return false. This is expected to
 * terminate the RGW process.
 *
 * @param conf Config Proxy.
 * @return true The configuration is valid for startup.
 * @return false The configuration is not valid for startup and the daemon
 * should exit.
 */
bool ubns_validate_startup_configuration(ConfigProxy& conf);

} // namespace rgw
