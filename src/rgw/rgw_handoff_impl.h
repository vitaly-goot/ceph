/**
 * @file rgw_handoff_impl.h
 * @author André Lucas (alucas@akamai.com)
 * @brief Handoff declarations involving gRPC.
 * @version 0.1
 * @date 2023-11-10
 *
 * @copyright Copyright (c) 2023
 *
 * Declarations for HandoffHelperImpl and related classes.
 *
 * TRY REALLY HARD to not include this anywhere except handoff.cc and
 * handoff_impl.cc. In particular, don't add it to rgw_handoff.h no matter how
 * tempting that seems.
 *
 * This file pulls in the gRPC headers and we don't want that everywhere.
 */

#ifndef RGW_HANDOFF_IMPL_H
#define RGW_HANDOFF_IMPL_H

#include <iosfwd>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>
#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>

#include "common/async/yield_context.h"
#include "common/config_obs.h"
#include "common/dout.h"
#include "common/tracer.h"
#include "rgw/rgw_common.h"

#include "authenticator/v1/authenticator.grpc.pb.h"
#include "authenticator/v1/authenticator.pb.h"
using namespace ::authenticator::v1;

#include "authorizer/v1/authorizer.grpc.pb.h"
#include "authorizer/v1/authorizer.pb.h"
using namespace ::authorizer::v1;

#include "rgw_handoff.h"
#include "rgw_handoff_grpcutil.h"

namespace rgw {

/****************************************************************************/

/**
 * @brief Implement DoutPrefixPipe for a simple prefix string.
 *
 * To add an additional string (which will be followed by ": ") to the
 * existing log prefix, use:
 *
 * ```
 * HandoffDoutPrefixPipe hdpp(*dpp_in, foo);
 * auto dpp = &hdpp;
 * ```
 *
 * It will save time to create a DoutPrefixProvider*, as demonstrated by the
 * unit tests it's quite jarring to have to use &dpp.
 */
class HandoffDoutPrefixPipe : public DoutPrefixPipe {
  const std::string prefix_;

public:
  HandoffDoutPrefixPipe(const DoutPrefixProvider& dpp, const std::string& prefix)
      : DoutPrefixPipe { dpp }
      , prefix_ { fmt::format(FMT_STRING("{}: "), prefix) }
  {
  }
  virtual void add_prefix(std::ostream& out) const override final
  {
    out << prefix_;
  }
};

/**
 * @brief Add request state as a prefix to the log message. This should be
 * used to help support engineers correlate log messages.
 *
 * Pass in the request state.
 *
 * ```
 * HandoffDoutStateProvider hdpp(*dpp_in, s);
 * auto dpp = &hdpp;
 * ```
 */
class HandoffDoutStateProvider : public HandoffDoutPrefixPipe {

public:
  /**
   * @brief Construct a new Log provider object with an existing provider and
   * the request state.
   *
   * Use our HandoffDoutPrefixPipe implementation for implementation. Add a
   * standard prefix 'HandoffEngine'.
   *
   * @param dpp An existing DoutPrefixProvider reference.
   * @param s The request state.
   */
  HandoffDoutStateProvider(const DoutPrefixProvider& dpp, const req_state* s)
      : HandoffDoutPrefixPipe {
        dpp, fmt::format(FMT_STRING("HandoffEngine trans_id={}"), s->trans_id)
      } {};

  /**
   * @brief Construct a new Log provider object with an existing provider, a
   * string prefix, and the request state.
   *
   * Use our HandoffDoutPrefixPipe implementation for implementation.
   *
   * @param dpp An existing DoutPrefixProvider reference.
   * @param prefix A string prefix for the log message.
   * @param s The request state.
   */
  HandoffDoutStateProvider(const DoutPrefixProvider& dpp, const std::string prefix, const req_state* s)
      : HandoffDoutPrefixPipe {
        dpp, fmt::format(FMT_STRING("{} trans_id={}"), prefix, s->trans_id)
      } {};
};

/****************************************************************************/

/**
 * @brief gRPC client wrapper for authenticator/v1/AuthenticatorService.
 *
 * Very thin wrapper around the gRPC client. Construct with a channel to
 * create a stub. Call services via the corresponding methods, with sanitised
 * return values.
 */
class AuthServiceClient {
private:
  std::unique_ptr<AuthenticatorService::Stub> stub_;

public:
  /**
   * @brief Return value from GetSigningKey().
   */
  class GetSigningKeyResult {
    std::vector<uint8_t> signing_key_;
    bool success_;
    std::string error_message_;

  public:
    /**
     * @brief Construct a success-type result.
     *
     * @param key The signing key.
     */
    GetSigningKeyResult(std::vector<uint8_t> key)
        : signing_key_ { key }
        , success_ { true }
    {
    }

    /**
     * @brief Construct a failure-type result.
     *
     * ok() will return false.
     *
     * @param msg A human-readable error message.
     */
    GetSigningKeyResult(std::string msg)
        : success_ { false }
        , error_message_ { msg }
    {
    }

    // Can't have a copy constructor with a unique_ptr.
    GetSigningKeyResult(const GetSigningKeyResult&) = delete;
    GetSigningKeyResult& operator=(const GetSigningKeyResult&) = delete;
    // Move is fine.
    GetSigningKeyResult(GetSigningKeyResult&&) = default;
    GetSigningKeyResult& operator=(GetSigningKeyResult&&) = default;

    /// @brief Return true if a signing key is present, false otherwise.
    bool ok() const noexcept { return success_; }
    /// Return true if this is a failure-type object, false otherwise.
    bool err() const noexcept { return !ok(); }

    /**
     * @brief Return the signing key if present, otherwise throw std::runtime_error.
     *
     * @return std::vector<uint8_t> The signing key.
     * @throws std::runtime_error if this is a failure-type object.
     */
    std::vector<uint8_t> signing_key() const
    {
      if (!ok()) {
        throw std::runtime_error("signing_key() called in error");
      }
      return signing_key_;
    }
    /**
     * @brief Return an error message if present, otherwise an empty string.
     *
     * @return std::string The error message. May be empty.
     */
    std::string error_message() const noexcept { return error_message_; }
  }; // class AuthServiceClient::GetSigningKeyResult

  /**
   * @brief Construct a new Auth Service Client object. You must use set_stub
   * before using any gRPC calls, or the object's behaviour is undefined.
   */
  AuthServiceClient() { }

  /**
   * @brief Construct a new AuthServiceClient object and initialise the gRPC
   * stub.
   *
   * @param channel pointer to the grpc::Channel object to be used.
   */
  explicit AuthServiceClient(std::shared_ptr<::grpc::Channel> channel)
      : stub_(AuthenticatorService::NewStub(channel))
  {
  }

  // Copy constructors can't work with the stub unique_ptr.
  AuthServiceClient(const AuthServiceClient&) = delete;
  AuthServiceClient& operator=(const AuthServiceClient&) = delete;
  // Moves are fine.
  AuthServiceClient(AuthServiceClient&&) = default;
  AuthServiceClient& operator=(AuthServiceClient&&) = default;

  /**
   * @brief Set the gRPC stub for this object.
   *
   * @param channel the gRPC channel pointer.
   */
  void set_stub(std::shared_ptr<::grpc::Channel> channel)
  {
    stub_ = AuthenticatorService::NewStub(channel);
  }

  /**
   * @brief Call rgw::auth::v1::AuthService::Auth() and return a
   * HandoffAuthResult, suitable for HandoffHelperImpl::auth().
   *
   * On success, return the embedded username. We don't currently support the
   * original_user_id field of the response message, as we have no current use
   * for it.
   *
   * If authz_state is non-null, a successful authentication will populate
   * authorization-related fields using
   * authz_state->set_authenticator_id_fields(). This method can punch through
   * the fact that authz_state is a const pointer. This is not ideal, but is
   * necessary because the internal API calls authenticate() with a const
   * req_state*, and that's where our authz_state is stored.
   *
   * On error, parse the result for an S3ErrorDetails message embedded in the
   * details field. (Richer error model.) If we find one, return the error
   * message and embed the contained HTTP status code. It's up to the caller
   * to follow up and pass the HTTP status code back to RGW in the proper
   * form, it won't do to return the raw HTTP status code! We're assuming the
   * Authenticator knows what HTTP code it wants returned, and it's up to
   * Handoff to interpret it properly and return a useful code.
   *
   * If we don't find an S3ErrorDetails message, return a generic error (with
   * the provided error message) with error type TRANSPORT_ERROR. This allows
   * the caller to differentiate between authentication problems and RPC
   * problems.
   *
   * The alternative to using HandoffAuthResult would be interpreting the
   * request status and result object would be either returning a bunch of
   * connection status plus an AuthResponse object, or throwing exceptions on
   * error. This feels like an easier API to use in our specific use case.
   *
   * @param req the filled-in authentication request protobuf
   * (rgw::auth::v1::AuthRequest).
   * @param authz_state The authorization state, if available. On success,
   * this _will_ be modified, even though it's a const pointer (see method
   * docs).
   * @param span An optional trace span.
   * @return HandoffAuthResult A completed auth result.
   */
  HandoffAuthResult Auth(const AuthenticateRESTRequest &req,
                         const HandoffAuthzState *authz_state = nullptr,
                         std::optional<jspan> span = std::nullopt);

  /**
   * @brief Map an Authenticator gRPC error code onto an error code that RGW
   * can digest.
   *
   * The Authenticator returns a details error code in S3ErrorDetails.Type. We
   * need to map this onto the list of error codes in rgw_common.cc, array
   * rgw_http_s3_errors. There may be exceptions if the inbuild RGW codes that
   * match the Authenticator codes don't return the proper HTTP status code.
   * This will probably need tweaked over time.
   *
   * If there's no direct mapping, we'll try to map a subset of HTTP error
   * codes onto a matching RGW error code. If we can't do that, we'll return
   * EACCES which results in an HTTP 403.
   *
   * @param auth_type The Authenticator error code (S3ErrorDetails.Type).
   * @param auth_http_status_code The desired HTTP status code.
   * @param message The Authenticator's error message (copied verbatim into
   * the HandoffAuthResult).
   * @return HandoffAuthResult
   */
  static HandoffAuthResult _translate_authenticator_error_code(
      ::authenticator::v1::S3ErrorDetails_Type auth_type,
      int32_t auth_http_status_code,
      const std::string& message);

  /**
   * @brief Request a signing key for the given authorization header. The
   * signing key is valid on the day it is issued, as it has a date component
   * in the HMAC.
   *
   * This is intended for use with chunked uploads, but may be useful for
   * caching purposes as the singing key allows us to authenticate locally.
   * However, it would also reduce the granularity of credential changes so I
   * don't recommend it be used with a time window of a day. Maybe 15 minutes
   * is acceptable, though honestly if we were to use this in anger I would
   * recommend an API to imperatively expire credentials.
   *
   * @param req A properly filled authenticator.v1.GetSigningKeyRequest
   * protobuf message.
   * @param span An optional trace span.
   * @return A GetSigningKeyResult object.
   */
  GetSigningKeyResult GetSigningKey(const GetSigningKeyRequest req,
                                    std::optional<jspan> span = std::nullopt);
};

/****************************************************************************/

/**
 * @brief Gathered information about an inflight request that we want to sent
 * to the Authentication service for verification.
 *
 * Normally these data are gathered later in the request and subject to
 * internal policies, acls etc. We're giving the Authentication service a
 * chance to see this information early.
 */
class AuthorizationParameters {

private:
  bool valid_;
  std::string method_;
  std::string bucket_name_;
  std::string object_key_name_;
  std::unordered_map<std::string, std::string> http_headers_;
  std::string http_request_path_;
  std::unordered_map<std::string, std::string> http_query_params_;

  void valid_check() const
  {
    if (!valid()) {
      throw new std::runtime_error("AuthorizationParameters not valid");
    }
  }

public:
  AuthorizationParameters(const DoutPrefixProvider* dpp, const req_state* s) noexcept;

  // Standard copies and moves are fine.
  AuthorizationParameters(const AuthorizationParameters& other) = default;
  AuthorizationParameters& operator=(const AuthorizationParameters& other) = default;
  AuthorizationParameters(AuthorizationParameters&& other) = default;
  AuthorizationParameters& operator=(AuthorizationParameters&& other) = default;

  /**
   * @brief Return the validity of this AuthorizationParameters object.
   *
   * If at construction time the request was well-formed and contained
   * sufficient information to be used in an authorization request to the
   * Authenticator, return true.
   *
   * Otherwise, return false.
   *
   * @return true The request can be used as the source of an
   * authorization-enhanced authentication operation.
   * @return false The request cannot be used.
   */
  bool valid() const noexcept
  {
    return valid_;
  }
  /**
   * @brief Return the HTTP method for a valid request. Throw if valid() is
   * false.
   *
   * @return std::string The method.
   * @throw std::runtime_error if !valid().
   */
  std::string method() const
  {
    valid_check();
    return method_;
  }
  /**
   * @brief Return the bucket name for a valid request. Throw if valid() is
   * false.
   *
   * @return std::string The bucket name.
   * @throw std::runtime_error if !valid().
   */
  std::string bucket_name() const
  {
    valid_check();
    return bucket_name_;
  }
  /**
   * @brief Return the object key name for a valid request. Throw if valid()
   * is false.
   *
   * @return std::string The object key name.
   * @throw std::runtime_error if !valid().
   */
  std::string object_key_name() const
  {
    valid_check();
    return object_key_name_;
  }

  /**
   * @brief Convert this AuthorizationParameters object to string form.
   *
   * Note we don't dump the object key name - this might be a large string,
   * might be full of invalid characters, or might be private.
   *
   * @return std::string A string representation of the object. Works fine for
   * objects in the invalid state; this call is always safe.
   */
  std::string to_string() const noexcept;

  /**
   * @brief Return a const reference to the map of HTTP headers.
   *
   * @return const std::unordered_map<std::string, std::string>& A read-only
   * reference to the HTTP headers map.
   */
  const std::unordered_map<std::string, std::string>& http_headers() const
  {
    valid_check();
    return http_headers_;
  }

  /**
   * @brief Return the http request path (req_info.request_uri).
   *
   * @return std::string the request path.
   */
  std::string http_request_path() const
  {
    valid_check();
    return http_request_path_;
  }

  /**
   * @brief Return a const reference to the map of HTTP query parameters.
   *
   * @return const std::unordered_map<std::string, std::string>& A read-only
   * reference to the HTTP query parameters map.
   */
  const std::unordered_map<std::string, std::string>& http_query_params() const
  {
    valid_check();
    return http_query_params_;
  }

  friend std::ostream& operator<<(std::ostream& os, const AuthorizationParameters& ep);

}; // class AuthorizationParameters.

std::ostream& operator<<(std::ostream& os, const AuthorizationParameters& ep);

/****************************************************************************/

enum class AuthParamMode {
  NEVER,
  WITHTOKEN,
  ALWAYS
};

/**
 * @brief Config Observer utility class for HandoffHelperImpl.
 *
 * This is constructed so as to make it feasible to mock the ConfigObserver
 * interface. We can construct an instance of this class with a mocked helper,
 * just so long as that mocked helper implements the proper signature. There's
 * no way to formally specify the required signatures in advance in C++,
 * compile-time polymorphism means running the SFINAE gauntlet.
 *
 * As of 20231127, T must implement (with the same signature as
 * HandoffHelperImpl):
 *   - get_authn_channel()  (returned type needs to implement
 *       HandoffGRPCChannel's interface, doesn't need to return exactly
 *       HandoffGRPCChannel&))
 *   - set_signature_v2()
 *   - set_authorization_mode()
 *   - set_chunked_upload_mode()
 *
 * or it won't compile.
 *
 * @tparam T The HandoffHelperImpl-like class. The class has to implement the
 * same configuration mutation methods as HandoffHelperImpl.
 */
template <typename T>
class HandoffConfigObserver final : public md_config_obs_t {
public:
  /**
   * @brief Construct a new Handoff Config Observer object with a
   * backreference to the owning HandoffHelperImpl.
   *
   * @param helper
   */
  explicit HandoffConfigObserver(T& helper)
      : helper_(helper)
  {
  }

  // Don't allow a default construction without the helper_.
  HandoffConfigObserver() = delete;

  /**
   * @brief Destructor. Remove the observer from the Ceph configuration system.
   *
   */
  ~HandoffConfigObserver()
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

  /**
   * @brief Read config and return the resultant AuthParamMode in effect.
   *
   * @param conf ConfigProxy reference.
   * @return AuthParamMode the mode in effect based on system config.
   */
  AuthParamMode get_authorization_mode(const ConfigProxy& conf) const
  {
    if (conf->rgw_handoff_authparam_always) {
      return AuthParamMode::ALWAYS;
    } else if (conf->rgw_handoff_authparam_withtoken) {
      return AuthParamMode::WITHTOKEN;
    } else {
      return AuthParamMode::NEVER;
    }
  }

  // Config observer. See notes in src/common/config_obs.h and for
  // ceph::md_config_obs_impl.

  const char** get_tracked_conf_keys() const
  {
    // Note that these are keys that support runtime alteration. Keys that are
    // set at startup time only do not need to appear here.
    static const char* keys[] = {
      "rgw_handoff_authparam_always",
      "rgw_handoff_authparam_withtoken",
      "rgw_handoff_authz_grpc_uri",
      "rgw_handoff_enable_anonymous_authorization",
      "rgw_handoff_enable_chunked_upload",
      "rgw_handoff_enable_signature_v2",
      "rgw_handoff_grpc_arg_initial_reconnect_backoff_ms",
      "rgw_handoff_grpc_arg_max_reconnect_backoff_ms",
      "rgw_handoff_grpc_arg_min_reconnect_backoff_ms",
      "rgw_handoff_grpc_uri",
      nullptr
    };
    return keys;
  }

  void handle_conf_change(const ConfigProxy& conf,
      const std::set<std::string>& changed)
  {
    // You should bundle any gRPC arguments changes into this first block.
    if (changed.count("rgw_handoff_grpc_arg_initial_reconnect_backoff_ms") || changed.count("rgw_handoff_grpc_arg_max_reconnect_backoff_ms") || changed.count("rgw_handoff_grpc_arg_min_reconnect_backoff_ms")) {
      auto args = helper_.get_authn_channel().get_default_channel_args(cct_);
      helper_.get_authn_channel().set_channel_args(cct_, args);
      helper_.get_authz_channel().set_channel_args(cct_, args);
    }
    // The gRPC channel change needs to come after the arguments setting, if any.
    if (changed.count("rgw_handoff_grpc_uri")) {
      helper_.get_authn_channel().set_channel_uri(cct_, conf->rgw_handoff_grpc_uri);
    }
    if (changed.count("rgw_handoff_authz_grpc_uri")) {
      helper_.get_authz_channel().set_channel_uri(cct_, conf->rgw_handoff_authz_grpc_uri);
    }
    if (changed.count("rgw_handoff_enable_anonymous_authorization")) {
      helper_.set_anonymous_authorization(cct_, conf->rgw_handoff_enable_anonymous_authorization);
    }
    if (changed.count("rgw_handoff_enable_chunked_upload")) {
      helper_.set_chunked_upload_mode(cct_, conf->rgw_handoff_enable_chunked_upload);
    }
    if (changed.count("rgw_handoff_enable_signature_v2")) {
      helper_.set_signature_v2(cct_, conf->rgw_handoff_enable_signature_v2);
    }
    if (changed.count("rgw_handoff_authparam_always") || changed.count("rgw_handoff_authparam_withtoken")) {
      helper_.set_authorization_mode(cct_, get_authorization_mode(conf));
    }
  }

private:
  T& helper_;
  CephContext* cct_ = nullptr;
  bool observer_added_ = false;
};

/****************************************************************************/

/**
 * @brief Wrapper for gRPC calls to the Authorizer service.
 *
 * Attempt to make calls to the Authorizer service easier to test, by
 * encapsulating the actual calls and error handling.
 */
class AuthorizerClient {
private:
  std::unique_ptr<AuthorizerService::Stub> stub_;

public:
  /**
   * @brief Construct a new Authorizer Client object. You must use set_stub
   * before using any gRPC calls, or the object's behaviour is undefined.
   */
  AuthorizerClient() { }

  /**
   * @brief Construct a new Authorizer Client object and initialise the gRPC
   * stub.
   *
   * @param channel pointer to the grpc::Channel object to be used.
   */
  explicit AuthorizerClient(std::shared_ptr<::grpc::Channel> channel)
      : stub_(AuthorizerService::NewStub(channel))
  {
  }

  // Copy constructors can't work with the stub unique_ptr.
  AuthorizerClient(const AuthorizerClient&) = delete;
  AuthorizerClient& operator=(const AuthorizerClient&) = delete;

  // Moves are fine.
  AuthorizerClient(AuthorizerClient&&) = default;
  AuthorizerClient& operator=(AuthorizerClient&&) = default;

  /**
   * @brief Set the gRPC stub for this object.
   *
   * @param channel the gRPC channel pointer.
   */
  void set_stub(std::shared_ptr<::grpc::Channel> channel)
  {
    stub_ = AuthorizerService::NewStub(channel);
  }

  /**
   * @brief Call the Authorizer Ping() endpoint.
   *
   * @param id The authorization_id field to be echoed back.
   * @return true The call succeeded and the ID was echoed back properly.
   * @return false The call failed for some reason.
   */
  bool Ping(const std::string& id);

  /**
   * @brief Collection of results from the Authorizer Authorize() RPC endpoint.
   *
   * There are a a lot of potential results from an Authorize() call. In the
   * successful RPC case we want to inspect the response protobuf message. In
   * failure cases, we want at the grpc::Status.
   *
   * Rather than use a tuple response, bit the bullet and encapsulate the lot.
   * That way we can standardise display code and error handling.
   */
  class AuthorizeResult {
    bool success_;
    bool has_request_ = false;
    AuthorizeV2Request request_;
    bool has_response_ = false;
    AuthorizeV2Response response_;
    bool has_status_ = false;
    ::grpc::Status status_;
    bool has_message_ = false;
    std::string message_;

  public:
    /**
     * @brief Construct a new Authorize Result object, saving the success
     * (taking into account the answers in the response) and moving the
     * response itself.
     *
     * @param success Whether or not the response indicates that all questions
     * received an ALLOW response.
     * @param response The AuthorizeV2Response message. This will be moved;
     * the original response value will become useless.
     */
    AuthorizeResult(bool success, AuthorizeV2Response& response)
        : success_ { success }
        , has_response_ { true }
        , response_ { std::move(response) }
    {
    }

    /**
     * @brief Construct a new Authorize Result object, saving the success
     * (taking into account the answers in the response) and moving the
     * request and response into this object.
     *
     * @param success Whether or not the response indicates that all questions
     * received an ALLOW response.
     * @param request The AuthorizeV2Request message. This will be moved; the
     * original request value will become useless.
     * @param response The AuthorizeV2Response message. This will be moved;
     * the original response value will become useless.
     */
    AuthorizeResult(bool success, AuthorizeV2Request& request, AuthorizeV2Response& response)
        : success_ { success }
        , has_request_ { true }
        , request_ { std::move(request) }
        , has_response_ { true }
        , response_ { std::move(response) }
    {
    }

    /**
     * @brief Construct a new failure-type Authorize Result object, moving the
     * request and response objects into this object, and saving the provided
     * error message.
     *
     * @param request Will be moved into this object.
     * @param response Will be moved into this object.
     * @param message The error message.
     */
    AuthorizeResult(AuthorizeV2Request& request, AuthorizeV2Response& response, const std::string& message)
        : success_(false)
        , has_request_(true)
        , request_(std::move(request))
        , has_response_(true)
        , response_(std::move(response))
        , has_message_(true)
        , message_(message)
    {
    }

    /**
     * @brief Construct a failure-type Authorize Result object, saving the
     * gRPC status code.
     *
     * @param status the ::grpc::Status response from the RPC call.
     */
    explicit AuthorizeResult(const ::grpc::Status& status)
        : success_(false)
        , has_status_(true)
        , status_(status)
    {
    }

    /**
     * @brief Construct a failure-type Authorize Result object, saving the
     * error message given by the caller.
     *
     * @param message The error message.
     */
    explicit AuthorizeResult(const std::string& message)
        : success_(false)
        , has_message_(true)
        , message_(message)
    {
    }

    AuthorizeResult(const AuthorizeResult&) = delete;
    AuthorizeResult& operator=(const AuthorizeResult&) = delete;
    AuthorizeResult(AuthorizeResult&&) = default;
    AuthorizeResult& operator=(AuthorizeResult&&) = default;

    /**
     * @brief Return true iff the call has been made and the call succeeded
     * with a success (ALLOW) result.
     *
     * @return true The call has been made and the call returned ALLOW status.
     * @return false The call has not been made, or the call failed, or the
     * call did not return ALLOW status.
     */
    bool ok() const noexcept { return success_; }
    /**
     * @brief Return true if the call has not yet been made, or if the call
     * did not succeed with a success (ALLOW) result.
     *
     * @return true The call has not been made, or the call failed, or the
     * call did not return ALLOW status.
     * @return false The call has been made and the call returned ALLOW status.
     */
    bool err() const noexcept { return !ok(); }

    /**
     * @brief Utility function to determine if the call failed due to an extra
     * data requirement.
     *
     * If there's no saved response, or the gRPC call returned failure, this
     * will simply return false. It will only return true if one or more of
     * the answers in a response contained AUTHZ_STATUS_EXTRA_DATA_REQUIRED.
     *
     * @return true if any answer in the response required extra data.
     */
    bool is_extra_data_required() const;

    /// @brief Return true if the call has a request.
    bool has_request() const noexcept { return has_request_; }
    /// @brief Return true if the call has a response.
    bool has_response() const noexcept { return has_response_; }
    /// @brief Return true if the call has a status.
    bool has_status() const noexcept { return has_status_; }
    /// @brief Return true if the call has an error message.
    bool has_message() const noexcept { return has_message_; }

    /// @brief Return a reference to the AuthorizeRequest message that was
    /// sent to the server, if any.
    const AuthorizeV2Request& request() const noexcept { return request_; }
    /// @brief Return a reference to the AuthorizeResponse message resulting
    /// from the RPC call, if any.
    const AuthorizeV2Response& response() const noexcept { return response_; }
    /// @brief Return the gRPC status object from the RPC call, if any. Having
    /// a status object means the call did not succeed.
    const ::grpc::Status& status() const noexcept { return status_; }
    /// @brief Return the error message, if any. Having an error message
    /// means the call did not succeed.
    std::string message() const noexcept { return message_; }

    friend std::ostream& operator<<(std::ostream& os, const AuthorizerClient::AuthorizeResult& ep);
  }; // class AuthorizerClient::AuthorizeResult

  /**
   * @brief Call the Authorizer Authorize() endpoint.
   *
   * Note that, if successful, this will *move* the request protobuf into the
   * result!
   *
   * If the common.timestamp is set to 0, the client will fill in the current
   * time as this is almost always the correct thing to do.
   *
   * @param req The AuthorizeRequest message.
   * @param span An optional trace span.
   * @return AuthorizeResponse the AuthorizeResponse message from the server.
   */
  AuthorizerClient::AuthorizeResult
  AuthorizeV2(AuthorizeV2Request &req,
              std::optional<jspan> span = std::nullopt);

}; // class AuthorizerClient

/****************************************************************************/

/**
 * @brief Support class for 'handoff' authentication.
 *
 * Used by rgw::auth::s3::HandoffEngine to implement authentication via an
 * external Authenticator Service.
 *
 * In gRPC mode, holds long-lived state.
 */
class HandoffHelperImpl final {

public:
  using chan_lock_t = std::shared_mutex;

private:
  // Ceph configuration observer.
  HandoffConfigObserver<HandoffHelperImpl> config_obs_;

  // The store should be constant throughout the lifetime of the helper.
  rgw::sal::Driver* store_;

  // These are used in place of constantly querying the ConfigProxy.
  mutable std::shared_mutex m_config_;
  bool grpc_mode_ = true; // Not runtime-alterable.
  bool presigned_expiry_check_ = false; // Not runtime-alterable.
  bool disable_local_authorization_ = false; // Not runtime-alterable.
  bool reject_filtered_commands_ = true; // Not runtime-alterable.
  bool allow_native_copy_object_ = true; // Not runtime-alterable.

  bool enable_anonymous_authorization_ = true; // Runtime-alterable.
  bool enable_signature_v2_ = true; // Runtime-alterable.
  bool enable_chunked_upload_ = true; // Runtime-alterable.
  AuthParamMode authorization_mode_ = AuthParamMode::ALWAYS; // Runtime-alterable.

  // The gRPC channel for authentication.
  HandoffGRPCChannel authn_channel_ { "handoff-authn" };
  // The gRPC channel for authorization.
  HandoffGRPCChannel authz_channel_ { "handoff-authz" };

public:
  /**
   * @brief Construct a new HandoffHelperImpl object.
   *
   * This is the constructor to use for all except unit tests. Note no
   * persisted state is set up; that's done by calling init().
   */
  HandoffHelperImpl()
      : config_obs_(*this)
  {
  }

  ~HandoffHelperImpl() = default;

  /**
   * @brief Initialise any long-lived state for this engine.
   * @param cct Pointer to the Ceph context.
   * @param store Pointer to the sal::Store object.
   * @param grpc_uri Optional URI for the gRPC server. If empty (the default),
   * config value rgw_handoff_grpc_uri is used.
   * @param grpc_authz_uri Optional URI for the gRPC authorization server. If
   * empty (the default), config value rgw_handoff_authz_grpc_uri is used.
   * @return 0 on success, otherwise failure.
   *
   * Store long-lived state.
   *
   * The \p store pointer isn't used at this time.
   *
   * In gRPC mode, a grpc::Channel is created and stored on the object for
   * later use. This will manage the persistent connection(s) for all gRPC
   * communications.
   */
  int init(CephContext* const cct, rgw::sal::Driver* store, const std::string& grpc_uri = "", const std::string& authz_grpc_uri = "");

  /**
   * @brief Return a reference to the the authentication channel wrapper object.
   *
   * This object always exists. Note that the underlying gRPC channel may not
   * be set (non-nullptr).
   *
   * @return HandoffGRPCChannel& a reference to this HandoffHelperImpl's
   * authentication channel wrapper.
   */
  HandoffGRPCChannel& get_authn_channel() { return authn_channel_; }

  /**
   * @brief Return a reference to the the authorization channel wrapper
   * object.
   *
   * This object always exists. Note that the underlying gRPC channel may not
   * be set (non-nullptr).
   *
   * @return HandoffGRPCChannel& a reference to this HandoffHelperImpl's
   * authorization channel wrapper.
   */
  HandoffGRPCChannel& get_authz_channel() { return authz_channel_; }

  /**
   * @brief Configure support for AWS signature v2.
   *
   * I strongly recommend this remain enabled for broad client support.
   *
   * Do not call from auth() unless you _know_ you've not taken a lock on
   * m_config_!
   *
   * @param enabled Whether or not V2 signatures should be allowed.
   */
  void set_signature_v2(CephContext* const cct, bool enabled);

  /**
   * @brief Set the authorization mode for subsequent requests.
   *
   * Do not call from auth() unless you _know_ you've not taken a lock on
   * m_config_!
   *
   * @param cct CephContext pointer.
   * @param mode The authorization mode.
   */
  void set_authorization_mode(CephContext* const cct, AuthParamMode mode);

  /**
   * @brief Configure chunked upload mode.
   *
   * This should probably remain enabled, but the toggle exists just in case
   * we have performance problems with the additional gRPC calls this
   * requires.
   *
   * @param cct CephContext pointer.
   * @param enabled Whether or not chunked uploads should be allowed.
   */
  void set_chunked_upload_mode(CephContext* const cct, bool enabled);

  /**
   * @brief Set the anonymous authorization mode.
   *
   * @param cct CephContext pointer.
   * @param enabled Whether or not anonymous authorization should be performed.
   */
  void set_anonymous_authorization(CephContext* const cct, bool enabled);

  /**
   * @brief Return true if anonymous authorization is enabled, false
   * otherwise.
   *
   * This needs exposed to the HandoffHelper, so regular (non-gRPC) code can
   * decide whether or not anonymous authz is enabled. We don't want to query
   * the config proxy every time.
   *
   * @return true Anonymous authorization is enabled.
   * @return false Anonymous authorization is disabled.
   */
  bool anonymous_authorization_enabled() const;

  /**
   * @brief Return true if Handoff is configured to disable *all* local
   * authorization checks in favour of external authorization.
   *
   * This is intended for use in subordinate functions of process_request() -
   * mostly rgw_process_authenticated() - to determine whether or not we
   * should attempt local authorization.
   *
   * @return true Local authorization is disabled.
   * @return false Local authorization is enabled and should not be bypassed.
   */
  bool disable_local_authorization() const
  {
    // This is a simple bool set at init() time, there's no need for locking
    // etc.
    return disable_local_authorization_;
  }

  /**
   * @brief Return true if Handoff Authz is configured to reject filtered
   * commands.
   *
   * This is intended for use in operation verify_permission() overrides,
   * where we can decide whether or not a command that we don't expect to see
   * in the presence of the microservices environment should be rejected with
   * a failure code, rather than attempting to authorize it.
   *
   * The aim here is to assist testing, by allowing tests to operate in the
   * absence of the microservices environment.
   *
   * @return true Handoff Authz is configured to reject filtered commands.
   * @return false Handoff Authz should attempt to authorize commands that
   * would normally be filtered.
   */
  bool reject_filtered_commands() const
  {
    // This is a simple bool set at init() time, there's no need for locking
    // etc.
    return reject_filtered_commands_;
  }

  /**
   * @brief Return true if Handoff is configured to allow native copy-object.
   *
   * @return true copy-object should be processed normally.
   * @return false copy-object should be rejected with INVALID REQUEST.
   */
  bool allow_native_copy_object() const
  {
    return allow_native_copy_object_;
  }

  /**
   * @brief Authenticate the transaction using the Handoff engine.
   * @param dpp Debug prefix provider. Points to the Ceph context.
   * @param session_token Unused by Handoff.
   * @param access_key_id The S3 access key.
   * @param string_to_sign The canonicalised S3 signature input.
   * @param signature The transaction signature provided by the user.
   * @param s Pointer to the req_state.
   * @param y An optional yield token.
   * @return A HandofAuthResult encapsulating a return error code and any
   * parameters necessary to continue processing the request, e.g. the uid
   * associated with the access key.
   *
   * Perform request authentication via the external authenticator.
   *
   * auth() runs with shared lock of m_config_, so runtime-alterable
   * configuration isn't undefined during a single authentication.
   * Modifications to the affected runtime parameters are performed under a
   * unique lock of m_config_.
   *
   * - Extract the Authorization header from the environment. This will be
   *   necessary to validate a v4 signature because we need some fields (date,
   *   region, service, request type) for step 2 of the signature process.
   *
   * - If the Authorization header is absent, attempt to extract the relevant
   *   information from query parameters to synthesize an Authorization
   *   header. This is to support presigned URLs.
   *
   * - If the header indicates AWS Signature V2 authentication, but V2 is
   *   disabled via configuration, return a failure immediately.
   *
   * - If required, introspect the request to obtain additional authentication
   *   parameters that might be required by the external authenticator.
   *
   * - Depending on configuration, call either the gRPC arm (_grpc_auth()) or
   *   the HTTP arm (_http_auth()) and return the result.
   *
   */
  HandoffAuthResult
  auth(const DoutPrefixProvider* dpp,
      const std::string_view& session_token,
      const std::string_view& access_key_id,
      const std::string_view& string_to_sign,
      const std::string_view& signature,
      const req_state* const s,
      optional_yield y);

  /**
   * @brief Implement the gRPC arm of auth().
   *
   *
   * Implement a Handoff authentication request using gRPC.
   *
   * - Fill in the provided information in the request protobuf
   *   (rgw::auth::v1::AuthRequest).
   *
   * - If authorization parameters are provided, fill those in in the protobuf
   *   as well.
   *
   * - Send the request using an instance of rgw::AuthServiceClient. note that
   *   AuthServiceClient::Auth() handles the translation of the response code
   *   into a code suitable to be returned to RGW as the result of the engine
   *   authenticate() call.
   *
   * - If the gRPC request itself failed, log the error and return 'access
   *   denied'.
   *
   * - Log the authentication request's success or failure, and return the
   *   result from AuthServiceClient::Auth().
   *
   * @param dpp DoutPrefixProvider.
   * @param auth The authorization header, which may have been synthesized.
   * @param authorization_param Authorization parameters, if required.
   * @param session_token Unused by Handoff.
   * @param access_key_id The S3 access key.
   * @param string_to_sign The canonicalised S3 signature input.
   * @param signature The transaction signature provided by the user.
   * @param s Pointer to the req_state.
   * @param y An optional yield token.
   * @param is_presigned_request True if this authentication call has been
   * synthesised from a presigned request.
   * @return HandoffAuthResult The authentication result.
   */
  HandoffAuthResult
  _grpc_auth(const DoutPrefixProvider *dpp, const std::string &auth,
             const std::optional<AuthorizationParameters> &authorization_param,
             const std::string_view &session_token,
             const std::string_view &access_key_id,
             const std::string_view &string_to_sign,
             const std::string_view &signature, const req_state *const s,
             optional_yield y, bool is_presigned_request);

  /**
   * @brief Authorize an anonymous request.
   *
   * Send an authorization request to the Authenticator. Obviously there's no
   * authentication to do, but we can still create an AuthorizationParameters
   * struct and ask the Authenticator's opinion on the request. Everything
   * necessary to construct the AuthorizationParameters is found in \p s.
   *
   * @param dpp DoutPrefixProvider.
   * @param s Pointer to req_state.
   * @param y An optional yield token.
   * @return HandoffAuthResult
   */
  HandoffAuthResult anonymous_authorize(const DoutPrefixProvider* dpp,
      const req_state* const s,
      optional_yield y);

  /**
   * @brief Attempt to retrieve a signing key from the Authenticator.
   *
   * Request the signing key from the Authenticator. The signing key has a
   * validity of one day, so must be cached only with careful consideration.
   * We definitely should not cache it for a whole day.
   *
   * This can fail for a few reasons. The RPC can fail, or the Authenticator
   * may choose not to honour the request. We send the Authorization: header
   * and the internal transaction ID to try to help the Authenticator make a
   * decision.
   *
   * @param dpp The DoutPrefixProvider
   * @param auth The HTTP Authorization: header. Will be send verbatim to the
   * Authenticator.
   * @param s The request.
   * @param y The otional yield context.
   * @return std::optional<std::vector<uint8_t>> The signing key, or nullopt
   * on failure.
   */
  std::optional<std::vector<uint8_t>>
  get_signing_key(const DoutPrefixProvider *dpp, const std::string auth,
                  const req_state *const s, optional_yield y);

  /**
   * @brief Construct an Authorization header from the parsed query string
   * parameters.
   *
   * The Authorization header is a fairly concise way of sending a bunch of
   * bundled parameters to the Authenticator. So if (as would be the case with
   * a presigned URL) we don't get an Authorization header, see if we can
   * synthesize one from the query parameters.
   *
   * This function first has to distinguish between v2 and v4 parameters
   * (normally v2 if no region is supplied, defaulting to us-east-1). Then it
   * has to parse the completely distinct parameters for each version into a
   * v2 or v4 Authorization: header, via synthesize_v2_header() or
   * synthesize_v4_header() respectively.
   *
   * @param dpp DoutPrefixProvider.
   * @param s The request.
   * @return std::optional<std::string> The header on success, or std::nullopt
   * on any failure.
   */
  std::optional<std::string> synthesize_auth_header(
      const DoutPrefixProvider* dpp,
      const req_state* s);

  /**
   * @brief Authorize the operation via the external Authorizer.
   *
   * Call out to the external Authorizer to verify the operation, using a
   * combination of the req_state (in the \p s field of the \p op parameter),
   * our saved authorization state (if any), and the operation code (e.g.
   * rgw::IAM::GetObject).
   *
   * \p s is non-const because we might modify it by, say, loading bucket or
   * object tags.
   *
   * This single-operation version calls verify_permissions() and
   * returns the first (and only) result in the vector.
   *
   * @param op The RGWOp-subclass object pointer.
   * @param s The req_state object.
   * @param operation The operation code.
   * @param y Optional yield.
   * @return int Return code. 0 for success, <0 for error, typically -EACCES.
   */
  int verify_permission(const RGWOp* op, req_state* s,
      uint64_t operation, optional_yield y);

  /**
   * @brief Authorize the operation via the external Authorizer.
   *
   * Call out to the external Authorizer to check each operation, using a
   * combination of the req_state (in the \p s field of the \p op parameter),
   * our saved authorization state (if any), and the operation code (e.g.
   * rgw::IAM::GetObject).
   *
   * \p s is non-const because we might modify it by, say, loading bucket or
   * object tags.
   *
   * @param op The RGWOp-subclass object pointer.
   * @param s The req_state object.
   * @param operations A vector of operation codes to check.
   * @param y Optional yield.
   * @return std::vector<int> A vector of return codes, one for each code in
   * \p operations in order. 0 for success, <0 for error, typically -EACCES.
   */
  std::vector<int> verify_permissions(const RGWOp* op, req_state* s,
      const std::vector<uint64_t>& operations, optional_yield y);

  /**
   * @brief Update in s->handoff_authz the set of required extra data as
   * specified by the Authorizer.
   *
   * @param dpp The DoutPrefixProvider.
   * @param extra_spec The ExtraDataSpecification to load.
   * @param op The RGWOp object.
   * @param s The req_state.
   */
  void verify_permission_update_authz_state(const DoutPrefixProvider* dpp,
      const ExtraDataSpecification& extra_spec,
      const RGWOp* op, const req_state* s);

  /**
   * @brief Assuming an already-parsed (via synthesize_auth_header) presigned
   * header URL, check that the given expiry time has not expired. Note that
   * in v17.2.6, this won't get called - RGW checks the expiry time before
   * even calling our authentication engine.
   *
   * Fail closed - if we can't parse the parameters to check, assume we're not
   * authenticated.
   *
   * The fields are version-specific. For the v2-ish URLs (no region
   * specified), we're given an expiry unix time to compare against. For the
   * v4-type URLs (region specified), we're given a start time and a delta in
   * seconds.
   *
   * @param dpp DoutPrefixProvider.
   * @param s The request.
   * @param now The current UNIX time (seconds since the epoch).
   * @return true The request has not expired
   * @return false The request has expired, or a check was not possible
   */
  bool valid_presigned_time(const DoutPrefixProvider* dpp, const req_state* s, time_t now);

}; // class HandoffHelperImpl

/****************************************************************************/

/**
 * @brief Set the Authorization Common Timestamp object to 'now'.
 *
 * @param common a mutable AuthorizationCommon object.
 */
void SetAuthorizationCommonTimestamp(::authorizer::v1::AuthorizationCommon* common);

// The type we'll use to store object tags. Note that Amazon has a limit of
// max 10 object tags.
using objtag_map_type = boost::container::flat_map<std::string, std::string>;

// Function used to provide an alternate implementation to load object tags.
// Intended for unit testing without having to mock giant swathes of the SAL.
using load_object_tags_function = std::function<int(
    const DoutPrefixProvider* dpp,
    const req_state* s,
    objtag_map_type& obj_tags,
    optional_yield y)>;

/**
 * @brief Given a req_state, a HandoffAuthzState and a vector of operation
 * code, create and populate an ::authorizer::v1::AuthorizeV2Request protobuf
 * message.
 *
 * On failure, return std::nullopt.
 *
 * All the state required to fill the request should be contained in the
 * request and in the HandoffAuthzState, except for the actual operation codes
 * which are provided. Each operation code is an enum value, e.g.
 * rgw::IAM::GetObject, which will be mapped onto the equivalent
 * ::authorizer::v1::S3Opcode value.
 *
 * Note that the extra data fields of the request will only be filled in if
 * the corresponding toggles (HandoffAuthzState::set_bucket_tags_required(),
 * HandoffAuthzState::set_object_tags_required()) are set to true.
 *
 * We may modify \p s. For example, loading extra data such as tags may
 * require modification of s->env.
 *
 * @param dpp A DoutPrefixProvider for logging.
 * @param s The req_state. May be modified!
 * @param operations A vector of opcodes. Use the values defined in
 * <rgw/rgw_iam_policy.h> with prefix 's3'.
 * @param subrequest_index The subrequest index.
 * @param y Optional yield.
 * @param alt_load Optional alternative implementation of the object tag
 * loader function.
 * @return std::optional<::authorizer::v1::AuthorizeRequest> A populated
 * AuthorizeRequest message on success, std::nullopt on failure.
 */
std::optional<::authorizer::v1::AuthorizeV2Request> PopulateAuthorizeRequest(const DoutPrefixProvider* dpp,
    req_state* s, std::vector<uint64_t> operations, uint32_t subrequest_index, optional_yield y,
    std::optional<load_object_tags_function> alt_load = std::nullopt);

/**
 * @brief Single operation version of PopulateAuthorizeRequest.
 *
 * A shorthand for the common case where we only need to authorize a single
 * operation.
 *
 * @param dpp A DoutPrefixProvider for logging.
 * @param s The req_state. May be modified!
 * @param operation An opcode.
 * @param subrequest_index The subrequest index.
 * @param y Optional yield.
 * @param alt_load Optional alternative implementation of the object tag
 * loader function.
 * @return std::optional<::authorizer::v1::AuthorizeV2Request> A populated
 * AuthorizeRequest message on success, std::nullopt on failure.
 */
inline std::optional<::authorizer::v1::AuthorizeV2Request> PopulateAuthorizeRequest(const DoutPrefixProvider* dpp,
    req_state* s, uint64_t operation, uint32_t subrequest_index, optional_yield y,
    std::optional<load_object_tags_function> alt_load = std::nullopt)
{
  return PopulateAuthorizeRequest(dpp, s, std::vector<uint64_t> { operation }, subrequest_index, y, alt_load);
}

/**
 * @brief Populate ExtraData and ExtraDataSpecification using the SAL, or a
 * provided alternative implementation.
 *
 * @param dpp The DouPrefixProvider.
 * @param s The req_state.
 * @param extra_data_provided an ExtraDataProvided to populate.
 * @param extra_data an ExtraData to populate.
 * @param y Optional yield.
 * @param alt_load Optional alternative implementation of the object tag
 * loader function.
 * @return int Zero on success, <0 error code on failure.
 */
int PopulateExtraDataObjectTags(const DoutPrefixProvider* dpp, const req_state* s,
    ::authorizer::v1::ExtraDataSpecification* extra_data_provided,
    ::authorizer::v1::ExtraData* extra_data, optional_yield y,
    std::optional<load_object_tags_function> alt_load = std::nullopt);

/**
 * @brief Given a req_state with a populated HandoffAuthzState, load whatever
 * extra data into the request that the HandoffAuthzState says is required.
 *
 * Note the alt_load parameter is for unit testing only. It should be
 * std::nullopt in production, but in tests it provides an alternative way of
 * loading extra data that doesn't reuire mocking the SAL.
 *
 * @param dpp
 * @param s
 * @param spec The ExtraDataSpecification message to update.
 * @param extra_data The ExtraData message to update.
 * @param y Optional yield.
 * @param alt_load Optional alternative implementation of the object tag
 * loader function.
 * @return int The error code. 0 on success, <0 on failure.
 */
int PopulateAuthorizeRequestLoadExtraData(const DoutPrefixProvider* dpp, req_state* s,
    ::authorizer::v1::ExtraDataSpecification* spec, ::authorizer::v1::ExtraData* extra_data,
    optional_yield y,
    std::optional<load_object_tags_function> alt_load = std::nullopt);

/**
 * @brief Given a req_state and an existing AuthorizeV2Request, populate the
 * environment field of the protobuf with the IAM environment variables set in
 * the req_state.
 *
 * This will clear any existing data in the environment field. That makes it
 * suitable for use in the resubmit workflow, where we want to pick up the
 * differences in the IAM environment variables caused by, say, loading bucket
 * or object tags.
 *
 * @param dpp A DoutPrefixProvider for logging.
 * @param s The req_state.
 * @param question The AuthorizeV2Question message.
 */
void PopulateAuthorizeRequestIAMEnvironment(const DoutPrefixProvider* dpp,
    req_state* s, ::authorizer::v1::AuthorizeV2Question* question);

/**
 * @brief Format a protobuf as a JSON string, or an error message on failure.
 *
 * Use:
 * ```
 *   ldpp_dout(dpp, 20) << "Request: " << proto_to_JSON(req) << dendl;
 * ```
 *
 * The compiler should be able to infer the appropriate type for the template.
 *
 * @tparam T The protobuf message type
 * @param proto The protobuf message.
 * @return std::string The output string, or an error message if the
 * formatting failed.
 */
template <typename T>
std::string proto_to_JSON(const T& proto)
{
  using namespace google::protobuf::util;
  JsonPrintOptions options;
  options.always_print_primitive_fields = true;
  std::string out;
  auto status = google::protobuf::util::MessageToJsonString(proto, &out, options);
  if (status.ok()) {
    return out;
  } else {
    return fmt::format(FMT_STRING("Error formatting protobuf as JSON: {}"), status.ToString());
  }
}

/// Output stream operator for ::authorizer::v1:AuthorizeRequest.
extern std::ostream& operator<<(std::ostream& os, const ::authorizer::v1::AuthorizeV2Request& res);
/// Output stream operator for ::authorizer::v1::AuthorizeResponse.
extern std::ostream& operator<<(std::ostream& os, const ::authorizer::v1::AuthorizeV2Response& res);
/// Output stream operator for ::authorizer::v1::ExtraData.
extern std::ostream& operator<<(std::ostream& os, const ::authorizer::v1::ExtraData& res);
/// Output stream operator for ::authorizer::v1::ExtraDataSpecification.
extern std::ostream& operator<<(std::ostream& os, const ::authorizer::v1::ExtraDataSpecification& res);

/****************************************************************************/

} /* namespace rgw */

#endif // RGW_HANDOFF_IMPL_H
