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

#include "acconfig.h"
#include "common/tracer.h"
#include "rgw/rgw_common.h"

#ifdef HAVE_JAEGER
#include "opentelemetry/context/propagation/text_map_propagator.h"
#endif // HAVE_JAEGER

#include <memory>
#include <optional>
#include <shared_mutex>

#include "authorizer/v1/authorizer.pb.h"

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

#ifdef HAVE_JAEGER
/**
 * @brief Support class for injecting OpenTelemetry context into gRPC metadata.
 *
 * Copied with minor edits from the OpenTelemetry C++ SDK at:
 *   https://github.com/open-telemetry/opentelemetry-cpp/blob/main/examples/grpc/tracer_common.h
 */
class HandoffGrpcClientCarrier
    : public opentelemetry::context::propagation::TextMapCarrier {
public:
  HandoffGrpcClientCarrier(grpc::ClientContext *context) : context_(context) {}
  HandoffGrpcClientCarrier() = default;
  virtual opentelemetry::nostd::string_view
  Get(opentelemetry::nostd::string_view /* key */) const noexcept override {
    return "";
  }

  virtual void Set(opentelemetry::nostd::string_view key,
                   opentelemetry::nostd::string_view value) noexcept override {
    context_->AddMetadata(std::string(key), std::string(value));
  }

  grpc::ClientContext *context_;
};

#endif // HAVE_JAEGER

/**
 * @brief Load a tracer Context into the provided gRPC client context.
 *
 * When tracing is enabled, we want to decorate the gRPC client context with
 * information about the enclosing span. The OpenTelemetry library does this
 * via its propagation interface, which is fiddly enough that we want it
 * hidden behind a nice simple function.
 *
 * It is very important that this function not do anything involving the \p
 * optional_yield token passed around inside the RGW frontend. We don't want
 * to change coroutine context here. opentelemetry-cpp doesn't understand
 * coroutines and we don't have a new enough version of opentelemetry-cpp to
 * have the workarounds. This is briefly discussed here:
 *   https://github.com/open-telemetry/opentelemetry-cpp/discussions/2588
 *
 * @param context pointer to a grpc::ClientContext object.
 * @param jspan the current tracing span, typically found in \p
 * req_state->trace.
 */
void populate_trace_context(grpc::ClientContext *context, jspan trace);

/**
 * @brief Return a current trace context if tracing is enabled, otherwise
 * nullopt.
 *
 * std::optional<jspan> is commonly passed to gRPC client wrapper calls, so
 * this makes those calls easy to read.
 *
 * E.g.
 * ```
 *   auto result = client.Auth(req, s->handoff_authz.get(), optional_trace(s));
 * ```
 *
 * is far nicer than:
 * ```
 *   std::optional<jspan> trace;
 *   if (s->trace_enabled) {
 *     trace = s->trace;
 *   }
 *   auto result = client.Auth(req, s->handoff_authz.get(), trace);
 * ```
 *
 * @param s The req_state.
 * @return std::optional<jspan> Contents of \p s->trace if tracing is enabled,
 * otherwise std::nullopt.
 */
std::optional<jspan> optional_trace(const req_state *s);

} // namespace rgw
