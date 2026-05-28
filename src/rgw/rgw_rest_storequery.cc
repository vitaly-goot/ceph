// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <absl/strings/numbers.h>
#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <boost/token_functions.hpp>
#include <boost/tokenizer.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <rapidjson/document.h>
#include <stdexcept>
#include <string>

#include "cls/rgw/cls_rgw_types.h"
#include "common/async/yield_context.h"
#include "common/dout.h"
#include "rgw_b64.h"
#include "rgw_common.h"
#include "rgw_op.h"
#include "rgw_rest_storequery.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

namespace rgw {

/***************************************************************************/

// RGWStoreQueryOp_Base

void RGWStoreQueryOp_Base::send_response_pre()
{
  if (op_ret) {
    set_req_state_err(s, op_ret);
  }
  auto ret = RGWHandler_REST::reallocate_formatter(s, RGWFormat::JSON);
  if (ret != 0) {
    ldpp_dout(this, 0) << "failed to set formatter to JSON" << dendl;
    set_req_state_err(s, -EINVAL);
  }
  dump_errno(s);
  // override end_header() defaults: set force_no_error = true when ENOENT
  // this disables standard NoSuchKey s3 error message, to use our own.
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

/***************************************************************************/

// RGWStoreQueryOp_Ping

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

/***************************************************************************/

// RGWStoreQueryOp_ObjectStatus

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
      ldpp_dout(this, 0) << "sal bucket->list query failed ret=" << ret
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
      ldpp_dout(this, 0) << "list_multiparts() failed with code " << ret
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
  ldpp_dout(this, 0) << "key not found" << dendl;
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
  s->formatter->close_section(); // Object
  s->formatter->close_section(); // StoreQueryObjectStatusResult
}

/***************************************************************************/

// RGWStoreQueryOp_ObjectList

std::string RGWStoreQueryOp_ObjectList::create_continuation_token(const DoutPrefixProvider* dpp, rgw_obj_key& key)
{
  bufferlist bl;
  ceph::JSONFormatter f;
  f.open_object_section("ContinuationToken");
  f.dump_string("key", key.name);
  f.dump_string("instance", key.instance);
  f.close_section(); // ContinuationToken
  f.flush(bl);
  std::string token = bl.to_str();
  return token;
}

std::optional<rgw_obj_key> RGWStoreQueryOp_ObjectList::read_continuation_token(const DoutPrefixProvider* dpp, const std::string& token)
{
  if (token[0] != '{') {
    // Assume we're given just a name, not a JSON object containing the
    // instance.
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("unpacked legacy continuation token: key='{}', no instance available"), token)
                       << dendl;
    return rgw_obj_key(token);
  }
  rapidjson::Document doc;
  doc.Parse(token.c_str());
  if (doc.HasParseError()) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("failed to parse continuation token: {}"), rapidjson::GetParseError_En(doc.GetParseError()))
                      << dendl;
    return std::nullopt;
  }
  if (!doc.IsObject() || !doc.HasMember("key") || !doc["key"].IsString() || !doc.HasMember("instance") || !doc["instance"].IsString()) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("invalid continuation token format: {}"), token)
                      << dendl;
    return std::nullopt;
  }
  rgw_obj_key key(doc["key"].GetString(),
      doc["instance"].GetString());
  ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("unpacked continuation token: key='{}' instance='{}'"),
      key.name, key.instance)
                     << dendl;
  return key;
}

bool RGWStoreQueryOp_ObjectList::execute_query(optional_yield y)
{
  // The ListParams persists across multiple requests.
  rgw::sal::Bucket::ListParams params {};

  // Fill in the contination token if we need to.
  if (marker_.has_value()) {
    std::string init_marker;
    try {
      init_marker = from_base64(*marker_);
    } catch (std::exception& e) {
      // We can't catch boost::archive::archive_exception specifically, it
      // doesn't link and I'm not fixing the CMake just for one exception.
      ldpp_dout(this, 0) << fmt::format(FMT_STRING("failed to decode continuation token: '{}'"), e.what())
                         << dendl;
      op_ret = -EINVAL;
      return false;
    }
    std::optional<rgw_obj_key> marker_key = read_continuation_token(this, init_marker);
    if (!marker_key) {
      ldpp_dout(this, 0) << fmt::format(FMT_STRING("failed to unpack continuation token: '{}'"), init_marker)
                         << dendl;
      op_ret = -EINVAL;
      return false;
    }
    ldpp_dout(this, 10) << fmt::format(FMT_STRING("continuation token '{}' decoded as {}"), *marker_, init_marker)
                        << dendl;
    params.marker = *marker_key;
  }

  // No prefix for a complete list of the bucket.
  params.prefix = "";
  // We want results even if the last object is a delete marker. In a bucket
  // without versioning a query for a deleted or nonexistent object will
  // return zero objects, for which we'll return ENOENT.
  params.list_versions = true;
  // It appears pagination works fine with unordered queries.
  params.allow_unordered = g_conf()->rgw_storequery_objectlist_sort ? false : true;

  // Cap the number of entries we'll return to our LIST_QUERY_SIZE_HARD_LIMIT.
  // We can experiment with this in a lab, but in production let's make sure
  // we don't overtax the system.
  uint64_t query_max = std::min(max_entries_, LIST_QUERY_SIZE_HARD_LIMIT);
  if (query_max < max_entries_) {
    ldpp_dout(this, 5) << fmt::format(FMT_STRING("max_entries {} is above the hard limit, restricting query_max to {}"), max_entries_, query_max)
                       << dendl;
  }

  seen_eof_ = false; // Reset this for the next query.
  bool early_exit = false; // True if we ran out of space in the user list before we ran out of objects.

  rgw_obj_key next_marker;

  // Reserve space for the maximum number of entries we might return. This is
  // a compromise - we could reallocate as we issue queries against the
  // backend, but this will lead to heap churn and copies. This feels like the
  // proper balance to me; reserve enough space for the maximum number of
  // items we're going to return (which we don't in any case allow to be
  // /insanely/ high), and reserve it exactly once.
  items_.reserve(max_entries_);

  stats_.entries_max = max_entries_;

  // Loop until we've filled the user's requested number of entries, or we hit
  // EOF (i.e. the result of s->bucket->list() was truncated).
  while (!seen_eof_ && items_.size() < max_entries_) {
    rgw::sal::Bucket::ListResults results;

    ldpp_dout(this, 20) << fmt::format(
        FMT_STRING("issue bucket list() query query_max={} next=[marker={}, instance={}]"),
        query_max, params.marker.name, params.marker.instance)
                        << dendl;

    // Note that rgw::sal::RadosBucket::list() updates params.marker as it
    // goes. This isn't how list_multiparts() works, don't get caught.

    int ret;
    if (list_function_) {
      // TESTING ONLY! Use the overridden list function if it exists.
      ret = (*list_function_)(this, params, query_max, results, y);
    } else {
      // Use the default bucket list function.
      ret = s->bucket->list(this, params, query_max, results, y);
    }
    stats_.sal_queries++;

    // Clear next_marker. It's used outside the loop for the continuation
    // token, and not for SAL queries. We'll only set it if we decide it's
    // necessary to expose to the caller.
    next_marker = rgw_obj_key("");

    if (ret < 0) {
      op_ret = ret;
      ldpp_dout(this, 0) << "SAL bucket->list() query failed ret=" << ret
                         << dendl;
      break;
    }

    ldpp_dout(this, 20) << fmt::format(FMT_STRING("SAL bucket->list() query returned {} objects, is_truncated={}, next_marker[name={}, instance={}]"),
        results.objs.size(), results.is_truncated, results.next_marker.name, results.next_marker.instance)
                        << dendl;

    // If the SAL indicates there are no more results (i.e. 'not truncated')
    // we can stop querying for this objectlist command instance. Note that
    // the user might end up issuing additional queries, as it's possible to
    // fill the user's items_ array before we copy all the results out.
    //
    // It would be simpler if we were to allow the items_ array to be larger
    // than the user requested, but we'll follow the robustness principle and
    // not return more results than we were asked for.
    //
    // seen_eof is easier to understand than '!is_truncated', to my brain
    // anyway.

    seen_eof_ = !results.is_truncated;

    // Loop over the results of s->bucket->list() until we either fill the
    // user's maximum request size or get to the end of the query results.
    for (size_t n = 0; n < results.objs.size(); n++) {
      bool last_item = (n == results.objs.size() - 1);
      stats_.sal_seen++;

      auto& obj = results.objs[n];
      ldpp_dout(this, 20)
          << fmt::format(FMT_STRING("SAL result obj {}/{}: key='{}' instance='{}' exists={} current={} delete_marker={}"),
                 n + 1, results.objs.size(), obj.key.name, obj.key.instance, obj.exists, obj.is_current(),
                 obj.is_delete_marker())
          << dendl;

      if (obj.exists) {
        stats_.sal_exists++;
      }
      // We're only really interested in the current (most recent) version of
      // the object.
      if (!obj.is_current()) {
        stats_.sal_not_current++;

      } else {
        stats_.sal_current++;
        item_type item { obj.key.name };
        item.set_deleted(obj.is_delete_marker());
        if (obj.is_delete_marker()) {
          stats_.sal_deleted++;
        } else {
          // Only non-deleted items should have a size.
          item.set_size(obj.meta.size);
        }
        // This is the *only* place we add to the items_ array.
        items_.push_back(item);
        ldpp_dout(this, 20)
            << fmt::format(FMT_STRING("added user result item {}: key='{}'"), items_.size(), item.key())
            << dendl;
        stats_.entries_actual++;

        // If we've filled the user's requested number of entries, we need to
        // set the marker properly and exit the item loop.
        if (items_.size() >= max_entries_) {
          if (last_item && seen_eof_) {
            // In the special case where we've filled the user structure with
            // the last item in the query, AND we've seen EOF, then we don't
            // need to set a marker.
            ldpp_dout(this, 20) << fmt::format(FMT_STRING("filled user items array and at EOF, no marker set"))
                                << dendl;
          } else {
            // Set the marker to exactly where we left off.
            next_marker = rgw_obj_key(results.objs[n].key.name, results.objs[n].key.instance);
            early_exit = true;
            ldpp_dout(this, 20) << fmt::format(FMT_STRING("filled user items array, next_marker=[name={},instance={}]"),
                next_marker.name, next_marker.instance)
                                << dendl;
          }
          break; // We'll also fail the outer while loop's condition.
        }
      }

    } // for each SAL object result

  } // while !seen_eof && items_.size() < max_entries_

  ldpp_dout(this, 10) << fmt::format(FMT_STRING("loop exit: seen_eof={} early_exit={} next_marker=[name={},instance={}]"),
      seen_eof_, early_exit, next_marker.name, next_marker.instance)
                      << dendl;

  // s->bucket->list() can fail. We rely on op_ret being properly set at the
  // point of failure.
  if (op_ret < 0) {
    return false;
  }

  if (!next_marker.empty()) {
    // If there are more results, we need to safely encode the continuation
    // marker and return it to the user. This is done by setting
    // return_marker_, which will be dumped in send_response_json().
    //
    // Note that it's safe to use to_base64() here. Even though it looks like it
    // will insert line breaks, it's actually a template and the default line
    // wrap width is std::numeric_limits<int>::max().

    std::string marker_str = RGWStoreQueryOp_ObjectList::create_continuation_token(this, next_marker);
    std::string encoded_marker;
    try {
      encoded_marker = to_base64(marker_str);
    } catch (std::runtime_error& e) {
      ldpp_dout(this, 0) << fmt::format(FMT_STRING("failed to encode continuation token: '{}'"), e.what())
                         << dendl;
      op_ret = -EINVAL;
      return false;
    }
    while (encoded_marker.length() % 4 != 0) {
      // Pad the encoded marker with '=' characters to make it a multiple of
      // 4. Our parser code expects valid base64.
      encoded_marker += '=';
    }
    // Show the un-base64'd marker at l20. Show the base64'd marker at l5.
    ldpp_dout(this, 20) << fmt::format(FMT_STRING("EOF not reached, next_marker {}"), marker_str)
                        << dendl;
    ldpp_dout(this, 5) << fmt::format(FMT_STRING("EOF not reached, continuation token {}"), encoded_marker)
                       << dendl;
    set_return_marker(encoded_marker);
  }

  return true;
}

void RGWStoreQueryOp_ObjectList::execute(optional_yield y)
{
  if (!execute_query(y)) {
    // rely on execute_query() setting op_ret appropriately.
    ldpp_dout(this, 0) << "execute_query() failed" << dendl;
  }
}

void RGWStoreQueryOp_ObjectList::send_response_json()
{
  auto f = s->formatter;
  f->open_object_section("StoreQueryObjectListResult");

  f->open_array_section("Objects");
  for (const auto& item : items_) {
    item.dump(f);
  }
  f->close_section(); // Objects

  f->open_object_section("Stats");
  stats_.dump(f);
  f->close_section(); // Stats

  if (return_marker_.has_value()) {
    f->dump_string("NextToken", *return_marker_);
  }
  f->close_section(); // StoreQueryObjectListResult
}

void RGWStoreQueryOp_ObjectList::Item::dump(Formatter* f) const
{
  f->open_object_section("Object");
  f->dump_string("key", to_base64(key_));
  // Only dump optional attributes if they've been given values.
  if (is_deleted_.has_value() && *is_deleted_) {
    // We only dump the attribute if it's set and true.
    f->dump_bool("deleted", *is_deleted_);
  }
  if (size_.has_value()) {
    // Size of zero is a value value.
    f->dump_unsigned("size", *size_);
  }
  f->close_section();
}

/***************************************************************************/

// RGWStoreQueryOp_MPUploadList

std::string RGWStoreQueryOp_MPUploadList::create_continuation_token(const DoutPrefixProvider* dpp, std::unique_ptr<rgw::sal::MultipartUpload> const& upload)
{
  bufferlist bl;
  ceph::JSONFormatter f;
  f.open_object_section("ContinuationToken");
  // get_meta() returns the 'name of the object representing this upload in
  // the backing store', which we use as the actual pagination marker.
  f.dump_string("marker", upload->get_meta());
  // These are convenience attributes, since we have them.
  f.dump_string("key", upload->get_key());
  f.dump_string("uploadId", upload->get_upload_id());
  f.close_section(); // ContinuationToken
  f.flush(bl);
  std::string token = bl.to_str();
  return token;
}

std::optional<std::string> RGWStoreQueryOp_MPUploadList::read_continuation_token(const DoutPrefixProvider* dpp, const std::string& token)
{
  if (token[0] != '{') {
    // Assume we're given just a name, not a JSON object containing the
    // instance.
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("unpacked legacy continuation token: marker='{}'"), token)
                       << dendl;
    return token;
  }
  rapidjson::Document doc;
  doc.Parse(token.c_str());
  if (doc.HasParseError()) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("failed to parse continuation token: {}"), rapidjson::GetParseError_En(doc.GetParseError()))
                      << dendl;
    return std::nullopt;
  }
  if (!doc.IsObject() || !doc.HasMember("marker") || !doc["marker"].IsString()
      || !doc.HasMember("key") || !doc["key"].IsString()
      || !doc.HasMember("uploadId") || !doc["uploadId"].IsString()) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("invalid continuation token format: {}"), token)
                      << dendl;
    return std::nullopt;
  }
  std::string marker = doc["marker"].GetString();
  ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("unpacked continuation token: marker='{}'"), marker)
                     << dendl;
  return marker;
}

bool RGWStoreQueryOp_MPUploadList::execute_query(optional_yield y)
{
  std::vector<std::unique_ptr<rgw::sal::MultipartUpload>> uploads {};

  std::string marker;
  unset_return_marker(); // We'll set the continuation token iff it's needed.

  // If present, extract the marker string from the JSON continuation token.
  if (marker_.has_value()) {
    std::string marker_json;
    try {
      marker_json = from_base64(*marker_);
      ldpp_dout(this, 10) << fmt::format(FMT_STRING("continuation token '{}' decoded as {}"), *marker_, marker_json)
                          << dendl;
    } catch (std::exception& e) {
      // We can't catch boost::archive::archive_exception specifically, it
      // doesn't link and I'm not fixing the CMake just for one exception.
      ldpp_dout(this, 0) << fmt::format(FMT_STRING("failed to decode continuation token: '{}'"), e.what())
                         << dendl;
      op_ret = -EINVAL;
      return false;
    }
    std::optional<std::string> opt_marker = read_continuation_token(this, marker_json);
    if (!opt_marker.has_value()) {
      ldpp_dout(this, 0) << fmt::format(FMT_STRING("failed to read continuation token from JSON: {}"), marker_json)
                         << dendl;
      op_ret = -EINVAL;
      return false;
    }
    ldpp_dout(this, 10) << fmt::format(FMT_STRING("continuation token '{}' decoded as marker='{}'"), *marker_, *opt_marker)
                        << dendl;
    marker = *opt_marker;
  }

  bool is_truncated; // Must be present, pointer to this is unconditionally
                     // written by list_multiparts().

  uint64_t query_max = std::min(max_entries_, LIST_MULTIPARTS_QUERY_SIZE_HARD_LIMIT);
  if (query_max < max_entries_) {
    ldpp_dout(this, 5) << fmt::format(FMT_STRING("max_entries {} is above the hard limit, restricting query_max to {}"), max_entries_, query_max)
                       << dendl;
  }

  seen_eof_ = false;

  // This will be set if we want to set a continuation token.
  std::optional<std::string> opt_ret_marker;

  // Reserve space for the maximum number of entries we might return. This is
  // a compromise - we could reallocate as we issue queries against the
  // backend, but this will lead to heap churn and copies. This feels like the
  // proper balance to me; reserve enough space for the maximum number of
  // items we're going to return (which we don't in any case allow to be
  // /insanely/ high), and reserve it exactly once.
  items_.reserve(max_entries_);

  while (!seen_eof_ && items_.size() < max_entries_) {
    // Re-initialise this every run.
    uploads.clear();

    ldpp_dout(this, 20) << fmt::format(
        FMT_STRING("issue list_multiparts() query query_max={} marker={}"), query_max, marker)
                        << dendl;

    // rgw::sal::Bucket::list_multiparts() notes:
    //
    // - marker is an inout parameter that we need for pagination. Note this
    //   isn't the one we return to the storequery caller, we use it to
    //   paginate through the SAL results, and then if we need to return a
    //   continuation token to the user we create that ourselves using the SAL
    //   result where we left off. The RGWMultipartUpload object returns a
    //   get_meta() function that according to the documentation represents
    //   that upload in the backing store. This is what we pass to
    //   list_multiparts() to get proper pagination.
    //
    // - is_truncated must not be a null pointer, the underlying
    //   implementation doesn't do a nullptr check, at least not in v18.
    //
    // - Don't make any assumptions about how many records will be returned,
    //   except that it will be <= query_max.

    int ret;
    if (list_multiparts_function_) {
      ret = (*list_multiparts_function_)(this, "", marker, "", query_max, uploads, nullptr, &is_truncated);
    } else {
      ret = s->bucket->list_multiparts(this, "", marker, "", query_max, uploads, nullptr, &is_truncated);
    }

    if (ret < 0) {
      ldpp_dout(this, 0) << "list_multiparts() failed with code " << ret
                         << dendl;
      op_ret = ret;
      break;
    }

    ldpp_dout(this, 20) << fmt::format(FMT_STRING("SAL list_multiparts() uploads.size()={} is_truncated={}"), uploads.size(), is_truncated)
                        << dendl;

    // As with objectlist, I find '!is_truncated' confusing to reason with,
    // whereas a bool 'seen_eof' is more intuitive.
    seen_eof_ = !is_truncated;

    // for (auto const& upload : uploads) {
    for (size_t n = 0; n < uploads.size(); n++) {
      bool last_item = (n == uploads.size() - 1);

      auto& upload = uploads[n];

      auto& key = upload->get_key();
      auto& id = upload->get_upload_id();

      ldpp_dout(this, 20)
          << fmt::format(FMT_STRING("obj: key={} upload_id={}"), key, id)
          << dendl;

      item_type item(key, id);

      // This is the *only* place we add to the items_ array.
      items_.push_back(item);
      ldpp_dout(this, 20) << fmt::format(FMT_STRING("added user result item {}: key='{}' upload_id='{}'"), items_.size(), item.key(), item.upload_id())
                          << dendl;

      // Extra actions if we've reached the caller's size limit.
      if (items_.size() == max_entries_) {
        if (last_item && seen_eof_) {
          // In the special case where we've filled the user structure with
          // the last item in the query, AND we've seen EOF, then we don't
          // need to set a marker.
          ldpp_dout(this, 20) << fmt::format(FMT_STRING("filled user items array and at EOF, no marker set"))
                              << dendl;
        } else {
          // We filled the user items list but there are more entries. Set the
          // marker to where we left off (NOT where the SAL query left off)
          // and exit the loop.
          opt_ret_marker = create_continuation_token(this, upload);
          ldpp_dout(this, 10) << fmt::format(FMT_STRING("max_entries reached, next={}"), *opt_ret_marker)
              << dendl;
          break;
        }
      }

    } // for each upload result
  } // while !seen_eof && items_.size() < max_entries_

  ldpp_dout(this, 10) << fmt::format(FMT_STRING("loop exit: seen_eof={} next_marker={} items_.size={}"),
                                     seen_eof_, opt_ret_marker.has_value() ? *opt_ret_marker : "<none>", items_.size())
                      << dendl;

  if (op_ret < 0) {
    return false;
  }

  if (opt_ret_marker.has_value()) {
    // If there are more results, we need to safely encode the continuation
    // marker and return it to the user. This is done by setting
    // return_marker_, which will be dumped in send_response_json().
    //
    // Note that it's safe to use to_base64() here. Even though it looks like it
    // will insert line breaks, it's actually a template and the default line
    // wrap width is std::numeric_limits<int>::max().
    std::string encoded_marker;
    try {
      encoded_marker = to_base64(*opt_ret_marker);
    } catch (std::exception& e) {
      ldpp_dout(this, 0) << fmt::format(FMT_STRING("failed to encode continuation token: '{}'"), e.what())
                         << dendl;
      op_ret = -EINVAL;
      return false;
    }
    // Level 20 shows the raw marker.
    ldpp_dout(this, 20) << fmt::format(FMT_STRING("EOF not reached, next_marker {}"), *opt_ret_marker)
                        << dendl;
    // Level 5 shows the base64'd marker that we return to the user.
    ldpp_dout(this, 5) << fmt::format(FMT_STRING("EOF not reached, continuation token {}"), encoded_marker)
                       << dendl;
    set_return_marker(encoded_marker);
  }

  return true;
}

void RGWStoreQueryOp_MPUploadList::execute(optional_yield y)
{
  if (!execute_query(y)) {
    // rely on execute_query() setting op_ret appropriately.
    ldpp_dout(this, 0) << "execute_query() failed" << dendl;
  }
}

void RGWStoreQueryOp_MPUploadList::send_response_json()
{
  auto f = s->formatter;
  f->open_object_section("StoreQueryMPUploadListResult");
  f->open_array_section("Objects");
  for (const auto& item : items_) {
    item.dump(f);
  }
  f->close_section(); // Objects
  if (return_marker_.has_value()) {
    f->dump_string("NextToken", *return_marker_);
  }
  f->close_section(); // StoreQueryMPUploadListResult
}

void RGWStoreQueryOp_MPUploadList::Item::dump(Formatter* f) const
{
  f->open_object_section("Object");
  f->dump_string("key", to_base64(key_));
  f->dump_string("upload_id", to_base64(upload_id_));
  f->close_section(); // Object
}

/***************************************************************************/

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
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("illegal character found in {}"), HEADER_LC)
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
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("failed to parse storequery header: {}"), e.what()) << dendl;
    return false;
  }
}

bool RGWSQHeaderParser::valid_base64(const DoutPrefixProvider* dpp, const std::string& input)
{
  // This is quite fussy, but we should always output valid base64 with proper
  // padding, so it's not unreasonable to expect the same back.
  if (input.size() % 4 != 0) {
    ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("input length {} is not a multiple of 4"), input.size()) << dendl;
    return false;
  }
  // Using std::all() would prevent us giving specific diagnostics.
  for (size_t n = 0; n < input.size(); n++) {
    char c = input[n];
    if (!isalnum(c) && c != '+' && c != '/' && c != '=') {
      // It's safe to output the invalid character, we've already filtered the
      // input to printable ASCII-7.
      ldpp_dout(dpp, 0) << fmt::format(FMT_STRING("invalid base64 character '{}' at index {} of input='{}'"), c, n, input) << dendl;
      return false;
    }
  }
  return true;
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
  if (command_ == "objectstatus") {
    // ObjectStatus command.
    //
    if (handler_type != RGWSQHandlerType::Obj) {
      ldpp_dout(dpp, 0)
          << fmt::format(FMT_STRING("{}: objectstatus only applies in an Object context"),
                 HEADER_LC)
          << dendl;
      return false;
    }
    if (param_.size() != 0) {
      ldpp_dout(dpp, 0)
          << fmt::format(
                 "{}: malformed objectstatus command (expected zero args)",
                 HEADER_LC)
          << dendl;
      return false;
    }
    // The naked new is part of the interface.
    op_ = new RGWStoreQueryOp_ObjectStatus();
    return true;

  } else if (command_ == "objectlist") {
    // ObjectList command.
    //
    if (handler_type != RGWSQHandlerType::Bucket) {
      ldpp_dout(dpp, 0)
          << fmt::format(FMT_STRING("{}: objectlist only applies in a Bucket context"),
                 HEADER_LC)
          << dendl;
      return false;
    }
    if (param_.size() < 1 || param_.size() > 2) {
      ldpp_dout(dpp, 0)
          << fmt::format(
                 "{}: malformed objectlist command (expected one or two args)",
                 HEADER_LC)
          << dendl;
      return false;
    }
    uint64_t max_entries;
    if (!absl::SimpleAtoi(param_[0], &max_entries)) {
      ldpp_dout(dpp, 0)
          << fmt::format(FMT_STRING("{}: malformed ObjectList command (expected integer in first parameter)"),
                 HEADER_LC)
          << dendl;
      return false;
    }
    if (param_.size() >= 2 && !valid_base64(dpp, param_[1])) {
      ldpp_dout(dpp, 0)
          << fmt::format(FMT_STRING("{}: malformed objectlist command (invalid base64 in second parameter)"),
                 HEADER_LC)
          << dendl;
      return false;
    }

    std::optional<std::string> marker;
    if (param_.size() == 2) {
      marker = param_[1];
    }
    // The naked new is part of the interface.
    op_ = new RGWStoreQueryOp_ObjectList(max_entries, marker);
    return true;

  } else if (command_ == "mpuploadlist") {
    // mpuploadlist command.
    //
    if (handler_type != RGWSQHandlerType::Bucket) {
      ldpp_dout(dpp, 0)
          << fmt::format(FMT_STRING("{}: mpuploadlist only applies in a Bucket context"),
                 HEADER_LC)
          << dendl;
      return false;
    }
    if (param_.size() < 1 || param_.size() > 2) {
      ldpp_dout(dpp, 0)
          << fmt::format(
                 "{}: malformed mpuploadlist command (expected one or two args)",
                 HEADER_LC)
          << dendl;
      return false;
    }
    uint64_t max_entries;
    if (!absl::SimpleAtoi(param_[0], &max_entries)) {
      ldpp_dout(dpp, 0)
          << fmt::format(FMT_STRING("{}: malformed mpuploadlist command (expected integer in first parameter)"),
                 HEADER_LC)
          << dendl;
      return false;
    }
    if (param_.size() >= 2 && !valid_base64(dpp, param_[1])) {
      ldpp_dout(dpp, 0)
          << fmt::format(FMT_STRING("{}: malformed mpuploadlist command (invalid base64 in second parameter)"),
                 HEADER_LC)
          << dendl;
      return false;
    }

    std::optional<std::string> marker;
    if (param_.size() == 2) {
      marker = param_[1];
    }
    // The naked new is part of the interface.
    op_ = new RGWStoreQueryOp_MPUploadList(max_entries, marker);
    return true;

  } else if (command_ == "ping") {
    // Ping command.
    //
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

void storequery_encode_and_dump_key(Formatter* f, const std::string& key, const std::string& fieldname)
{
  f->dump_string(fieldname, rgw::to_base64(key));
}

} // namespace rgw
