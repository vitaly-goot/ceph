// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
#pragma once

#include <fmt/printf.h>
#include <memory>
#include <sys/types.h>
#include <unistd.h>

#include "common/dout.h"
#include "rgw_op.h"
#include "rgw_rest_s3.h"

namespace rgw {

/**
 * @brief The type of S3 request for which the StoreQuery handler was invoked.
 *
 * Declare rather than infer the mode from which the handler is called.
 * Certain commands only make sense from certain modes - there's no point
 * querying an object if we're invoked by the RGWHandler_REST_Service_S3 - we
 * don't have enough information to query an object.
 */
enum class RGWSQHandlerType { Service,
  Bucket,
  Obj };

/**
 * @brief Handler for StoreQuery REST commands (we only support S3).
 *
 * This handler requires the presence of the HTTP header x-rgw-storequery,
 * with specifically-formatted contents.
 *
 * This handler is created by RGWHandler_REST_Service_S3,
 * RGWHandler_REST_Bucket_S3 and RGWHandler_REST_Obj_s3. Currently only
 * Service (for Ping) and Obj (for ObjectStatus) are in use.
 *
 * Parsing of the `x-rgw-storequery` header is delegated to class
 * RGWSQHeaderParser and the header's format is documented therein.
 *
 */
class RGWHandler_REST_StoreQuery_S3 : public RGWHandler_REST_S3 {
private:
  const RGWSQHandlerType handler_type_;

protected:
  /**
   * @brief Duplicate RGWHandler_REST::init_permissions() processing so the
   * operations can continue.
   *
   * Interestingly, RGWHandler_REST::init_permissions() loads the bucket! It's
   * called from process_request() via rgw_process_authenticated(), and the
   * default handler does all sorts of policy handing and verification before
   * loading the bucket.
   *
   * The actual load for the regular REST handler is done in
   * RGWHandler_REST::do_init_permissions(), and thence to
   * RGWHandler::rgw_build_bucket_policies(). We're going to replicate chunks
   * of rgw_build_bucket_policies() to load the bucket. We can't call it; the
   * whole point of storequery is that we _don't_ respect things like
   * policies, and that function explicitly checks the policy.
   *
   * We can skip the policies etc., but we still have to verify the bucket
   * name and (if appropriate for the handler type) load the object.
   *
   * Do the minimum possible. For example, if we're a Service type query, we
   * don't need to load the bucket.
   *
   * @param op The operation.
   * @param y Optional yield.
   * @return int, zero means success, otherwise an error code, e.g.
   * ERR_NO_SUCH_BUCKET.
   */
  int init_permissions(RGWOp* op, optional_yield y) override;

  /**
   * @brief Null override of read_permissions.
   *
   * @param op The operation.
   * @param y Optional yield.
   * @return int, zero means success.
   */
  int read_permissions(RGWOp* op, optional_yield y) override { return 0; }

  /**
   * @brief Trivial override of supports_quota(). Spoiler: We don't.
   *
   * @return false Always returns false.
   */
  bool supports_quota() override { return false; }

  /**
   * @brief Determine if a StoreQuery GET operation is being requested.
   *
   * If the x-rgw-storequery HTTP header is absent, assert - we should have
   * checked for this in the REST handler.
   *
   * If the x- header is present but its contents fail to parse, return
   * nullptr to stop further processing of the request. This is the only thing
   * we can return to the API, a richer error interface would require
   * significant code changes.
   *
   * On success, return an object of the appropriate RGWOp subclass to handle
   * the request. As required by the interface, the object is allocated with
   * \p new and is freed by a call to handler->put_op() in process_request().
   *
   * @return RGWOp* nullptr on error, otherwise an RGWOp object to process the
   * operation.
   */
  RGWOp* op_get() override;

  /**
   * @brief No-op - we don't handle PUT requests yet.
   *
   * @return RGWOp* nullptr.
   */
  RGWOp* op_put() override;

  /**
   * @brief No-op - we don't handle DELETE requests yet.
   *
   * @return RGWOp* nullptr.
   */
  RGWOp* op_delete() override;

public:
  using RGWHandler_REST_S3::RGWHandler_REST_S3;
  RGWHandler_REST_StoreQuery_S3(
      const rgw::auth::StrategyRegistry& auth_registry,
      RGWSQHandlerType handler_type)
      : RGWHandler_REST_S3(auth_registry)
      , handler_type_ { handler_type }
  {
  }

  /**
   * @brief Given a req_state, determine if this is a storequery request.
   *
   * Note this is static - it's intended to be called from
   * RGWRESTMgr_S3::get_handler() before any objects are created.
   *
   * If the request is not OP_GET, return false. (Of course, we'll need to
   * change this if we ever support other than OP_GET.)
   *
   * Otherwise, if the storequery HTTP header is present, return true.
   *
   * @param s The request state
   * @return true This is a storequery request.
   * @return false This is not a storequery request.
   */
  static bool is_storequery_request(const req_state* s);
};

/// The longest supported value for the x-rgw-storequery header.
static constexpr size_t RGWSQMaxHeaderLength = 2048;

/**
 * @brief Parser for the x-rgw-storequery HTTP header.
 *
 * We need to parse the header and return an RGWOp-derived object to process
 * the REST operation associated with this request.
 *
 * The header format is explained in the documentation of the parse() method.
 */
class RGWSQHeaderParser {
private:
  std::string command_ = "";
  std::vector<std::string> param_;
  RGWOp* op_ = nullptr;

public:
  RGWSQHeaderParser() { }

  /// Reset the parser object.
  void reset();
  /// @private
  /// Tokenise the header value. Intended for testing, called implicitly by
  /// parse().
  bool tokenize(const DoutPrefixProvider* dpp, const std::string& input);

  /**
   * @brief Parse the value of the `x-rgw-storequery` header and configure
   * this to return an appropriate RGWOp* object.
   *
   * The header is required to contain only ASCII-7 printable characters
   * (codes 32-127). Any rune outside this range will result in the entire
   * request being rejected.
   *
   * There is no value in allowing UTF-8 with all its processing
   * sophistication here - if a command's parameters requires a wider
   * character set, those parameters will have to be e.g. base64 encoded.
   *
   * The header contents are most 2048 bytes. This value is chosen to allow
   * for an encoding of the maximum S3 key length (1024 bytes) into some safe
   * encoding, and for some additional parameters.
   *
   * Command names are ASCII-7 strings of arbitrary length. Case is ignored in
   * the command name.
   *
   * Command parameters are not case-tranformed, as it's not possible to know
   * in advance what significance case may have to as-yet unimplemented
   * commands. If case is significant in parameters, I recommend encoding with
   * e.g. base64 as I'm disinclined to trust proxies etc. to leave HTTP
   * headers alone.
   *
   * Command parameters are space-separated. However, double-quotes are
   * respected; double-quoted parameters may contain spaces, and contained
   * double-quotes may be escaped with the sequence `\"`. This facility is
   * included to allow for straightforward commands; however it is probably
   * more wise to encode 'complex' parameters with a scheme such as base64
   * rather than deal with a quote-encoding.
   *
   * @param dpp prefix provider.
   * @param input The value of the X- header.
   * @param handler_type An enum showing what type of handler that called us.
   * This affects which types of commands are valid for a given request.
   * @return true The header was successfully parsed; op() will return a
   * useful object.
   * @return false The header was not parsed, and op() will return nullptr.
   */
  bool parse(const DoutPrefixProvider* dpp, const std::string& input,
      RGWSQHandlerType handler_type);

  /// Quickly determine if the input is a valid base64 string.
  bool valid_base64(const DoutPrefixProvider* dpp, const std::string& input);

  RGWOp* op() noexcept { return op_; }
  std::string command() noexcept { return command_; }
  std::vector<std::string> param() noexcept { return param_; }
};

/**
 * @brief Common behaviour for StoreQuery implementations of RGWOp.
 *
 * There are some common behavious for StoreQuery commands:
 *
 * - All bypass authorization checks (verify_requester()).
 *
 * - All bypass permission checks (verify_permission()).
 *
 * - All return RGW_OP_TYPE_READ from op_mask().
 *
 * - All force their response format to JSON (by default).
 *
 * Commands have to implement execute(), send_response_json() and name() just
 * to compile. Other methods may well be required, of course.
 *
 * If you want to return something other than JSON, you need to override
 * send_response().
 */
class RGWStoreQueryOp_Base : public RGWOp {
private:
public:
  RGWStoreQueryOp_Base()
  {
  }

  /**
   * @brief Bypass requester authorization checks for storequery commands.
   *
   * @param auth_registry The registry (ignored).
   * @param y optional yield.
   * @return int zero (success).
   */
  int verify_requester(
      [[maybe_unused]] const rgw::auth::StrategyRegistry& auth_registry,
      [[maybe_unused]] optional_yield y) override
  {
    return 0;
  }
  /**
   * @brief Bypass permission checks for storequery commands.
   *
   * @param y optional yield.
   * @return int zero (success).
   */
  int verify_permission(optional_yield y) override { return 0; }

  /**
   * @brief StoreQuery commands are read-only.
   *
   * @return uint32_t the op type.
   */
  uint32_t op_mask() override { return RGW_OP_TYPE_READ; }

  /**
   * @brief Override hook for sending a command's response JSON.
   *
   * This method must be provided by subclasses to implement their responses.
   * The minimal implementation is an empty method; that's a valid JSON
   * document, so it's a valid response. You'll still get a `content-type:
   * application/xml` header in the HTTP response, and a valid response code.
   *
   * More typically, this will actually do something, e.g.
   *
   * ```
   *   s->formatter->open_object_section("MyCommandResult");
   *   s->formatter->dump_string("my_bool", true);
   *   s->formatter->close_section();
   * ```
   *
   * It's up to the override to send valid JSON. Note Ceph::formatter handles
   * other types of output as well, notably XML, so many of its methods will
   * be no-ops on JSON.
   */
  virtual void send_response_json() = 0;

  /**
   * @brief Override of RGWOp::send_response() with our default processing. In
   * normal use, leave this method alone and override send_response_json()
   * instead.
   *
   * We change the response formatter unconditionally to JSON (normally the
   * behaviour is to default to XML but to respect the `Accept:` header or a
   * `format=` query parameter).
   *
   * All our responses will be JSON, but we recommend callers still set
   * `Accept: application/json` so error responses will also be in JSON -
   * storequery doesn't control all error responses, and if the upstream REST
   * server sends the error you'll get XML by default.
   *
   * If you want different behaviour, you can still override send_response()
   * yourself. However, to get the standard behaviour, just override
   * send_response_json() and use \p s->formatter to format your response.
   */
  void send_response();

  // `void execute(optional_yield_ y)` still required.
  // `const char* name() const` still required.

protected:
  void send_response_pre();
  void send_response_post();
};

/**
 * @brief StoreQuery ping command implementation.
 *
 * Return a copy of the user's request_id (in the header) without further
 * processing. Used to check the command path.
 *
 * ```
 * Example query: request_id 'foo', object/bucket path is ignored.
 *
 * GET /
 * ...
 * x-rgw-storequery: ping foo
 * ...
 *
 * Example response:
 * 200 OK
 *
 * With body (formatting may vary):
 *
 *   "StoreQueryPingResult": {
 *     "request_id": "foo"
 *   }
 * ```
 *
 * The request_id is blindly mirrored back to the caller.
 *
 * Command-specific security considerations: Since the x- header is strictly
 * canonicalised (any non-printable ASCII-7 characters will result in the
 * header's rejection) there is no concern with mirroring the request back in
 * the response document.
 */
class RGWStoreQueryOp_Ping : public RGWStoreQueryOp_Base {
private:
  std::string request_id_;

public:
  RGWStoreQueryOp_Ping(const std::string& _request_id)
      : request_id_ { _request_id }
  {
  }

  /**
   * @brief Reflect the supplied request ID back to the caller.
   *
   * Used to indicate that storequery is operational, without reference to any
   * buckets or keys.
   *
   * @param y optional yield object.
   */
  void execute(optional_yield y) override;

  /**
   * @brief Send our JSON response.
   */
  void send_response_json() override;
  const char* name() const override { return "storequery_ping"; }
};

/**
 * @brief StoreQuery ObjectStatus command implementation.
 *
 * Return the status (presence, and optionally other details) of an object in
 * the context of the existing query.
 *
 * Look fairly hard to see if an object is present on this cluster. Check:
 *
 * - 'Regular' keys in the bucket (with or without versioning enabled).
 *
 * - In versioned mode, the presence of a delete marker is taken to indicate
 *   that the key is still present on this cluster.
 *
 * - If no regular key or delete marker is present, check to see if this key
 *   is presently receiving a multipart upload, and if so mark the key as
 *   'present' even though it won't show up otherwise until the multipart
 *   upload has completed successfully.
 *
 * As a side-effect of the multipart upload implementation, if the multipart
 * upload process fails, the key will show as not present in subsequent
 * queries.
 *
 * ```
 * Example query: Get status for bucket 'test', key 'foo' whose current
 * version is of size 123 bytes.
 *
 * GET /test/foo
 * ...
 * x-rgw-storequery: objectstatus
 * ...
 *
 * Example response:
 *   200 OK
 *
 * Response body:
 *
 *   "StoreQueryObjectStatusResult: {
 *     "Object": {
 *         "bucket": "test",
 *         "key": "foo",
 *         "deleted": false,
 *         "multipart_upload_in_progress": false,
 *         "version_id": "",
 *         "size": 123
 *     }
 *   }
 * ```
 */
class RGWStoreQueryOp_ObjectStatus : public RGWStoreQueryOp_Base {
private:
  std::string bucket_name_;
  std::string object_key_name_;
  std::string version_id_;
  size_t object_size_;
  bool object_deleted_;
  bool object_mpuploading_;
  std::string object_mpupload_id_;

  bool execute_simple_query(optional_yield y);
  bool execute_mpupload_query(optional_yield y);

public:
  RGWStoreQueryOp_ObjectStatus()
  {
  }

  /**
   * @brief execute() Implementation - query the index for the presence of the
   * given key.
   *
   * This will first query using rgw::sal::Bucket::list() for 'regular' keys
   * (or delete markers).
   *
   * If no key is found, it will then query using
   * rgw::sal::Bucket::list_multiparts() in order to find in-flight multipart
   * uploads for the key.
   *
   * In either search, if there is a failure other than 'not found' the search
   * will be terminated and an error will be returned via \p op_ret.
   *
   * If the key is not found, \p op_ret will be set to \p -ENOENT which will
   * result in a 404 being returned to the user.
   *
   * If the key is found, \p op_ret will be zero, and barring failures
   * elsewhere in the REST server the user will receive a 200.
   *
   * @param y optional yield object.
   */
  void execute(optional_yield y) override;

  /**
   * @brief Send our JSON response.
   */
  void send_response_json() override;
  const char* name() const override { return "storequery_objectstatus"; }
};

class RGWStoreQueryListItem {
private:
  std::string key_;
  std::optional<bool> is_deleted_;
  std::optional<uint64_t> size_;

public:
  RGWStoreQueryListItem(const std::string& key)
      : key_(key)
  {
  }
  const std::string& key() const noexcept { return key_; }

  void set_deleted(bool deleted) noexcept { is_deleted_ = deleted; }
  void unset_deleted() noexcept { is_deleted_.reset(); }
  std::optional<bool> is_deleted() const noexcept { return is_deleted_; }

  void set_size(uint64_t size) noexcept { size_ = size; }
  void unset_size() noexcept { size_.reset(); }
  std::optional<uint64_t> size() const noexcept { return size_; }

  void dump(Formatter* f) const
  {
    f->open_object_section("Object");
    f->dump_string("key", key_);
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
}; // RGWStoreQueryListItem

/**
 * @brief StoreQuery ObjectList command implementation.
 *
 * Return a list of objects in the bucket, limited to a maximum number of
 * records. Support pagination to allow the list to exceed the maximum number
 * of records.
 *
 * Unlike ObjectStatus, this will not include multipart uploads. It is
 * difficult to see how a single, paginated list across multiple RGWs could
 * meaninfully encompass both types, so we're not going to try. It makes sense
 * for ObjectStatus to combine the classes because the scope is limited. Here,
 * we're querying an entire bucket which might have billions of objects.
 *
 * Here is an example JSON response, with the query size limited rather
 * artifically to two objects:
 *
 * ```
 *   {
 *     "Objects": [
 *       {
 *         "key": "MDAwMDAwMTEK",
 *         "size": 16
 *       },
 *       {
 *         "key": "MDAwMDAwMjIK",
 *         "deleted": true
 *       }
 *     ],
 *     "Stats": {
 *       "entries_max": 2,
 *       "entries_actual": 2,
 *       "sal_seen": 2,
 *       "sal_exists": 1,
 *       "sal_current": 2,
 *       "sal_deleted": 1
 *     },
 *     "NextToken": "MDAwMDAwMjI="
 *   }
 * ```
 *
 * The object key names are base64-encoded as they can contain codepoints that
 * might interfere with JSON encoding. Size is in bytes.
 *
 * To retrieve the next page of results, the client issues an \a objectlist
 * query specifying the continuation token specified in the \a NextToken field
 * of the JSON response.
 */
class RGWStoreQueryOp_ObjectList : public RGWStoreQueryOp_Base {

protected:
  struct Stats {
    /// The maximum number of entries requested by the user.
    uint64_t entries_max = 0;
    /// The number of entries actually returned to the user.
    uint64_t entries_actual = 0;
    /// The number of list() queries of size \a entries_max to the SAL.
    uint64_t sal_queries = 0;
    /// The number of objects returned by querying the SAL.
    uint64_t sal_seen = 0;
    /// Out of \a sal_seen, the number of objects with the `exists` flag set.
    uint64_t sal_exists = 0;
    /// Out of \a sal_seen, the number of objects with the `current` flag set.
    uint64_t sal_current = 0;
    /// Out of \a sal_seen, the number of objects with the `current` flag
    /// cleared.
    uint64_t sal_not_current = 0;
    /// Out of \a sal_seen, the number of objects with the `current` /and/
    /// `deleted` flag set.
    uint64_t sal_deleted = 0;

    /// Format stats to the given formatter object.
    void dump(ceph::Formatter* f) const
    {
      f->dump_unsigned("entries_max", entries_max);
      f->dump_unsigned("entries_actual", entries_actual);
      f->dump_unsigned("sal_queries", sal_queries);
      f->dump_unsigned("sal_seen", sal_seen);
      f->dump_unsigned("sal_exists", sal_exists);
      f->dump_unsigned("sal_current", sal_current);
      f->dump_unsigned("sal_not_current", sal_not_current);
      f->dump_unsigned("sal_deleted", sal_deleted);
    }
  }; // struct RGWStoreQueryOp_ObjectList::Stats

  /**
   * @brief A single item in the list of objects returned by the query.
   */
  class Item {
  private:
    std::string key_;
    std::optional<bool> is_deleted_;
    std::optional<uint64_t> size_;

  public:
    Item(const std::string& key)
        : key_(key)
    {
    }
    const std::string& key() const noexcept { return key_; }

    void set_deleted(bool deleted) noexcept { is_deleted_ = deleted; }
    void unset_deleted() noexcept { is_deleted_.reset(); }
    std::optional<bool> is_deleted() const noexcept { return is_deleted_; }

    void set_size(uint64_t size) noexcept { size_ = size; }
    void unset_size() noexcept { size_.reset(); }
    std::optional<uint64_t> size() const noexcept { return size_; }

    void dump(Formatter* f) const;
  }; // RGWStoreQueryListItem

private:
  using item_type = Item;

  uint64_t max_entries_;
  std::optional<std::string> marker_;
  std::optional<std::string> return_marker_;
  std::vector<item_type> items_;

  Stats stats_;

public:
  RGWStoreQueryOp_ObjectList(uint64_t max_entries, std::optional<std::string> marker)
      : max_entries_(max_entries)
      , marker_(marker)
  {
  }

  RGWStoreQueryOp_ObjectList() = delete;
  RGWStoreQueryOp_ObjectList(const RGWStoreQueryOp_ObjectList&) = delete;
  RGWStoreQueryOp_ObjectList& operator=(const RGWStoreQueryOp_ObjectList&) = delete;
  RGWStoreQueryOp_ObjectList(RGWStoreQueryOp_ObjectList&&) = delete;
  RGWStoreQueryOp_ObjectList& operator=(RGWStoreQueryOp_ObjectList&&) = delete;

  static constexpr uint64_t LIST_QUERY_SIZE_HARD_LIMIT = 10000;

  /**
   * @brief execute() for RGWStoreQueryOp_ObjectList (StoreQuery objectlist).
   *
   * Simply calls execute_query() to perform the query.
   *
   * @param y optional yield.
   */
  void execute(optional_yield y) override;

  void send_response_json() override;
  const char* name() const override { return "storequery_objectlist"; }

  /**
   * @brief Fetch a subset of the list of object from the SAL.
   *
   * NOTE: The optional_yield is used here, so be careful if you're adding
   * tracing. The list queries can take a long time and you could end up
   * detaching traces from their parents.
   *
   * This method will populate \p items_ with a subset of the objects in the
   * bucket. The query is not configurable; we want to do as little work as
   * possible, and provide as few ways to break things as we can.
   *
   * If the query fails, \p op_ret will be set to the error code and \p items_
   * must not be used. If the query succeeds, \p op_ret will be >= 0, and \a
   * send_response_json() can return results to the user.
   *
   * It is highly likely that the query will not be able to return all objects
   * in a single request. As a result, the JSON will include a NextToken field
   * that will allow the user to request the next page of results. This is
   * very similar to pagination in AWS. Pass the returned continuation token
   * to the next query to get the next page of results.
   *
   * The page size is capped at LIST_QUERY_SIZE_HARD_LIMIT. This is a
   * precaution to stop over-zealous microservices breaking the OSDs. We do
   * not at the time of writing know the impact of gigantic list queries, so
   * the value is capped at the largest value used by vanilla RGW as part of
   * the SWIFT API. S3 usually limits responses to 1000 objects.
   *
   * Using very small page sizes is not helpful. The SAL queries will perform
   * a readahead of (by default) 1000 objects anyway, so setting a page size
   * less than this actively wastes time and does unnecessary work.
   *
   * The pagination can return duplicate items across requests in versioned
   * buckets. The client should deal with this gracefully.
   *
   * @param y optional yield.
   * @return true Success. \p items_ is populated and can be used.
   * @return false Failure. \p op_ret is set, and \p items_ should not be
   * used.
   */
  bool execute_query(optional_yield y);

  /**
   * @brief Set the return marker (continuation token) for the next query.
   *
   * By default, the return marker is \p std::nullopt, indicating no value.
   *
   * @param marker The return marker (contination token) to set.
   */
  void set_return_marker(const std::string& marker) { return_marker_ = marker; }
  /// Reset the return marker to its default no value state.
  void unset_return_marker() { return_marker_.reset(); }
  /// Fetch the return marker (continuation token) for the next query, or
  /// std::nullopt if none is set.
  std::optional<std::string> return_marker() const { return return_marker_; }

  /// Fetch the maximum number of entries to return in this query.
  uint64_t max_entries() const { return max_entries_; }
  /// Fetch the continuation marker used when issuing this query. Contrast
  /// with return_marker() which is the optional marker for the next query.
  std::optional<std::string> marker() const { return marker_; }

}; // RGWStoreQueryOp_ObjectList

/**
 * @brief StoreQuery MPUploadList command implementation.
 *
 * Return a list of in-flight multipart uploads for the bucket, limited to a
 * maximum number of records. Support pagination to allow the list to exceed
 * the maximum number of records.
 *
 * This is a counterpart to the objectlist command.
 *
 * Here is an example JSON response with the query size limited rather
 * artificially to two objects:
 *
 * ```
 *  {
 *    "Objects": [
 *      {
 *        "key": "bXAwMDAwMDAwMQ==",
 *        "upload_id": "Mn5hXzhtV2UtZGRaU1VPNF83WkJla0M2ZnNCV2VOOUpl"
 *      },
 *      {
 *        "key": "bXAwMDAwMDAwMg==",
 *        "upload_id": "Mn5GcXo2enFpT0t2bUlleVljY3llSFJ1b09ZeHZSWkhI"
 *      }
 *    ],
 *    "NextToken": "bXAwMDAwMDAwMi4yfkZxejZ6cWlPS3ZtSWV5WWNjeWVIUnVvT1l4dlJaSEgubWV0YQ=="
 *  }
 * ```
 *
 * The object key names and the upload IDs are base64-encoded, since both can
 * contain codepoints that might not survive JSON encoding.
 *
 * To retrieve the next page of results, the client issues an \a mpuploadlist
 * query specifying the continuation token specified in the \a NextToken field
 * of the JSON response.
 */
class RGWStoreQueryOp_MPUploadList : public RGWStoreQueryOp_Base {

protected:
  /**
   * @brief A single item in the list of in-progress multipart uploads.
   */
  class Item {
  private:
    std::string key_;
    std::string upload_id_;

  public:
    Item(const std::string& key, const std::string& upload_id)
        : key_(key)
        , upload_id_(upload_id)
    {
    }
    const std::string& key() const noexcept { return key_; }
    const std::string& upload_id() const noexcept { return upload_id_; }

    void dump(Formatter* f) const;
  }; // Item

private:
  using item_type = Item;

  uint64_t max_entries_;
  std::optional<std::string> marker_;
  std::optional<std::string> return_marker_;
  std::vector<item_type> items_;

public:
  RGWStoreQueryOp_MPUploadList(uint64_t max_entries, std::optional<std::string> marker)
      : max_entries_(max_entries)
      , marker_(std::move(marker))
  {
  }

  RGWStoreQueryOp_MPUploadList() = delete;
  RGWStoreQueryOp_MPUploadList(const RGWStoreQueryOp_MPUploadList&) = delete;
  RGWStoreQueryOp_MPUploadList& operator=(const RGWStoreQueryOp_MPUploadList&) = delete;
  RGWStoreQueryOp_MPUploadList(RGWStoreQueryOp_MPUploadList&&) = delete;
  RGWStoreQueryOp_MPUploadList& operator=(RGWStoreQueryOp_MPUploadList&&) = delete;

  static constexpr uint64_t LIST_MULTIPARTS_QUERY_SIZE_HARD_LIMIT = 10000;

  void execute(optional_yield y) override;

  void send_response_json() override;

  const char* name() const override { return "storequery_mpuploadlist"; }

  /**
   * @brief Fetch a subset of the list of in-progress multipart uploads from
   * the SAL.
   *
   * NOTE: The optional_yield is used here, so be careful if you're adding
   * tracing. It turns out that the multipart_list call doesn't actually take
   * the yield (I think it should) but don't assume this will always be the
   * case.
   *
   * This method will populate \p items_ with a subset of the in-progress
   * multipart uploads. The query is not configurable; we want to do as little
   * work as possible, and provide as few ways to break things as we can.
   *
   * If the query fails, \p op_ret will be set to the error code and \p items_
   * must not be used. If the query succeeds, \p op_ret will be >= 0, and \a
   * send_response_json() can return results to the user.
   *
   * It will often be the case that the query will not be able to return all
   * in-progress multipart uploads in one go. In that case, the JSON will
   * include  a NextToken field that will allow the user to request the next
   * page of results. This is very similar to pagination in AWS. Pass the
   * returned contuation token to the next query to get the next page of
   * results.
   *
   * The page size is capped at LIST_MULTIPARTS_QUERY_SIZE_HARD_LIMIT. This is
   * a precaution to stop over-zealous microservices breaking the OSDs. We do
   * not at the time of writing know the impact of gigantic list queries, so
   * the value is capped at the largest value passed to
   * rgw::sal::Bucket::list() (the underlying call used by
   * rgw::sal::Bucket::list_multipart_uploads()) in RGW. This is the value
   * used by the SWIFT API. S3 usually limits responses to 1000 objects.
   *
   * Using very small page sizes is not helpful. The SAL queries will perform
   * a readahead of (by default) 1000 objects anyway, so setting a page size
   * less than this actively wastes time and does unnecessary work.
   *
   * @param y optional yield.
   * @return true Success. \p items_ is populated and can be used.
   * @return false Failure. \p op_ret is set, and \p items_ should not be
   * used.
   */
  bool execute_query(optional_yield y);

  /**
   * @brief Set the return marker (continuation token) for the next query.
   *
   * By default, the return marker is \p std::nullopt, indicating no value.
   *
   * @param marker The return marker (contination token) to set.
   */
  void set_return_marker(const std::string& marker) { return_marker_ = marker; }
  /// Reset the return marker to its default no value state.
  void unset_return_marker() { return_marker_.reset(); }
  /// Fetch the return marker (continuation token) for the next query, or
  /// std::nullopt if none is set.
  std::optional<std::string> return_marker() const { return return_marker_; }

}; // RGWStoreQueryOp_MPUploadList

} // namespace rgw
