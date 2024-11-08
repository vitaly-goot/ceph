/**
 * @file rgw_handoff_grpcutil.h
 * @author André Lucas (alucas@akamai.com)
 * @brief gRPC support utilities.
 * @version 0.1
 * @date 2024-07-26
 *
 * @copyright Copyright (c) 2024
 *
 */

#pragma once

#include <fmt/format.h>
#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>

#include <memory>
#include <optional>
#include <shared_mutex>

#include "authorizer/v1/authorizer.pb.h"
#include "common/ceph_context.h"

namespace rgw {

/**
 * @brief Thinly wrap a gRPC channel and related arguments.
 */
class HandoffGRPCChannel {

  using chan_lock_t = std::shared_mutex;

  mutable std::shared_mutex m_channel_;
  std::shared_ptr<grpc::Channel> channel_;
  std::optional<grpc::ChannelArguments> channel_args_;
  std::string channel_uri_;

  const std::string description_;

public:
  /**
   * @brief Construct a new HandoffGRPCChannel object with the given test
   * description.
   *
   * @param desc A human-readable description of the channel this object
   * encapsulates. Used for logging.
   */
  HandoffGRPCChannel(const std::string& desc)
      : description_(desc)
  {
  }

  /**
   * @brief Return a user-supplied description of this channel. Useful for
   * logging.
   *
   * @return The description of the channel provided at object-creation time.
   */
  const std::string get_description() const
  {
    return description_;
  }

  /**
   * @brief Get a pointer to the channel object.
   *
   * @return std::shared_ptr<grpc::Channel>
   */
  std::shared_ptr<grpc::Channel> get_channel() const;

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
   * @brief Set the gRPC channel URI.
   *
   * This is used by init() and by the config observer. If no channel
   * arguments have been set via set_channel_args(), this will set them to the
   * default values (via get_default_channel_args).
   *
   * Do not call from auth() unless you _know_ you've not taken a lock on
   * m_config_!
   *
   * @param grpc_uri
   * @return true on success.
   * @return false on failure.
   */
  bool set_channel_uri(CephContext* const cct, const std::string& grpc_uri);

  /**
   * @brief Set custom gRPC channel arguments. Intended for testing.
   *
   * You should modify the default channel arguments obtained with
   * get_default_channel_args(). Don't start from scratch.
   *
   * Keep this simple. If you set vtable args you'll need to worry about the
   * lifetime of those is longer than the HandoffHelperImpl object that will
   * store a copy of the ChannelArguments object.
   *
   * Do not call from auth() unless you _know_ you've not taken a lock on
   * m_config_!
   *
   * @param args A populated grpc::ChannelArguments object.
   */
  void set_channel_args(CephContext* const cct, grpc::ChannelArguments& args);

}; // class HandoffGRPCChannel

/**
 * @brief Map from an RGW IAM S3 opcode (e.g. ::rgw::IAM::S3GetObject) to a
 * gRPC opcode (e.g. ::authorizer::v1::S3Opcode::S3_GET_OBJECT).
 *
 * @param iam_s3 The RGW S3 IAM opcode.
 * @return std::optional<::authorizer::v1::S3Opcode> A gRPC S3 opcode if a
 * mapping exists, otherwise std::nullopt.
 */
std::optional<::authorizer::v1::S3Opcode> iam_s3_to_grpc_opcode(uint64_t iam_s3);

/**
 * @brief Map from a gRPC Authorizer S3 opcode (e.g.
 * ::authorizer::v1::S3_GET_OBJECT) to an RGW IAM S3 opcode (e.g.
 * ::rgw::IAM::S3GetObject).
 *
 * @param grpc_opcode The gRPC S3 opcode.
 * @return std::optional<uint64_t> An RGW S3 IAM opcode if a mapping exists,
 * otherwise std::nullopt.
 */
std::optional<uint64_t> grpc_opcode_to_iam_s3(::authorizer::v1::S3Opcode grpc_opcode);

} // namespace rgw
