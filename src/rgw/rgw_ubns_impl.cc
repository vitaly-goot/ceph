/**
 * @file rgw_ubns_impl.cc
 * @author André Lucas (alucas@akamai.com)
 * @brief Unique Bucket Naming System (UBNS) private implementation.
 * @version 0.1
 * @date 2024-03-20
 *
 * @copyright Copyright (c) 2024
 *
 */

#include "rgw_ubns_impl.h"

#include <errno.h>
#include <fstream>
#include <string>

#include "common/dout.h"
#include "rgw/rgw_common.h"

#include "ubdb/v1/ubdb.grpc.pb.h"
#include "ubdb/v1/ubdb.pb.h"

// These are 'standard' protobufs for the 'Richer error model'
// (https://grpc.io/docs/guides/error/).
#include "google/rpc/error_details.pb.h"
#include "google/rpc/status.pb.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

namespace rgw {

UBNSClientResult UBNSgRPCClient::add_bucket_request(const ubdb::v1::AddBucketEntryRequest& req)
{
  ::grpc::ClientContext context;
  ubdb::v1::AddBucketEntryResponse resp;
  auto status = stub_->AddBucketEntry(&context, req, &resp);
  return _add_bucket_xform_result(status);
}

UBNSClientResult UBNSgRPCClient::_add_bucket_xform_result(const ::grpc::Status& status)
{
  if (status.ok()) {
    return UBNSClientResult::success();
  }
  switch (status.error_code()) {
  case ::grpc::StatusCode::INTERNAL:
    return UBNSClientResult::error(ERR_INTERNAL_ERROR,
        fmt::format(FMT_STRING("ERR_INTERNAL_ERROR: gRPC code INTERNAL message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::DEADLINE_EXCEEDED:
    return UBNSClientResult::error(ETIMEDOUT,
        fmt::format(FMT_STRING("ETIMEDOUT: Terminated due to context: gRPC code DEADLINE_EXCEEDED message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::INVALID_ARGUMENT:
    return UBNSClientResult::error(ERR_UBNS_BAD_REQUEST,
        fmt::format(FMT_STRING("ERR_UBNS_BAD_REQUEST: Invalid or missing parameter: gRPC code INVALID_ARGUMENT message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::FAILED_PRECONDITION:
    return UBNSClientResult::error(ERR_SERVICE_UNAVAILABLE,
        fmt::format(FMT_STRING("ERR_SERVICE_UNAVAILABLE: Aborted due to being duplicated: gRPC code FAILED_PRECONDITION message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::ALREADY_EXISTS:
    // Note: The backend docs say this should return 200 (OK), but we can't do
    // that here. RGW /will/ return the 200, but the UBNSCreateStateMachine
    // needs to know the difference between an idempotent recreate and a
    // first-time create. When it receives this error code, it knows to set
    // the state 'SOFT_FAIL' which allows the RGW create process to continue,
    // but prevents the state machine from sending Update messages.
    //
    return UBNSClientResult::error(ERR_UBNS_BUCKET_ALREADY_OWNED_BY_YOU,
        fmt::format(FMT_STRING("ERR_UBNS_BUCKET_ALREADY_OWNED_BY_YOU: User already owns bucket: gRPC code ALREADY_EXISTS message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::ABORTED:
    return UBNSClientResult::error(EEXIST,
        fmt::format(FMT_STRING("EEXIST: Another user owns the bucket: gRPC code ABORTED message: {}"),
            status.error_message()));

  default:
    return UBNSClientResult::error(ERR_INTERNAL_ERROR,
        fmt::format(FMT_STRING("ERR_INTERNAL_ERROR: Unexpected gRPC numeric error code {} message: {}"),
            status.error_code(), status.error_message()));
  }
}

UBNSClientResult UBNSgRPCClient::delete_bucket_request(const ubdb::v1::DeleteBucketEntryRequest& req)
{
  ::grpc::ClientContext context;
  ubdb::v1::DeleteBucketEntryResponse resp;
  auto status = stub_->DeleteBucketEntry(&context, req, &resp);
  return _delete_bucket_xform_result(status);
}

UBNSClientResult UBNSgRPCClient::_delete_bucket_xform_result(const ::grpc::Status& status)
{
  if (status.ok()) {
    // The document says '204 NoContent', but we're relying on RGW to supply
    // the return code to a success result. The user will get what RGW decides
    // to send.
    return UBNSClientResult::success();
  }
  switch (status.error_code()) {
  case ::grpc::StatusCode::INTERNAL:
    return UBNSClientResult::error(ERR_INTERNAL_ERROR,
        fmt::format(FMT_STRING("ERR_INTERNAL_ERROR: Internal error: gRPC code INTERNAL message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::DEADLINE_EXCEEDED:
    return UBNSClientResult::error(ETIMEDOUT,
        fmt::format(FMT_STRING("ETIMEDOUT: Terminated due to context: gRPC code DEADLINE_EXCEEDED message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::INVALID_ARGUMENT:
    return UBNSClientResult::error(ERR_UBNS_BAD_REQUEST,
        fmt::format(FMT_STRING("ERR_UBNS_BAD_REQUEST: Invalid or missing parameter: gRPC code INVALID_ARGUMENT message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::FAILED_PRECONDITION:
    return UBNSClientResult::error(ERR_NOT_FOUND,
        fmt::format(FMT_STRING("ERR_NOT_FOUND: Bucket is hosted on another cluster: gRPC code FAILED_PRECONDITION message: {}"),
            status.error_message()));

  default:
    return UBNSClientResult::error(ERR_INTERNAL_ERROR,
        fmt::format(FMT_STRING("Unexpected gRPC numeric error code {} message: {}"),
            status.error_code(), status.error_message()));
  }
}

UBNSClientResult UBNSgRPCClient::update_bucket_request(const ubdb::v1::UpdateBucketEntryRequest& req)
{
  ::grpc::ClientContext context;
  ubdb::v1::UpdateBucketEntryResponse resp;
  auto status = stub_->UpdateBucketEntry(&context, req, &resp);
  return _update_bucket_xform_result(status);
}

UBNSClientResult UBNSgRPCClient::_update_bucket_xform_result(const ::grpc::Status& status)
{
  if (status.ok()) {
    return UBNSClientResult::success();
  }
  switch (status.error_code()) {
  case ::grpc::StatusCode::INTERNAL:
    return UBNSClientResult::error(ERR_INTERNAL_ERROR,
        fmt::format(FMT_STRING("ERR_INTERNAL_ERROR: Internal error: gRPC code INTERNAL message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::DEADLINE_EXCEEDED:
    return UBNSClientResult::error(ETIMEDOUT,
        fmt::format(FMT_STRING("ETIMEDOUT: Terminated due to context: gRPC code DEADLINE_EXCEEDED message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::INVALID_ARGUMENT:
    // ubns/internal/ubdb/grpc/grpc.go lines 301, 358.
    if (status.error_message().find("invalid state") != std::string::npos || status.error_message().find("failed to delete deployment from bucketentry") != std::string::npos) {
      return UBNSClientResult::error(ERR_SERVICE_UNAVAILABLE,
          fmt::format(FMT_STRING("ERR_SERVICE_UNAVAILABLE: Invalid state: gRPC code INVALID_ARGUMENT message: {}"),
              status.error_message()));
    } else {
      return UBNSClientResult::error(ERR_UBNS_BAD_REQUEST,
          fmt::format(FMT_STRING("ERR_UBNS_BAD_REQUEST: Invalid or missing parameter: gRPC code INVALID_ARGUMENT message: {}"),
              status.error_message()));
    }

  case ::grpc::StatusCode::NOT_FOUND:
    return UBNSClientResult::error(ERR_NOT_FOUND,
        fmt::format(FMT_STRING("ERR_NOT_FOUND: BucketEntry not found: gRPC code NOT_FOUND message: {}"),
            status.error_message()));

  case ::grpc::StatusCode::FAILED_PRECONDITION:
    // ubns/internal/ubdb/grpc/grpc.go line 348
    if (status.error_message().find("failed to delete deployment from bucketentry") != std::string::npos) {
      return UBNSClientResult::error(ERR_NOT_FOUND,
          fmt::format(FMT_STRING("ERR_NOT_FOUND: Bucket is hosted on another cluster: gRPC code FAILED_PRECONDITION message: {}"),
              status.error_message()));
    } else {
      return UBNSClientResult::error(ERR_SERVICE_UNAVAILABLE,
          fmt::format(FMT_STRING("ERR_SERVICE_UNAVAILABLE: bucketentry exists in multiple Ceph clusters: gRPC code FAILED_PRECONDITION message: {}"),
              status.error_message()));
    }

  default:
    return UBNSClientResult::error(ERR_INTERNAL_ERROR,
        fmt::format(FMT_STRING("Unexpected gRPC code {} gRPC message: {}"),
            status.error_code(), status.error_message()));
  }
}

bool UBNSClientImpl::init(CephContext* cct, const std::string& grpc_uri)
{
  ceph_assert(cct != nullptr);

  auto& conf = cct->_conf;

  // Set up the configuration observer.
  config_obs_.init(cct);

  cluster_id_ = conf->rgw_ubns_cluster_id;

  // Empty grpc_uri (the default) means use the configuration value.
  auto uri = grpc_uri.empty() ? conf->rgw_ubns_grpc_uri : grpc_uri;

  mtls_enabled_ = conf->rgw_ubns_grpc_mtls_enabled;
  ldout(cct, 1) << fmt::format(FMT_STRING("UBNS: mTLS {}"), mtls_enabled_ ? "enabled" : "disabled") << dendl;

  if (!set_channel(cct, uri)) {
    lderr(cct) << "UBNS: failed to create initial gRPC channel" << dendl;
    return false;
  }
  return true;
}

void UBNSClientImpl::shutdown()
{
  // The gRPC channel shutdown will be handled by the unique_ptr, on
  // destruction.
}

std::optional<UBNSgRPCClient> UBNSClientImpl::safe_get_client(const DoutPrefixProvider* dpp)
{
  UBNSgRPCClient client {};
  std::shared_lock<std::shared_mutex> g(m_channel_);
  // Quick confidence check of channel_.
  if (!channel_) {
    ldpp_dout(dpp, 0) << "Unset gRPC channel" << dendl;
    return std::nullopt;
  }
  client.set_stub(channel_);
  return std::make_optional(std::move(client));
}

UBNSClientResult UBNSClientImpl::add_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner)
{
  ldpp_dout(dpp, 20) << __func__ << dendl;
  auto client = safe_get_client(dpp);
  if (!client) {
    return UBNSClientResult::error(ERR_INTERNAL_ERROR, "Internal error (could not fetch gRPC client)");
  }
  ubdb::v1::AddBucketEntryRequest req;
  req.set_bucket(bucket_name);
  req.set_cluster(cluster_id);
  req.set_owner(owner);
  ldpp_dout(dpp, 5) << fmt::format(FMT_STRING("UBNS: sending gRPC AddBucketRequest(bucket={},cluster={},owner={})"), req.bucket(), req.cluster(), req.owner()) << dendl;
  return client->add_bucket_request(req);
}

UBNSClientResult UBNSClientImpl::delete_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner)
{
  ldpp_dout(dpp, 20) << __func__ << dendl;
  auto client = safe_get_client(dpp);
  if (!client) {
    return UBNSClientResult::error(ERR_INTERNAL_ERROR, "Internal error (could not fetch gRPC client)");
  }
  ubdb::v1::DeleteBucketEntryRequest req;
  req.set_bucket(bucket_name);
  req.set_cluster(cluster_id);
  req.set_owner(owner);
  ldpp_dout(dpp, 5) << fmt::format(FMT_STRING("UBNS: sending gRPC DeleteBucketRequest(bucket={},cluster={},owner={})"), req.bucket(), req.cluster(), req.owner()) << dendl;
  return client->delete_bucket_request(req);
}

UBNSClientResult UBNSClientImpl::update_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner, UBNSBucketUpdateState state)
{
  ldpp_dout(dpp, 20) << __func__ << dendl;
  auto client = safe_get_client(dpp);
  if (!client) {
    return UBNSClientResult::error(ERR_INTERNAL_ERROR, "Internal error (could not fetch gRPC client)");
  }
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
  ldpp_dout(dpp, 1) << fmt::format(FMT_STRING("UBNS: sending gRPC UpdateBucketRequest(bucket={},cluster={},owner={},state={})"), req.bucket(), req.cluster(), req.owner(), to_str(state)) << dendl;
  return client->update_bucket_request(req);
}

grpc::ChannelArguments UBNSClientImpl::get_default_channel_args(CephContext* const cct)
{
  grpc::ChannelArguments args;

  // Set our default backoff parameters. These are runtime-alterable.
  args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, cct->_conf->rgw_ubns_grpc_arg_initial_reconnect_backoff_ms);
  args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, cct->_conf->rgw_ubns_grpc_arg_max_reconnect_backoff_ms);
  args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, cct->_conf->rgw_ubns_grpc_arg_min_reconnect_backoff_ms);
  ldout(cct, 20) << fmt::format(FMT_STRING("HandoffHelperImpl::{}: reconnect_backoff(ms): initial/min/max={}/{}/{}"),
      __func__,
      cct->_conf->rgw_ubns_grpc_arg_initial_reconnect_backoff_ms,
      cct->_conf->rgw_ubns_grpc_arg_min_reconnect_backoff_ms,
      cct->_conf->rgw_ubns_grpc_arg_max_reconnect_backoff_ms)
                 << dendl;

  return grpc::ChannelArguments();
}

bool UBNSClientImpl::_set_insecure_channel(CephContext* const cct, const std::string& new_uri)
{
  std::unique_lock<chan_lock_t> g(m_channel_);
  if (!channel_args_) {
    auto args = get_default_channel_args(cct);
    // Don't use set_channel_args(), which takes lock m_channel_.
    channel_args_ = std::make_optional(std::move(args));
  }
  // grpc::InsecureChannelCredentials() is appropriate here. For a TLS-enabled
  // channel, see set_mtls_channel().
  auto new_channel = grpc::CreateCustomChannel(new_uri, grpc::InsecureChannelCredentials(), *channel_args_);
  if (!new_channel) {
    ldout(cct, 0) << "UBNSClientImpl::set_channel_uri(): ERROR: Failed to create new gRPC channel " << new_uri << dendl;
    return false;
  } else {
    ldout(cct, 1) << "UBNSClientImpl::set_channel_uri(" << new_uri << ") success" << dendl;
    channel_ = std::move(new_channel);
    channel_uri_ = new_uri;
    return true;
  }
}

// Given a filename, return the contents, or nullopt on failure.
static std::optional<std::string> load_credential_from_file(CephContext* const cct, const std::string& descr, const std::string& path)
{
  ldout(cct, 0) << fmt::format(FMT_STRING("{}: Load credential '{}' from file '{}'"), __func__, descr, path) << dendl;
  try {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    std::string buffer;
    f.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    f.seekg(0, std::ios::end);
    buffer.resize(f.tellg());
    f.seekg(0, std::ios::beg);
    f.read(&buffer[0], buffer.size());
    f.close();
    return buffer;
  } catch (std::ifstream::failure& e) {
    ldout(cct, 0) << fmt::format(FMT_STRING("{}: ERROR: Failed to load {} from file '{}': {}"), __func__, descr, path, e.what()) << dendl;
    return std::nullopt;
  }
}

/**
 * @brief Write zero bytes to the credential, then clear it.
 *
 * @param cred The credential optional-string returned by
 * load_credential_from_file().
 */
static void wipe_credential(std::optional<std::string>& cred)
{
  std::fill(cred->begin(), cred->end(), '\0');
  cred->clear();
  cred.reset();
}

/**
 * @brief Write zero bytes to the string, then clear it.
 *
 * @param s A std::string.
 */
static void wipe_string(std::string& s)
{
  std::fill(s.begin(), s.end(), '\0');
  s.clear();
}

bool UBNSClientImpl::_set_mtls_channel(CephContext* const cct, const std::string& new_uri)
{
  auto& conf = cct->_conf;

  std::unique_lock<chan_lock_t> g(m_channel_);
  if (!channel_args_) {
    auto args = get_default_channel_args(cct);
    // Don't use set_channel_args(), which takes lock m_channel_.
    channel_args_ = std::make_optional(std::move(args));
  }

  // gRPC authentication: See https://grpc.io/docs/guides/auth/ .

  auto cred_options = grpc::SslCredentialsOptions();
  auto ca_cert = load_credential_from_file(cct, "CA cert", conf->rgw_ubns_grpc_mtls_ca_cert_file);
  if (!ca_cert) {
    return false;
  }
  cred_options.pem_root_certs = *ca_cert;
  wipe_credential(ca_cert);
  auto client_cert = load_credential_from_file(cct, "Client cert", conf->rgw_ubns_grpc_mtls_client_cert_file);
  if (!client_cert) {
    return false;
  }
  cred_options.pem_cert_chain = *client_cert;
  wipe_credential(client_cert);
  auto client_key = load_credential_from_file(cct, "Client key", conf->rgw_ubns_grpc_mtls_client_key_file);
  if (!client_key) {
    return false;
  }
  cred_options.pem_private_key = *client_key;
  wipe_credential(client_key);
  auto channel_creds = grpc::SslCredentials(cred_options);

  auto new_channel = grpc::CreateCustomChannel(new_uri, channel_creds, *channel_args_);
  wipe_string(cred_options.pem_root_certs);
  wipe_string(cred_options.pem_cert_chain);
  wipe_string(cred_options.pem_private_key);

  if (!new_channel) {
    ldout(cct, 0) << __func__ << ": ERROR: Failed to create new gRPC channel for uri a" << new_uri << dendl;
    return false;
  } else {
    ldout(cct, 1) << "UBNS: " << __func__ << "(" << new_uri << ") success" << dendl;
    channel_ = std::move(new_channel);
    channel_uri_ = new_uri;
    return true;
  }

  return false;
}

} // namespace rgw
