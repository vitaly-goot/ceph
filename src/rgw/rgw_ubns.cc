/**
 * @file rgw_ubns.cc
 * @author André Lucas (alucas@akamai.com)
 * @brief Unique Bucket Naming System (UBNS) implementation.
 * @version 0.1
 * @date 2024-03-20
 *
 * @copyright Copyright (c) 2024
 *
 */

#include "rgw_ubns.h"

#include "common/debug.h"
#include "include/ceph_assert.h"
#include "rgw_ubns_impl.h"

#include <fmt/format.h>
#include <iostream>
#include <memory>

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

namespace rgw {

std::string to_str(UBNSBucketUpdateState state)
{
  switch (state) {
  case UBNSBucketUpdateState::UNSPECIFIED:
    return "UNSPECIFIED";
  case UBNSBucketUpdateState::CREATED:
    return "CREATED";
  case UBNSBucketUpdateState::DELETING:
    return "DELETING";
  }
}

// This has to be here, in a .cc file where we know the size of
// UBNSClientImpl. It can't be in the header file. See
// https://www.fluentcpp.com/2017/09/22/make-pimpl-using-unique_ptr/ .
UBNSClient::UBNSClient()
    : impl_(std::make_unique<UBNSClientImpl>())
{
}

// This has to be here, in a .cc file where we know the size of
// UBNSClientImpl. It can't be in the header file. See
// https://www.fluentcpp.com/2017/09/22/make-pimpl-using-unique_ptr/ .
UBNSClient::~UBNSClient() { }

bool UBNSClient::init(CephContext* cct, const std::string& grpc_uri)
{
  return impl_->init(cct, grpc_uri);
}

void UBNSClient::shutdown()
{
  impl_->shutdown();
}

UBNSClientResult UBNSClient::add_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner)
{
  return impl_->add_bucket_entry(dpp, bucket_name, cluster_id, owner);
}
UBNSClientResult UBNSClient::delete_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner)
{
  return impl_->delete_bucket_entry(dpp, bucket_name, cluster_id, owner);
}
UBNSClientResult UBNSClient::update_bucket_entry(const DoutPrefixProvider* dpp, const std::string& bucket_name, const std::string& cluster_id, const std::string& owner, UBNSBucketUpdateState state)
{
  return impl_->update_bucket_entry(dpp, bucket_name, cluster_id, owner, state);
}

std::string UBNSClient::cluster_id() const
{
  return impl_->cluster_id();
}

std::string UBNSClientResult::to_string() const
{
  if (ok()) {
    return "UBNSClientResult(success,code=0)";
  } else {
    return fmt::format(FMT_STRING("UBNSClientResult(failure,code={},message='{}')"), code(), message());
  }
}

std::ostream& operator<<(std::ostream& os, const UBNSClientResult& r)
{
  os << r.to_string();
  return os;
}

static bool _check_configured_file_path(ConfigProxy& conf, const std::string& config_key)
{
  std::string path;
  dout(20) << __func__ << ": checking file for UBNS configuration key '" << config_key << "'." << dendl;
  if (conf.get_val(config_key, &path) == 0) {
    if (path.empty()) {
      derr << "FATAL: UBNS is set to enabled, but " << config_key << " is not properly set." << dendl;
      return false;
    }
    if (access(path.c_str(), R_OK) == -1) {
      derr << fmt::format(
          FMT_STRING("FATAL: UBNS is set to enabled, but {} file '{}' is not accessible: {}"),
          config_key, path, strerror(errno))
           << dendl;
      return false;
    }
  } else {
    derr << "FATAL: UBNS is set to enabled, but config key " << config_key << " is not set." << dendl;
    return false;
  }
  return true;
}

bool ubns_validate_startup_configuration(ConfigProxy& conf)
{
  // Re-check the enabled state. If it's not set, we've been called
  // inappropriately. This is a programming error.
  ceph_assertf_always(conf->rgw_ubns_enabled, "UBNS is not enabled, but we were called to validate configuration.");

  // The cluster_id *must* be set. UBNS won't work without this.
  if (conf->rgw_ubns_cluster_id.empty() || conf->rgw_ubns_cluster_id.size() < 3) {
    derr << "FATAL: UBNS is enabled, but rgw_cluster_id is not properly set." << dendl;
    return false;
  }

  if (conf->rgw_ubns_grpc_uri.empty()) {
    derr << "FATAL: UBNS is enabled, but rgw_ubns_grpc_uri is not properly set." << dendl;
    return false;
  }

  // Carefully check the mTLS certificate configuration. Check not only that
  // the configuration variables are set, but that the files themselves exist.
  if (conf->rgw_ubns_grpc_mtls_enabled) {
    std::vector<std::string> mtls_config_keys = {
      "rgw_ubns_grpc_mtls_ca_cert_file",
      "rgw_ubns_grpc_mtls_client_cert_file",
      "rgw_ubns_grpc_mtls_client_key_file"
    };
    int tls_files_verified = 0;
    for (const auto& key : mtls_config_keys) {
      if (_check_configured_file_path(conf, key)) {
        tls_files_verified++;
      }
    }
    if (tls_files_verified != mtls_config_keys.size()) {
      derr << "FATAL: UBNS mTLS is enabled, but one or more TLS files is not properly configured" << dendl;
      return false;
    }
  }

  dout(5) << "UBNS configuration validated." << dendl;
  return true;
}

} // namespace rgw
