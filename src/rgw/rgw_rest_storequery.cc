// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/algorithm/string.hpp>
#include <boost/token_functions.hpp>
#include <boost/tokenizer.hpp>
#include <fmt/format.h>
#include <string>

#include "cls/rgw/cls_rgw_types.h"
#include "common/dout.h"
#include "rgw_common.h"
#include "rgw_op.h"
#include "rgw_rest_storequery.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

namespace rgw {

void RGWStoreQueryOp_Base::send_response_pre()
{
  if (op_ret) {
    set_req_state_err(s, op_ret);
  }
  auto ret = RGWHandler_REST::reallocate_formatter(s, RGWFormat::JSON);
  if (ret != 0) {
    ldpp_dout(this, 20) << "failed to set formatter to JSON" << dendl;
    set_req_state_err(s, -EINVAL);
  }
  dump_errno(s);
  end_header(s, this, "application/json", CHUNKED_TRANSFER_ENCODING, true, op_ret == -ENOENT);
  dump_start(s);
}

void RGWStoreQueryOp_Base::send_response_post()
{
  rgw_flush_formatter_and_reset(s, s->formatter);
}

void RGWStoreQueryOp_Base::send_response()
{
  send_response_pre();
  send_response_json();
  send_response_post();
}

void RGWStoreQueryOp_Ping::execute(optional_yield y)
{
  ldpp_dout(this, 20) << fmt::format(FMT_STRING("{}: {}({})"), typeid(this).name(),
      __func__, request_id_)
                      << dendl;
  // This can't fail.
  op_ret = 0;
}

void RGWStoreQueryOp_Ping::send_response_json()
{
  s->formatter->open_object_section("StoreQueryPingResult");
  s->formatter->dump_string("request_id", request_id_);
  s->formatter->close_section();
}

/**
 * @brief Query already-existing objects, or delete markers.
 *
 * Perform a 'regular' query, returning either pre-existing objects or (in
 * versioning-enabled buckets) delete markers for previously-existing objects.
 * In either case, the object is deemed to be found.
 *
 * We check for the current version and stop further searching the moment we
 * find it.
 *
 * However, since rgw::sal::Bucket::list() queries on a prefix not a key, we
 * also check for an exact key match each time.
 *
 * Note that op_ret will be set <0 in case failures other than 'not found'.
 * This indicates that we should abort the query process.
 *
 * @param y optional yield object.
 * @return true Success. The object was found, in this case it is either
 * present or a delete marker for it exists. op_ret==0.
 * @return false Failure. If op_ret==0, the object was simply not found. If
 * op_ret<0, a failure occurred.
 */
bool RGWStoreQueryOp_ObjectStatus::execute_simple_query(optional_yield y)
{
  bool found = false;

  rgw::sal::Bucket::ListParams params {};
  params.prefix = object_key_name_;
  // We want results even if the last object is a delete marker. In a bucket
  // without versioning a query for a deleted or nonexistent object will
  // return zero objects, for which we'll return ENOENT.
  params.list_versions = true;
  // We always want an ordered list of objects. This is the default atow.
  params.allow_unordered = false;

  do {
    rgw::sal::Bucket::ListResults results;
    // This is the 'page size' for the bucket list. We're unlikely to have
    // more than a thousand versions, but we're querying a prefix and there
    // could easily be a *lot* of objects with the given prefix.
    constexpr int version_query_max = 100;

    ldpp_dout(this, 20) << fmt::format(
        FMT_STRING("issue bucket list() query next_marker={}"),
        params.marker.name)
                        << dendl;
    // NOTE: rgw::sal::RadosBucket::list() updates params.marker as it
    // goes. This isn't how list_multiparts() works.
    auto ret = s->bucket->list(this, params, version_query_max, results, y);

    if (ret < 0) {
      op_ret = ret;
      ldpp_dout(this, 2) << "sal bucket->list query failed ret=" << ret
                         << dendl;
      break;
    }

    if (results.objs.size() == 0) {
      // EOF. Exit the simple search loop.
      ldpp_dout(this, 20) << fmt::format(FMT_STRING("bucket list() prefix='{}' EOF"),
          object_key_name_)
                          << dendl;
      break;

    } else {
      for (size_t n = 0; n < results.objs.size(); n++) {
        auto& obj = results.objs[n];
        // Check for exact key match - we searched a prefix.
        if (obj.key.name != object_key_name_) {
          ldpp_dout(this, 20)
              << fmt::format(FMT_STRING("ignore non-exact match key={}"), obj.key.name)
              << dendl;
          continue;
        }

        ldpp_dout(this, 20)
            << fmt::format(FMT_STRING("obj {}/{}: exists={} current={} delete_marker={}"),
                   n, results.objs.size(), obj.exists, obj.is_current(),
                   obj.is_delete_marker())
            << dendl;
        if (obj.is_current()) {
          found = true;
          object_deleted_ = obj.is_delete_marker();
          if (!object_deleted_) {
            object_size_ = obj.meta.size;
          }
          break;
        }
      }
    }
  } while (!found);

  if (found) {
    ldpp_dout(this, 20) << fmt::format(FMT_STRING("found key={} in standard path"),
        object_key_name_)
                        << dendl;
    op_ret = 0;
    return true;
  }
  return false;
}

/**
 * @brief Query in-progress multipart uploads for our key.
 *
 * Query in-process multipart uploads for an exact match for our key. This can
 * be an expensive index query if there are a lot of in-flight mp uploads.
 *
 * rgw::sal::Bucket::list_multiparts() queries on a prefix (not a full key),
 * so we check for an exact key match each time.
 *
 * Note that op_ret will be set <0 in case failures other than 'not found'.
 * This indicates that we should abort the query process.
 *
 * @param y optional yield object.
 * @return true Success, the object was found. op_ret==0.
 * @return false Failure. If op_ret==0, the object was simply not found. If
 * op_ret<0, an error occurred.
 */
bool RGWStoreQueryOp_ObjectStatus::execute_mpupload_query(optional_yield y)
{
  bool found = false;

  std::vector<std::unique_ptr<rgw::sal::MultipartUpload>> uploads {};
  std::string marker { "" };
  std::string delimiter { "" };
  constexpr int mp_query_max = 100;
  bool is_truncated; // Must be present, pointer to this is unconditionally
                     // written by list_multiparts().

  do {
    // Re-initialise this every run. We can only see if the query is complete
    // across multiple list_multiparts() by checking if this is empty.
    // However, nothing in list_multiparts() clears it.
    uploads.clear();

    ldpp_dout(this, 20) << fmt::format(
        FMT_STRING("issue list_multiparts() query marker='{}'"),
        marker)
                        << dendl;
    // Note that 'marker' is an inout param that we'll need for subsequent
    // queries.
    auto ret = s->bucket->list_multiparts(this, object_key_name_, marker,
        delimiter, mp_query_max, uploads,
        nullptr, &is_truncated);
    if (ret < 0) {
      ldpp_dout(this, 2) << "list_multiparts() failed with code " << ret
                         << dendl;
      op_ret = ret;
      break;
    }

    if (uploads.size() == 0) {
      ldpp_dout(this, 20) << fmt::format(FMT_STRING("list_multiparts() prefix='{}' EOF"),
          object_key_name_)
                          << dendl;
      break;
    }

    for (auto const& upload : uploads) {
      if (upload->get_key() == object_key_name_) {
        object_mpuploading_ = true;
        object_mpupload_id_ = upload->get_upload_id();
        ldpp_dout(this, 20)
            << fmt::format(
                   FMT_STRING("multipart upload found for object={} upload_id='{}'"),
                   upload->get_key(), object_mpupload_id_)
            << dendl;
        found = true;
        break;
      }
    }
  } while (!found);

  if (found) {
    ldpp_dout(this, 20) << fmt::format(FMT_STRING("found key={} in mp upload path"),
        object_key_name_)
                        << dendl;
    op_ret = 0;
    return true;
  }
  return false;
}

void RGWStoreQueryOp_ObjectStatus::execute(optional_yield y)
{
  ceph_assert(s->bucket != nullptr);
  bucket_name_ = rgw_make_bucket_entry_name(s->bucket_tenant, s->bucket_name);
  object_key_name_ = s->object->get_key().name;

  ldpp_dout(this, 20) << fmt::format(FMT_STRING("{}: {} (bucket='{}' object='{}')"),
      typeid(this).name(), __func__,
      bucket_name_, object_key_name_)
                      << dendl;

  // op_ret is used to signal a real failure, meaning we should not continue.
  op_ret = 0;

  if (execute_simple_query(y) || op_ret < 0) {
    return;
  }
  if (execute_mpupload_query(y) || op_ret < 0) {
    return;
  }

  // Not found anywhere.
  ldpp_dout(this, 2) << "key not found" << dendl;
  op_ret = -ENOENT;
  return;
}

void RGWStoreQueryOp_ObjectStatus::send_response_json()
{
  s->formatter->open_object_section("StoreQueryObjectStatusResult");
  s->formatter->open_object_section("Object");
  s->formatter->dump_string("bucket", bucket_name_);
  s->formatter->dump_string("key", object_key_name_);
  s->formatter->dump_bool("deleted", object_deleted_);
  s->formatter->dump_bool("multipart_upload_in_progress", object_mpuploading_);
  if (object_mpuploading_) {
    s->formatter->dump_string("multipart_upload_id", object_mpupload_id_);
  }
  if (!object_deleted_ && !object_mpuploading_) {
    s->formatter->dump_string("version_id", version_id_);
    s->formatter->dump_int("size", static_cast<int64_t>(object_size_));
  }
  s->formatter->close_section();
  s->formatter->close_section();
}

namespace ba = boost::algorithm;

static const char* SQ_HEADER = "HTTP_X_RGW_STOREQUERY";
static const char* HEADER_LC = "x-rgw-storequery";

void RGWSQHeaderParser::reset()
{
  op_ = nullptr;
  command_ = "";
  param_.clear();
}

bool RGWSQHeaderParser::tokenize(const DoutPrefixProvider* dpp,
    const std::string& input)
{
  if (input.empty()) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("illegal empty {} header"), HEADER_LC)
                      << dendl;
    return false;
  }
  if (input.size() > RGWSQMaxHeaderLength) {
    ldpp_dout(dpp, 0) << fmt::format(
        FMT_STRING("{} header exceeds maximum length of {} chars"),
        HEADER_LC, RGWSQMaxHeaderLength)
                      << dendl;
    return false;
  }
  // Enforce ASCII-7 non-control characters.
  if (!std::all_of(input.cbegin(), input.cend(),
          [](auto c) { return c >= ' '; })) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("Illegal character found in {}"), HEADER_LC)
                      << dendl;
    return false;
  }

  // Only debug the header contents after canonicalising it.
  ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("header {}: '{}'"), HEADER_LC, input)
                     << dendl;

  try {
    // Use boost::tokenizer to split into space-separated fields, allowing
    // double-quoted fields to contain spaces.
    boost::escaped_list_separator<char> els("\\", " ", "\"");
    boost::tokenizer<boost::escaped_list_separator<char>> tok { input, els };
    bool first = true;
    for (const auto& t : tok) {
      if (first) {
        // Always lowercase the command name.
        command_ = ba::to_lower_copy(t);
        first = false;
        continue;
      }
      param_.push_back(std::string { t });
    }
    return true;
  } catch (const boost::escaped_list_error& e) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("Failed to parse storequery header: {}"), e.what()) << dendl;
    return false;
  }
}

bool RGWSQHeaderParser::parse(const DoutPrefixProvider* dpp,
    const std::string& input,
    RGWSQHandlerType handler_type)
{
  op_ = nullptr;
  if (!tokenize(dpp, input)) {
    return false;
  }
  if (command_.empty()) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("{}: no command found"), HEADER_LC)
                      << dendl;
    return false;
  }
  // ObjectStatus command.
  //
  if (command_ == "objectstatus") {
    if (handler_type != RGWSQHandlerType::Obj) {
      ldpp_dout(dpp, 0)
          << fmt::format(FMT_STRING("{}: ObjectStatus only applies in an Object context"),
                 HEADER_LC)
          << dendl;
      return false;
    }
    if (param_.size() != 0) {
      ldpp_dout(dpp, 0)
          << fmt::format(
                 "{}: malformed ObjectStatus command (expected zero args)",
                 HEADER_LC)
          << dendl;
      return false;
    }
    // The naked new is part of the interface.
    op_ = new RGWStoreQueryOp_ObjectStatus();
    return true;
  }
  // Ping command.
  //
  else if (command_ == "ping") {
    // Allow ping from any handler type - it doesn't matter!
    if (param_.size() != 1) {
      ldpp_dout(dpp, 0) << fmt::format(
          "{}: malformed Ping command (expected one arg)",
          HEADER_LC)
                        << dendl;
      return false;
    }
    // The naked new is part of the interface.
    op_ = new RGWStoreQueryOp_Ping(param_[0]);
    return true;
  }
  return false;
}

bool RGWHandler_REST_StoreQuery_S3::is_storequery_request(const req_state* s)
{
  if (s->op != OP_GET) {
    return false;
  }
  auto hdr = s->info.env->get(SQ_HEADER, nullptr);
  return (hdr != nullptr);
}

RGWOp* RGWHandler_REST_StoreQuery_S3::op_get()
{
  // Handler selection (RGWHandler_REST_S3::get_handler()) checks for the
  // storequery header. If we get here without that header, something is
  // wrong.
  ceph_assert(is_storequery_request(s));

  auto hdr = s->info.env->get(SQ_HEADER, nullptr);
  DoutPrefix dpp { g_ceph_context, ceph_subsys_rgw, "storequery_parse " };

  // If we fail to parse now, we return nullptr to indicate 'method not
  // allowed'. The logs will contain an explanation, but there's no way (using
  // op_get(), an override) to influence the output returned to the user
  // without much more substantial code changes.
  auto p = RGWSQHeaderParser();
  if (!p.parse(&dpp, hdr, handler_type_)) {
    ldpp_dout(&dpp, 0) << fmt::format(FMT_STRING("{}: parser failure"), HEADER_LC) << dendl;
    return nullptr;
  }
  p.op()->init(driver, s, this);
  return p.op();
}

RGWOp* RGWHandler_REST_StoreQuery_S3::op_put()
{
  // We don't handle PUT requests yet.
  return nullptr;
}
RGWOp* RGWHandler_REST_StoreQuery_S3::op_delete()
{
  // We don't handle DELETE requests yet.
  return nullptr;
}

int RGWHandler_REST_StoreQuery_S3::init_permissions(RGWOp* op, optional_yield y)
{
  ldpp_dout(op, 20) << "init_permissions()" << dendl;
  const auto dpp = op;

  int ret = 0;

  if (handler_type_ == RGWSQHandlerType::Service) {
    // Service handlers don't care about buckets or objects.
    return ret;
  }

  // This is essentially copied (somewhat truncated) from
  // RGWHandler::rgw_build_bucket_policies(). When changing Ceph versions, it
  // would be smart to check if the code still matches.
  if (!s->bucket_name.empty()) {
    s->bucket_exists = true;

    /* This is the only place that s->bucket is created.  It should never be
     * overwritten. */
    ret = driver->get_bucket(dpp, s->user.get(), rgw_bucket(s->bucket_tenant, s->bucket_name, s->bucket_instance_id), &s->bucket, y);
    if (ret < 0) {
      if (ret != -ENOENT) {
        string bucket_log;
        bucket_log = rgw_make_bucket_entry_name(s->bucket_tenant, s->bucket_name);
        ldpp_dout(dpp, 0) << "NOTICE: couldn't get bucket from bucket_name (name="
                          << bucket_log << ")" << dendl;
        return ret;
      }
      s->bucket_exists = false;
      return -ERR_NO_SUCH_BUCKET;
    }

    s->bucket_mtime = s->bucket->get_modification_time();
    s->bucket_attrs = s->bucket->get_attrs();

    // There's no need to load an object if we're a bucket-type handler.
    if (handler_type_ != RGWSQHandlerType::Obj) {
      return ret;
    }
    if (!rgw::sal::Object::empty(s->object.get())) {
      s->object->set_bucket(s->bucket.get());
    }
  }
  return ret;
}

} // namespace rgw
