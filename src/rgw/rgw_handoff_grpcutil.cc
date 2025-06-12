/**
 * @file rgw_handoff_grpcutil.cc
 * @author André Lucas (alucas@akamai.com)
 * @brief gRPC support utilities.
 * @version 0.1
 * @date 2024-07-26
 *
 * @copyright Copyright (c) 2024
 *
 */

#include "rgw/rgw_handoff_grpcutil.h"

#include "common/dout.h"
#include "common/tracer.h"
#include "rgw/rgw_iam_policy.h"
#include "rgw/rgw_tracer.h"

#ifdef HAVE_JAEGER
#include "opentelemetry/context/context.h"
#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/context/propagation/text_map_propagator.h"
#include "opentelemetry/context/runtime_context.h"
#include "opentelemetry/sdk/version/version.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#endif // HAVE_JAEGER

// #include "authorizer/v1/authorizer.grpc.pb.h"
#include "authorizer/v1/authorizer.pb.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

namespace rgw {

std::shared_ptr<grpc::Channel> HandoffGRPCChannel::get_channel() const
{
  std::shared_lock<chan_lock_t> lock(m_channel_);
  return channel_;
}

grpc::ChannelArguments HandoffGRPCChannel::get_default_channel_args(CephContext* const cct)
{
  grpc::ChannelArguments args;

  // Set our default backoff parameters. These are runtime-alterable.
  args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, cct->_conf->rgw_handoff_grpc_arg_initial_reconnect_backoff_ms);
  args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, cct->_conf->rgw_handoff_grpc_arg_max_reconnect_backoff_ms);
  args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, cct->_conf->rgw_handoff_grpc_arg_min_reconnect_backoff_ms);
  ldout(cct, 20) << fmt::format(FMT_STRING("HandoffGRPCChannel::{}: {}: reconnect_backoff(ms): initial/min/max={}/{}/{}"),
      __func__, description_,
      cct->_conf->rgw_handoff_grpc_arg_initial_reconnect_backoff_ms,
      cct->_conf->rgw_handoff_grpc_arg_min_reconnect_backoff_ms,
      cct->_conf->rgw_handoff_grpc_arg_max_reconnect_backoff_ms)
                 << dendl;

  return args;
}

void HandoffGRPCChannel::set_channel_args(CephContext* const cct, grpc::ChannelArguments& args)
{
  std::unique_lock<chan_lock_t> l { m_channel_ };
  channel_args_ = std::make_optional(args);
}

bool HandoffGRPCChannel::set_channel_uri(CephContext* const cct, const std::string& new_uri)
{
  ldout(cct, 5) << fmt::format(FMT_STRING("HandoffGRPCChannel::set_channel_uri: {}: begin set uri '{}'"), description_, new_uri) << dendl;
  std::unique_lock<chan_lock_t> g(m_channel_);
  if (!channel_args_) {
    auto args = get_default_channel_args(cct);
    // Don't use set_channel_args(), which takes lock m_channel_.
    channel_args_ = std::make_optional(std::move(args));
  }
  // XXX grpc::InsecureChannelCredentials()...
  auto new_channel = grpc::CreateCustomChannel(new_uri, grpc::InsecureChannelCredentials(), *channel_args_);
  if (!new_channel) {
    ldout(cct, 0) << fmt::format(FMT_STRING("HandoffGRPCChannel::set_channel_uri: {}: ERROR: Failed to create new gRPC channel for URI {}"), description_, new_uri) << dendl;
    return false;
  } else {
    ldout(cct, 1) << fmt::format(FMT_STRING("HandoffGRPCChannel::set_channel_uri: {}: set uri '{}' success"), description_, new_uri) << dendl;
    channel_ = std::move(new_channel);
    channel_uri_ = new_uri;
    return true;
  }
}

/**
 * @brief Map from RGW IAM S3 operation to S3Opcode.
 *
 * This is a map from the RGW IAM S3 operation to the S3Opcode enum used in
 * the Authorizer protobuf.
 *
 * Now, I know this was set up so that the conversion could be easily
 * performed, so we can just add one to the RGW IAM operation code to get the
 * enum. However, I don't know for sure that the enum values will never
 * change, and I don't know that more won't be added out-of-order, or even
 * removed. So, I'm playing it safe here. It's not like it's egregiously
 * expensive.
 */
static std::unordered_map<uint64_t, authorizer::v1::S3Opcode> iam_s3_to_s3opcode = {
  { ::rgw::IAM::s3GetObject, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT },
  { ::rgw::IAM::s3GetObjectVersion, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_VERSION },
  { ::rgw::IAM::s3PutObject, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT },
  { ::rgw::IAM::s3GetObjectAcl, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_ACL },
  { ::rgw::IAM::s3GetObjectVersionAcl, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_VERSION_ACL },
  { ::rgw::IAM::s3PutObjectAcl, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_ACL },
  { ::rgw::IAM::s3PutObjectVersionAcl, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_VERSION_ACL },
  { ::rgw::IAM::s3DeleteObject, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_OBJECT },
  { ::rgw::IAM::s3DeleteObjectVersion, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_OBJECT_VERSION },
  { ::rgw::IAM::s3ListMultipartUploadParts, ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_MULTIPART_UPLOAD_PARTS },
  { ::rgw::IAM::s3AbortMultipartUpload, ::authorizer::v1::S3Opcode::S3_OPCODE_ABORT_MULTIPART_UPLOAD },
  { ::rgw::IAM::s3GetObjectTorrent, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_TORRENT },
  { ::rgw::IAM::s3GetObjectVersionTorrent, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_VERSION_TORRENT },
  { ::rgw::IAM::s3RestoreObject, ::authorizer::v1::S3Opcode::S3_OPCODE_RESTORE_OBJECT },
  { ::rgw::IAM::s3CreateBucket, ::authorizer::v1::S3Opcode::S3_OPCODE_CREATE_BUCKET },
  { ::rgw::IAM::s3DeleteBucket, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_BUCKET },
  { ::rgw::IAM::s3ListBucket, ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_BUCKET },
  { ::rgw::IAM::s3ListBucketVersions, ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_BUCKET_VERSIONS },
  { ::rgw::IAM::s3ListAllMyBuckets, ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_ALL_MY_BUCKETS },
  { ::rgw::IAM::s3ListBucketMultipartUploads, ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_BUCKET_MULTIPART_UPLOADS },
  { ::rgw::IAM::s3GetAccelerateConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_ACCELERATE_CONFIGURATION },
  { ::rgw::IAM::s3PutAccelerateConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_ACCELERATE_CONFIGURATION },
  { ::rgw::IAM::s3GetBucketAcl, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_ACL },
  { ::rgw::IAM::s3PutBucketAcl, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_ACL },
  { ::rgw::IAM::s3GetBucketCORS, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_CORS },
  { ::rgw::IAM::s3PutBucketCORS, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_CORS },
  { ::rgw::IAM::s3GetBucketVersioning, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_VERSIONING },
  { ::rgw::IAM::s3PutBucketVersioning, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_VERSIONING },
  { ::rgw::IAM::s3GetBucketRequestPayment, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_REQUEST_PAYMENT },
  { ::rgw::IAM::s3PutBucketRequestPayment, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_REQUEST_PAYMENT },
  { ::rgw::IAM::s3GetBucketLocation, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_LOCATION },
  { ::rgw::IAM::s3GetBucketPolicy, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_POLICY },
  { ::rgw::IAM::s3DeleteBucketPolicy, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_BUCKET_POLICY },
  { ::rgw::IAM::s3PutBucketPolicy, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_POLICY },
  { ::rgw::IAM::s3GetBucketNotification, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_NOTIFICATION },
  { ::rgw::IAM::s3PutBucketNotification, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_NOTIFICATION },
  { ::rgw::IAM::s3GetBucketLogging, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_LOGGING },
  { ::rgw::IAM::s3PutBucketLogging, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_LOGGING },
  { ::rgw::IAM::s3GetBucketTagging, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_TAGGING },
  { ::rgw::IAM::s3PutBucketTagging, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_TAGGING },
  { ::rgw::IAM::s3GetBucketWebsite, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_WEBSITE },
  { ::rgw::IAM::s3PutBucketWebsite, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_WEBSITE },
  { ::rgw::IAM::s3DeleteBucketWebsite, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_BUCKET_WEBSITE },
  { ::rgw::IAM::s3GetLifecycleConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_LIFECYCLE_CONFIGURATION },
  { ::rgw::IAM::s3PutLifecycleConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_LIFECYCLE_CONFIGURATION },
  { ::rgw::IAM::s3PutReplicationConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_REPLICATION_CONFIGURATION },
  { ::rgw::IAM::s3GetReplicationConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_REPLICATION_CONFIGURATION },
  { ::rgw::IAM::s3DeleteReplicationConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_REPLICATION_CONFIGURATION },
  { ::rgw::IAM::s3GetObjectTagging, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_TAGGING },
  { ::rgw::IAM::s3PutObjectTagging, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_TAGGING },
  { ::rgw::IAM::s3DeleteObjectTagging, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_OBJECT_TAGGING },
  { ::rgw::IAM::s3GetObjectVersionTagging, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_VERSION_TAGGING },
  { ::rgw::IAM::s3PutObjectVersionTagging, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_VERSION_TAGGING },
  { ::rgw::IAM::s3DeleteObjectVersionTagging, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_OBJECT_VERSION_TAGGING },
  { ::rgw::IAM::s3PutBucketObjectLockConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_OBJECT_LOCK_CONFIGURATION },
  { ::rgw::IAM::s3GetBucketObjectLockConfiguration, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_OBJECT_LOCK_CONFIGURATION },
  { ::rgw::IAM::s3PutObjectRetention, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_RETENTION },
  { ::rgw::IAM::s3GetObjectRetention, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_RETENTION },
  { ::rgw::IAM::s3PutObjectLegalHold, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_LEGAL_HOLD },
  { ::rgw::IAM::s3GetObjectLegalHold, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_LEGAL_HOLD },
  { ::rgw::IAM::s3BypassGovernanceRetention, ::authorizer::v1::S3Opcode::S3_OPCODE_BYPASS_GOVERNANCE_RETENTION },
  { ::rgw::IAM::s3GetBucketPolicyStatus, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_POLICY_STATUS },
  { ::rgw::IAM::s3PutPublicAccessBlock, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_PUBLIC_ACCESS_BLOCK },
  { ::rgw::IAM::s3GetPublicAccessBlock, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_PUBLIC_ACCESS_BLOCK },
  { ::rgw::IAM::s3DeletePublicAccessBlock, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_PUBLIC_ACCESS_BLOCK },
  { ::rgw::IAM::s3GetBucketPublicAccessBlock, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_PUBLIC_ACCESS_BLOCK },
  { ::rgw::IAM::s3PutBucketPublicAccessBlock, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_PUBLIC_ACCESS_BLOCK },
  { ::rgw::IAM::s3DeleteBucketPublicAccessBlock, ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_BUCKET_PUBLIC_ACCESS_BLOCK },
  { ::rgw::IAM::s3GetBucketEncryption, ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_ENCRYPTION },
  { ::rgw::IAM::s3PutBucketEncryption, ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_ENCRYPTION },
};

static std::unordered_map<::authorizer::v1::S3Opcode, uint64_t> s3opcode_to_iam_s3 = {
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT, ::rgw::IAM::s3GetObject },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_VERSION, ::rgw::IAM::s3GetObjectVersion },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT, ::rgw::IAM::s3PutObject },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_ACL, ::rgw::IAM::s3GetObjectAcl },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_VERSION_ACL, ::rgw::IAM::s3GetObjectVersionAcl },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_ACL, ::rgw::IAM::s3PutObjectAcl },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_VERSION_ACL, ::rgw::IAM::s3PutObjectVersionAcl },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_OBJECT, ::rgw::IAM::s3DeleteObject },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_OBJECT_VERSION, ::rgw::IAM::s3DeleteObjectVersion },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_MULTIPART_UPLOAD_PARTS, ::rgw::IAM::s3ListMultipartUploadParts },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_ABORT_MULTIPART_UPLOAD, ::rgw::IAM::s3AbortMultipartUpload },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_TORRENT, ::rgw::IAM::s3GetObjectTorrent },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_VERSION_TORRENT, ::rgw::IAM::s3GetObjectVersionTorrent },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_RESTORE_OBJECT, ::rgw::IAM::s3RestoreObject },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_CREATE_BUCKET, ::rgw::IAM::s3CreateBucket },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_BUCKET, ::rgw::IAM::s3DeleteBucket },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_BUCKET, ::rgw::IAM::s3ListBucket },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_BUCKET_VERSIONS, ::rgw::IAM::s3ListBucketVersions },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_ALL_MY_BUCKETS, ::rgw::IAM::s3ListAllMyBuckets },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_LIST_BUCKET_MULTIPART_UPLOADS, ::rgw::IAM::s3ListBucketMultipartUploads },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_ACCELERATE_CONFIGURATION, ::rgw::IAM::s3GetAccelerateConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_ACCELERATE_CONFIGURATION, ::rgw::IAM::s3PutAccelerateConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_ACL, ::rgw::IAM::s3GetBucketAcl },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_ACL, ::rgw::IAM::s3PutBucketAcl },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_CORS, ::rgw::IAM::s3GetBucketCORS },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_CORS, ::rgw::IAM::s3PutBucketCORS },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_VERSIONING, ::rgw::IAM::s3GetBucketVersioning },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_VERSIONING, ::rgw::IAM::s3PutBucketVersioning },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_REQUEST_PAYMENT, ::rgw::IAM::s3GetBucketRequestPayment },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_REQUEST_PAYMENT, ::rgw::IAM::s3PutBucketRequestPayment },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_LOCATION, ::rgw::IAM::s3GetBucketLocation },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_POLICY, ::rgw::IAM::s3GetBucketPolicy },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_BUCKET_POLICY, ::rgw::IAM::s3DeleteBucketPolicy },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_POLICY, ::rgw::IAM::s3PutBucketPolicy },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_NOTIFICATION, ::rgw::IAM::s3GetBucketNotification },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_NOTIFICATION, ::rgw::IAM::s3PutBucketNotification },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_LOGGING, ::rgw::IAM::s3GetBucketLogging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_LOGGING, ::rgw::IAM::s3PutBucketLogging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_TAGGING, ::rgw::IAM::s3GetBucketTagging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_TAGGING, ::rgw::IAM::s3PutBucketTagging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_WEBSITE, ::rgw::IAM::s3GetBucketWebsite },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_WEBSITE, ::rgw::IAM::s3PutBucketWebsite },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_BUCKET_WEBSITE, ::rgw::IAM::s3DeleteBucketWebsite },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_LIFECYCLE_CONFIGURATION, ::rgw::IAM::s3GetLifecycleConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_LIFECYCLE_CONFIGURATION, ::rgw::IAM::s3PutLifecycleConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_REPLICATION_CONFIGURATION, ::rgw::IAM::s3PutReplicationConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_REPLICATION_CONFIGURATION, ::rgw::IAM::s3GetReplicationConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_REPLICATION_CONFIGURATION, ::rgw::IAM::s3DeleteReplicationConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_TAGGING, ::rgw::IAM::s3GetObjectTagging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_TAGGING, ::rgw::IAM::s3PutObjectTagging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_OBJECT_TAGGING, ::rgw::IAM::s3DeleteObjectTagging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_VERSION_TAGGING, ::rgw::IAM::s3GetObjectVersionTagging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_VERSION_TAGGING, ::rgw::IAM::s3PutObjectVersionTagging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_OBJECT_VERSION_TAGGING, ::rgw::IAM::s3DeleteObjectVersionTagging },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_OBJECT_LOCK_CONFIGURATION, ::rgw::IAM::s3PutBucketObjectLockConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_OBJECT_LOCK_CONFIGURATION, ::rgw::IAM::s3GetBucketObjectLockConfiguration },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_RETENTION, ::rgw::IAM::s3PutObjectRetention },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_RETENTION, ::rgw::IAM::s3GetObjectRetention },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_OBJECT_LEGAL_HOLD, ::rgw::IAM::s3PutObjectLegalHold },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_OBJECT_LEGAL_HOLD, ::rgw::IAM::s3GetObjectLegalHold },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_BYPASS_GOVERNANCE_RETENTION, ::rgw::IAM::s3BypassGovernanceRetention },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_POLICY_STATUS, ::rgw::IAM::s3GetBucketPolicyStatus },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_PUBLIC_ACCESS_BLOCK, ::rgw::IAM::s3PutPublicAccessBlock },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_PUBLIC_ACCESS_BLOCK, ::rgw::IAM::s3GetPublicAccessBlock },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_PUBLIC_ACCESS_BLOCK, ::rgw::IAM::s3DeletePublicAccessBlock },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_PUBLIC_ACCESS_BLOCK, ::rgw::IAM::s3GetBucketPublicAccessBlock },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_PUBLIC_ACCESS_BLOCK, ::rgw::IAM::s3PutBucketPublicAccessBlock },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_DELETE_BUCKET_PUBLIC_ACCESS_BLOCK, ::rgw::IAM::s3DeleteBucketPublicAccessBlock },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_GET_BUCKET_ENCRYPTION, ::rgw::IAM::s3GetBucketEncryption },
  { ::authorizer::v1::S3Opcode::S3_OPCODE_PUT_BUCKET_ENCRYPTION, ::rgw::IAM::s3PutBucketEncryption },
};

/**
 * @brief Map from an RGW IAM S3 opcode (e.g. ::rgw::IAM::S3GetObject) to a
 * gRPC opcode (e.g. ::authorizer::v1::S3Opcode::S3_GET_OBJECT).
 *
 * @param iam_s3 The RGW S3 IAM opcode.
 * @return std::optional<::authorizer::v1::S3Opcode> A gRPC S3 opcode if a
 * mapping exists, otherwise std::nullopt.
 */
std::optional<::authorizer::v1::S3Opcode> iam_s3_to_grpc_opcode(uint64_t iam_s3)
{
  auto it = iam_s3_to_s3opcode.find(iam_s3);
  if (it != iam_s3_to_s3opcode.end()) {
    return std::make_optional(it->second);
  } else {
    return std::nullopt;
  }
}

/**
 * @brief Map from a gRPC Authorizer S3 opcode (e.g.
 * ::authorizer::v1::S3_GET_OBJECT) to an RGW IAM S3 opcode (e.g.
 * ::rgw::IAM::S3GetObject).
 *
 * @param grpc_opcode The gRPC S3 opcode.
 * @return std::optional<uint64_t> An RGW S3 IAM opcode if a mapping exists,
 * otherwise std::nullopt.
 */
std::optional<uint64_t> grpc_opcode_to_iam_s3(::authorizer::v1::S3Opcode grpc_opcode)
{
  auto it = s3opcode_to_iam_s3.find(grpc_opcode);
  if (it != s3opcode_to_iam_s3.end()) {
    return std::make_optional(it->second);
  } else {
    return std::nullopt;
  }
}

void populate_trace_context(grpc::ClientContext *context, jspan trace) {
#ifdef HAVE_JAEGER
  if (!trace) {
    return;
  }
  auto scope = tracing::rgw::tracer.WithActiveSpan(trace);

  auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
  HandoffGrpcClientCarrier carrier(context);
  auto prop = opentelemetry::context::propagation::GlobalTextMapPropagator::
      GetGlobalPropagator();
  prop->Inject(carrier, current_ctx);
#endif // HAVE_JAEGER
}

std::optional<jspan> optional_trace(const req_state *s) {
  if (s->trace_enabled) {
    return s->trace;
  } else {
    return std::nullopt;
  }
}

} // namespace rgw
