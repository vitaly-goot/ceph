// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest-param-test.h>
#include <gtest/gtest.h>
#include <initializer_list>

#include "cls/rgw/cls_rgw_types.h"
#include "common/async/yield_context.h"
#include "common/ceph_argparse.h"
#include "common/dout.h"
#include "global/global_context.h"
#include "rgw/rgw_rest_storequery.h"
#include "rgw_common.h"
#include "rgw_obj_types.h"
#include "rgw_process_env.h"

// Despite clangd's assertions, the following two includes are necessary
// because there are a couple of static asserts in RGWProcessEnv that check
// that rgw::sal::LuaManager and rgw::auth::StrategyRegistry are of size > 0.
#include "rgw/rgw_sal.h"
#include "rgw_auth_registry.h"

namespace {

using namespace std::string_literals;

using namespace rgw;

class StoreQueryHeaderParserTest : public ::testing::Test {
protected:
  DoutPrefix dpp { g_ceph_context, ceph_subsys_rgw, "unittest " };
  RGWSQHeaderParser p;
};

TEST_F(StoreQueryHeaderParserTest, EmptyFail)
{
  ASSERT_FALSE(p.parse(&dpp, "", RGWSQHandlerType::Service));
}
TEST_F(StoreQueryHeaderParserTest, TooLongFail)
{
  auto s = std::string(RGWSQMaxHeaderLength + 1, ' ');
  ASSERT_FALSE(p.parse(&dpp, s, RGWSQHandlerType::Service));
}
TEST_F(StoreQueryHeaderParserTest, EmptyBogusFail)
{
  ASSERT_FALSE(p.parse(&dpp, "nope", RGWSQHandlerType::Service));
}
TEST_F(StoreQueryHeaderParserTest, BogonCharFail)
{
  // Control character.
  ASSERT_FALSE(p.parse(&dpp, "ping\007", RGWSQHandlerType::Service));
  // >127.
  ASSERT_FALSE(p.parse(&dpp, "ping\xff", RGWSQHandlerType::Service));
}
TEST_F(StoreQueryHeaderParserTest, Tokenizer)
{
  ASSERT_TRUE(p.tokenize(&dpp, "one two three"));
  ASSERT_EQ(p.command(), "one");
  ASSERT_EQ(p.param().size(), 2);
  ASSERT_EQ(p.param()[0], "two");
  ASSERT_EQ(p.param()[1], "three");

  // Throw in a space-separated field.
  p.reset();
  ASSERT_TRUE(p.tokenize(&dpp, R"(one "two, two-and-a-half" three)"));
  ASSERT_EQ(p.command(), "one");
  ASSERT_EQ(p.param().size(), 2);
  ASSERT_EQ(p.param()[0], "two, two-and-a-half");
  ASSERT_EQ(p.param()[1], "three");

  // Add an escaped double-quote in a quoted field. The first param should be
  // 'two' followed by a double-quote character.
  p.reset();
  ASSERT_TRUE(p.tokenize(&dpp, R"(one "two\"" three)"));
  ASSERT_EQ(p.command(), "one");
  ASSERT_EQ(p.param().size(), 2);
  ASSERT_EQ(p.param()[0], "two\"");
  ASSERT_EQ(p.param()[1], "three");

  // Add an escaped double-quote in a non-quoted field. The second param should
  // be 'three' with a double-quote character before 'r'.
  p.reset();
  ASSERT_TRUE(p.tokenize(&dpp, R"(one "two" th\"ree)"));
  ASSERT_EQ(p.command(), "one");
  ASSERT_EQ(p.param().size(), 2);
  ASSERT_EQ(p.param()[0], "two");
  ASSERT_EQ(p.param()[1], "th\"ree");
}
TEST_F(StoreQueryHeaderParserTest, PingSuccess)
{
  // Successful parse.
  ASSERT_TRUE(p.parse(&dpp, "Ping foo", RGWSQHandlerType::Service));
  ASSERT_EQ(p.command(), "ping");
  ASSERT_EQ(p.param().size(), 1);
  ASSERT_TRUE(p.op() != nullptr);
  ASSERT_STREQ(p.op()->name(), "storequery_ping");
}

TEST_F(StoreQueryHeaderParserTest, PingFail)
{
  // Fail parse.
  ASSERT_FALSE(p.parse(&dpp, "ping", RGWSQHandlerType::Service));
  p.reset();
  ASSERT_FALSE(p.parse(&dpp, "ping foo bar", RGWSQHandlerType::Service));
}

TEST_F(StoreQueryHeaderParserTest, ObjectStatusSuccess)
{
  // Successful parse.
  ASSERT_TRUE(p.parse(&dpp, "ObjectStatus", RGWSQHandlerType::Obj));
  ASSERT_EQ(p.command(), "objectstatus");
  ASSERT_TRUE(p.param().empty());
  ASSERT_TRUE(p.op() != nullptr);
  ASSERT_STREQ(p.op()->name(), "storequery_objectstatus");
}

TEST_F(StoreQueryHeaderParserTest, ObjectStatusFail)
{
  // Fail parse.
  ASSERT_FALSE(p.parse(&dpp, "objectstatus foo", RGWSQHandlerType::Obj));
  // Wrong handler type.
  p.reset();
  ASSERT_FALSE(p.parse(&dpp, "objectstatus", RGWSQHandlerType::Service));
  // Wrong handler type.
  p.reset();
  ASSERT_FALSE(p.parse(&dpp, "objectstatus", RGWSQHandlerType::Bucket));
}

TEST_F(StoreQueryHeaderParserTest, ObjectListSuccess)
{
  ASSERT_TRUE(p.parse(&dpp, "objectlist 666", RGWSQHandlerType::Bucket));
  ASSERT_EQ(p.command(), "objectlist");
  ASSERT_EQ(p.param().size(), 1);
  ASSERT_TRUE(p.op() != nullptr);
  ASSERT_STREQ(p.op()->name(), "storequery_objectlist");

  // Two-argument form. Second arg must be valid base64.
  p.reset();
  ASSERT_TRUE(p.parse(&dpp, "objectlist 666 cmh1YmFyYGo=", RGWSQHandlerType::Bucket));
  ASSERT_EQ(p.command(), "objectlist");
  ASSERT_EQ(p.param().size(), 2);
  ASSERT_TRUE(p.op() != nullptr);
  ASSERT_STREQ(p.op()->name(), "storequery_objectlist");
}

TEST_F(StoreQueryHeaderParserTest, ObjectListFail)
{
  // Fail parse (no argument).
  ASSERT_FALSE(p.parse(&dpp, "objectlist", RGWSQHandlerType::Bucket));
  p.reset();
  // Fail parse (max two arguments).
  ASSERT_FALSE(p.parse(&dpp, "objectlist 666 TOKEN_FOO rhubarb", RGWSQHandlerType::Bucket));
  p.reset();
  // Fail parse (not int).
  ASSERT_FALSE(p.parse(&dpp, "objectlist foo", RGWSQHandlerType::Bucket));
  p.reset();
  // Wrong handler type.
  ASSERT_FALSE(p.parse(&dpp, "objectlist 666", RGWSQHandlerType::Service));
  p.reset();
  // Wrong handler type.
  ASSERT_FALSE(p.parse(&dpp, "objectlist 666", RGWSQHandlerType::Obj));
  p.reset();

  // Fail parse (second argument not valid base64).
  ASSERT_FALSE(p.parse(&dpp, "objectlist 666 xx!", RGWSQHandlerType::Bucket));
  p.reset();
  // Fail parse (second argument not valid base64).
  ASSERT_FALSE(p.parse(&dpp, "objectlist 666 xx", RGWSQHandlerType::Bucket));
  p.reset();
  ASSERT_FALSE(p.parse(&dpp, "objectlist 666 x", RGWSQHandlerType::Bucket));
  p.reset();
}

TEST_F(StoreQueryHeaderParserTest, MPUploadListSuccess)
{
  ASSERT_TRUE(p.parse(&dpp, "mpuploadlist 666", RGWSQHandlerType::Bucket));
  ASSERT_EQ(p.command(), "mpuploadlist");
  ASSERT_EQ(p.param().size(), 1);
  ASSERT_TRUE(p.op() != nullptr);
  ASSERT_STREQ(p.op()->name(), "storequery_mpuploadlist");

  // Two-argument form. Second arg must be valid base64.
  p.reset();
  ASSERT_TRUE(p.parse(&dpp, "mpuploadlist 666 cmh1YmFyYGo=", RGWSQHandlerType::Bucket));
  ASSERT_EQ(p.command(), "mpuploadlist");
  ASSERT_EQ(p.param().size(), 2);
  ASSERT_TRUE(p.op() != nullptr);
  ASSERT_STREQ(p.op()->name(), "storequery_mpuploadlist");
}

TEST_F(StoreQueryHeaderParserTest, MPUploadListFail)
{
  // Fail parse (no argument).
  ASSERT_FALSE(p.parse(&dpp, "mpuploadlist", RGWSQHandlerType::Bucket));
  p.reset();
  // Fail parse (max two arguments).
  ASSERT_FALSE(p.parse(&dpp, "mpuploadlist 666 TOKEN_FOO rhubarb", RGWSQHandlerType::Bucket));
  p.reset();
  // Fail parse (not int).
  ASSERT_FALSE(p.parse(&dpp, "mpuploadlist foo", RGWSQHandlerType::Bucket));
  p.reset();
  // Wrong handler type.
  ASSERT_FALSE(p.parse(&dpp, "mpuploadlist 666", RGWSQHandlerType::Service));
  p.reset();
  // Wrong handler type.
  ASSERT_FALSE(p.parse(&dpp, "mpuploadlist 666", RGWSQHandlerType::Obj));
  p.reset();

  // Fail parse (second argument not valid base64).
  ASSERT_FALSE(p.parse(&dpp, "mpuploadlist 666 xx!", RGWSQHandlerType::Bucket));
  p.reset();
  // Fail parse (second argument not valid base64).
  ASSERT_FALSE(p.parse(&dpp, "mpuploadlist 666 xx", RGWSQHandlerType::Bucket));
  p.reset();
  ASSERT_FALSE(p.parse(&dpp, "mpuploadlist 666 x", RGWSQHandlerType::Bucket));
  p.reset();
}

/***************************************************************************/

/* objectlist test harness */

// Stole this from test_rgw_lua.cc. Set up a req_state s for testing.
#define DEFINE_REQ_STATE \
  RGWProcessEnv pe;      \
  RGWEnv e;              \
  req_state s(g_ceph_context, pe, &e, 0);

// Minimal client for req_state.
class TestClient : public rgw::io::BasicClient {
  RGWEnv env;

protected:
  virtual int init_env(CephContext* cct) override
  {
    return 0;
  }

public:
  virtual RGWEnv& get_env() noexcept override
  {
    return env;
  }

  virtual size_t complete_request() override
  {
    return 0;
  }
};

// Minimally model an entry in a bucket index. There's a lot we don't care
// about, but we do care about the key's name, instance, and versioning.
struct SrcKey {
  std::string name = "";
  std::string instance = "";
  bool versioned = false;
  bool exists = false;
  bool current = false;
  bool delete_marker = false;

  enum Flag {
    /// The VERSIONED flag must be present for the CURRENT flag to take
    /// effect, namely for rgw_bucket_dir_entry.is_versioned() to operate.
    VERSIONED = 1 << 0,
    EXISTS = 1 << 1,
    CURRENT = 1 << 2,
    DELETE_MARKER = 1 << 3
  };

  /// Construct a nonversioned object, which exists and is current.
  SrcKey(const std::string& name)
      : name(name)
      , versioned(false)
      , exists(true)
      , current(true)
  {
  }
  /// Construct a versioned object, which exists and is current, but is not a
  /// delete marker.
  SrcKey(const std::string& name, const std::string& instance)
      : name(name)
      , instance(instance)
      , versioned(true)
      , exists(true)
      , current(true)
  {
  }
  /// Construct a potentially versioned object, setting the versioned, exists,
  /// current and delete_marker flags specifically.
  SrcKey(const std::string& name, const std::string& instance, int flags)
      : name(name)
      , instance(instance)
  {
    versioned = (flags & VERSIONED) != 0;
    exists = (flags & EXISTS) != 0;
    current = (flags & CURRENT) != 0;
    delete_marker = (flags & DELETE_MARKER) != 0;
  }

  // Default auto-generated constructors are all fine.

  // Create a new delete marker suitable for the top of the version stack,
  // i.e. current, !exists, delete_marker.
  SrcKey new_delete_marker(const std::string& name, const std::string& instance)
  {
    return SrcKey(name, instance, Flag::VERSIONED | Flag::CURRENT | Flag::DELETE_MARKER);
  }

  /// Mimic the behaviour of rgw_bucket_dir_entry.is_current().
  bool is_current() const
  {
    return (!versioned) || (versioned && current && !exists && !delete_marker);
  }

  rgw_bucket_dir_entry to_dir_entry() const
  {
    rgw_bucket_dir_entry entry;
    entry.key = cls_rgw_obj_key(name, instance);
    entry.exists = exists;

    uint16_t flags = 0;
    if (versioned) {
      flags |= rgw_bucket_dir_entry::FLAG_VER;
    }
    if (current) {
      flags |= rgw_bucket_dir_entry::FLAG_CURRENT;
    }
    if (delete_marker) {
      flags |= rgw_bucket_dir_entry::FLAG_DELETE_MARKER;
    }
    entry.flags = flags;
    return entry;
  }

  rgw_obj_key to_marker() const
  {
    return rgw_obj_key(name, instance);
  }

  bool operator==(const SrcKey& other) const
  {
    return name == other.name
        && instance == other.instance
        && versioned == other.versioned
        && exists == other.exists
        && current == other.current
        && delete_marker == other.delete_marker;
  }

  bool operator!=(const SrcKey& other) const
  {
    return !(*this == other);
  }

  std::string to_string() const
  {
    return fmt::format(FMT_STRING("SrcKey(name={}, instance={}, versioned={}, exists={}, current={}, delete_marker={})"),
        name, instance, versioned, exists, current, delete_marker);
  }

  friend std::ostream& operator<<(std::ostream& os, const SrcKey& key)
  {
    os << key.to_string();
    return os;
  }

}; // struct SrcKey

// This fmtlib formatter spec needs to be outside the anonymous namespace.
class SrcKey; // Forward declaration.

class SQObjectlistHarness : public testing::TestWithParam<size_t> {
protected:
  /// The default number of entries to return in a list operation. When
  /// debugging, it will help you *a lot* to reduce this to a much smaller
  /// number, like 10.
  static constexpr uint64_t kDefaultEntries = 1000;
  // static constexpr uint64_t kDefaultEntries = 10;

  RGWStoreQueryOp_ObjectList* op = nullptr;

protected:
  req_state* s_;
  TestClient cio_;

  std::shared_ptr<DoutPrefix> dpp_;
  // It just looks weird to not have 'dpp' as the pointer.
  DoutPrefixProvider* dpp;

  using SALBucket = rgw::sal::Bucket;

public:
  std::vector<SrcKey> src_bucket_;

protected:
  void SetUp() override
  {
    dpp_ = std::make_shared<DoutPrefix>(g_ceph_context, ceph_subsys_rgw, "unittest ");
    dpp = dpp_.get();
    ldpp_dout(dpp, 1) << "SetUp()" << dendl;
  }

  void TearDown() override
  {
    ldpp_dout(dpp, 1) << "TearDown()" << dendl;
    if (op) {
      delete op;
      op = nullptr;
    }
    dpp_.reset();
    s_ = nullptr;
  }

  void init_op(req_state* s, uint64_t max_entries, std::optional<std::string> marker)
  {
    s_ = s;
    s_->cio = &cio_;
    op = new RGWStoreQueryOp_ObjectList(max_entries, marker);
    op->set_req_state(s_);
    // By default, have the list function throw an exception. That way,
    // forgetting to set the list function should be hard to do.
    op->set_list_function(std::bind(&SQObjectlistHarness::list_always_throw, this,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    op->init(nullptr, s_, nullptr);
    ldpp_dout(dpp, 1) << "op configured" << dendl;
  }

  void fill_bucket_nonversioned(size_t count)
  {
    std::vector<SrcKey> src;
    src.reserve(count);
    for (int i = 0; i < count; ++i) {
      // This uses the single-string (nonversioned object) constructor.
      src.emplace_back(fmt::format("obj{:04d}", i));
    }
    move_bucket(std::move(src));
  }

  void fill_bucket_versioned(size_t count, size_t versions_per_item, std::vector<size_t> deletions = {})
  {
    std::vector<bool> del(count);
    assert(del.size() == count);
    for (auto index : deletions) {
      // Allow for a fixed array of deletions that might be larger than count.
      if (index < del.size()) {
        del[index] = true;
      }
    }

    std::vector<SrcKey> src;
    src.reserve(count * versions_per_item);
    for (int i = 0; i < count; i++) {
      // rados will put the current entry first.
      if (del[i]) {
        SrcKey newkey { fmt::format("obj{:04d}", i), "d0001", SrcKey::Flag::VERSIONED | SrcKey::Flag::CURRENT | SrcKey::Flag::DELETE_MARKER };
        // ldpp_dout(dpp, 20) << fmt::format(
        //     FMT_STRING("fill_bucket_versioned() adding delete marker key: {}"), newkey.to_string())
        //                    << dendl;
        src.push_back(newkey);
      }
      for (int v = 0; v < versions_per_item; v++) {
        bool current = false;
        if (del[i]) {
          current = false;
        } else {
          // Current entry goes first.
          current = (v == 0);
        }
        // This uses the full constructor (name, instance, flags).
        using f = SrcKey::Flag;
        SrcKey newkey { fmt::format("obj{:04d}", i), fmt::format("v{:04d}", v), f::VERSIONED | (current ? f::CURRENT : 0) | f::EXISTS };
        // ldpp_dout(dpp, 20) << fmt::format(
        //     FMT_STRING("fill_bucket_versioned() adding key: {}"), newkey.to_string())
        //                    << dendl;
        src.push_back(newkey);
      }
    }
    move_bucket(std::move(src));
  }

  std::set<std::string> bucket_object_keys()
  {
    std::set<string> res;
    for (const auto& key : src_bucket_) {
      res.insert(key.name);
    }
    return res;
  }

  // Quickly initialise the source bucket from an existing vector, destroying
  // the original vector.
  void move_bucket(std::vector<SrcKey>&& keys)
  {
    src_bucket_ = std::move(keys);
  }

  // Allow tests to index the source bucket.
  const std::vector<SrcKey>& src_bucket() const
  {
    return src_bucket_;
  }

public:
  /// List implementation that always throws an exception.
  int list_always_throw(const DoutPrefixProvider* dpp, SALBucket::ListParams&, int, SALBucket::ListResults& results, optional_yield y)
  {
    clear_results(results);
    throw std::runtime_error("list_throw");
  }

  /// List implementation that always returns -ERR_INTERNAL_ERROR.
  int list_always_fail(const DoutPrefixProvider* dpp, SALBucket::ListParams&, int, SALBucket::ListResults& results, optional_yield y)
  {
    clear_results(results);
    return -ERR_INTERNAL_ERROR;
  }

  /**
   * @brief List implementation that always returns an empty list and EoL.
   *
   * 'Empty' means the items() list is empty. EoL means that 'is_truncated' is
   * set in \p results.
   *
   * @param dpp     DoutPrefixProvider for logging.
   * @param param   Parameters to the list function.
   * @param results InOut: Results from the simulated list function.
   * @param y       Optional yield object.
   * @return int    The error code. Zero means success, <0 means failure with
   * a meaningful code.
   */
  int list_always_empty(const DoutPrefixProvider* dpp, SALBucket::ListParams& param, int, SALBucket::ListResults& results, optional_yield y)
  {
    clear_results(results);
    results.is_truncated = false;
    return 0; // Success.
  }

  /**
   * @brief Mimic the standard SAL list operation.
   *
   * Take the src_bucket_ array and page it out to the results object as
   * list() would.
   *
   * @param dpp The DoutPrefixProvider for logging.
   * @param param Parameters to the list function.
   * @param results InOut: Results from the simulated list function.
   * @param y       Optional yield object.
   * @return int    The error code. Zero means success, <0 means failure with
   * a meaningful code.
   */
  int list_standard(const DoutPrefixProvider* dpp, SALBucket::ListParams& param, int, SALBucket::ListResults& results, optional_yield y)
  {
    clear_results(results);

    auto& objs = results.objs;

    size_t start_index = 0;
    if (!param.marker.empty()) {
      bool seen_marker = false;
      // If there's a marker, scan the bucket for the marker. Note we don't
      // include the last item, a token pointing at the last item doesn't make
      // sense (we shouldn't have returned it, it's the EOF).
      for (size_t n = 0; n < src_bucket_.size() - 1; ++n) {
        if (src_bucket_[n].name == param.marker.name && src_bucket_[n].instance == param.marker.instance) {
          // We'll start at the immediately following item.
          start_index = n + 1;
          seen_marker = true;
          break;
        }
      }
      if (!seen_marker) {
        ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("marker[name={},instance={}] not found in bucket"), param.marker.name, param.marker.instance) << dendl;
        results.is_truncated = true;
        return -ENOENT; // XXX XXX what does RGW do with marker-not-found?
      }
    }
    ldpp_dout(dpp, 20) << fmt::format(
        FMT_STRING("list_standard() marker=[name={},instance={}] start_index={}"),
        param.marker.name, param.marker.instance, start_index)
                       << dendl;

    int max_entries = op->max_entries();
    bool seen_eof = false;

    size_t n = start_index;
    for (; objs.size() < max_entries && n < src_bucket_.size(); n++) {
      auto src_obj = src_bucket_[n];
      auto entry = src_obj.to_dir_entry();
      objs.push_back(entry);
      // Just keep this as a running item, to simplify bookkeeping.
      results.next_marker = src_obj.to_marker();
      // Likewise update the params, as the API expects.
      param.marker = results.next_marker;
    }
    ldpp_dout(dpp, 5) << fmt::format(
        FMT_STRING("list_standard() loop exit n={} objs.size={} marker=[name={},instance={}]"),
        n, objs.size(), param.marker.name, param.marker.instance)
                      << dendl;
    assert(objs.size() <= max_entries);
    if (n == src_bucket_.size()) {
      seen_eof = true;
      // In this case, we don't want a marker set.
      results.next_marker = rgw_obj_key();
    }
    // is_truncated means there are more results available.
    results.is_truncated = !seen_eof;

    return 0; // Success.
  }

protected:
  /**
   * @brief Clear the results of a bucket list operation. Expected to be
   * called by list implementations to set \p results to sane default values.
   *
   * @param results The results Object to clear.
   */
  void clear_results(SALBucket::ListResults& results)
  {
    results.objs.clear();
    results.common_prefixes.clear();
    results.next_marker = rgw_obj_key();
    results.is_truncated = false;
  }
}; // class StoreQueryObjectListHarness

TEST_F(SQObjectlistHarness, HarnessDefaults)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  // The default setup sets the list function to list_always_throw.
  ASSERT_THROW(op->execute(null_yield), std::runtime_error);
}

TEST_F(SQObjectlistHarness, AlwaysFail)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&SQObjectlistHarness::list_always_fail, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), -ERR_INTERNAL_ERROR);
}

// This catches a meaningful special case where the bucket is empty. Make sure
// we exit the loop properly when there are no items at all.
TEST_F(SQObjectlistHarness, AlwaysEmpty)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&SQObjectlistHarness::list_always_empty, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 0U);
}

TEST_F(SQObjectlistHarness, StdEmpty)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 0U);
}

// Check the first page of potentially paginated output.
TEST_P(SQObjectlistHarness, StdNonVersionedFirstPage)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

  size_t count = GetParam();
  fill_bucket_nonversioned(count);

  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  // We should never return more results than the max.
  ASSERT_LE(op->items().size(), kDefaultEntries);
  // If we requested <= kDefaultItems entries, we should match exactly and we
  // should see the EOF flag.
  // This catches important special-case handling of the marker. If we're on
  // an exact page boundary we don't set the marker.
  if (count <= kDefaultEntries) {
    ASSERT_EQ(op->items().size(), count);
    ASSERT_TRUE(op->seen_eof());
    ASSERT_TRUE(op->return_marker() == std::nullopt);
  } else {
    // If there were more than kDefaultItems entries, we shouldn't see EOF.
    ASSERT_FALSE(op->seen_eof());
    auto last = op->items()[op->items().size() - 1];
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("last item: key={}"), last.key()) << dendl;
    auto opt_token = op->return_marker();
    ASSERT_TRUE(opt_token != std::nullopt);

    if (opt_token.has_value()) {
      // If this base64 decode throws, the test will fail.
      auto token_json = from_base64(*opt_token);
      auto opt_token_marker = unpack_continuation_token(dpp, token_json);
      ASSERT_TRUE(opt_token_marker != std::nullopt);
      ASSERT_EQ(last.key(), opt_token_marker->name);
    }
  }
}

// Check the last page of paginated output. It's easy to predict what the
// continuation token will be for nonversioned buckets.
TEST_P(SQObjectlistHarness, StdNonVersionedLastPage)
{
  DEFINE_REQ_STATE;
  size_t count = GetParam();
  fill_bucket_nonversioned(count);

  size_t last_page_size = count % kDefaultEntries;
  size_t last_page_index = (count / kDefaultEntries) * kDefaultEntries;
  if (last_page_size == 0) {
    // Round the page index down so we get the actual last page.
    last_page_index -= kDefaultEntries;
    // An exact multiple of the page size, so round the size up.
    last_page_size = kDefaultEntries;
  }
  if (last_page_index <= 0) {
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("for count={}, last_page_index={}"), count, last_page_index) << dendl;
    return;
  }
  auto marker_src_key = src_bucket_[last_page_index - 1];
  auto marker_obj_key = marker_src_key.to_marker();
  auto token = prepare_continuation_token(dpp, marker_obj_key);
  auto token_b64 = to_base64(token);

  init_op(&s, kDefaultEntries, token_b64);
  op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  // We should never return more results than the max.
  ASSERT_LE(op->items().size(), kDefaultEntries);

  // Ok: We expect a certain number of items.
  ASSERT_EQ(op->items().size(), last_page_size);
  // We expect EOF, it's the last page.
  ASSERT_TRUE(op->seen_eof());
  // First item should be as expected.
  auto first_item = src_bucket_[last_page_index];
  ASSERT_EQ(op->items()[0].key(), first_item.to_marker());

  // Check for exact-page-boundary special case.
  if (last_page_size == kDefaultEntries) {
    // If we have an exact page boundary, we shouldn't return a marker.
    ASSERT_TRUE(op->return_marker() == std::nullopt);
  }
}

TEST_F(SQObjectlistHarness, StdVersionedOneItemFiveVersions)
{
  // One item with five versions, only the last item will be current.
  fill_bucket_versioned(1, 5);

  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 1U);
}

TEST_F(SQObjectlistHarness, StdVersionedOneItemFiveVersionsWithOneDeleted)
{
  // One item with five versions, only the last item will be current.
  fill_bucket_versioned(1, 5, { 0 });

  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 1U);
}

// Check the first page of potentially paginated output.
TEST_P(SQObjectlistHarness, StdVersionedFirstPage)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

  size_t count = GetParam();
  fill_bucket_versioned(count, 5);

  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  // We should never return more results than the max.
  ASSERT_LE(op->items().size(), kDefaultEntries);

  // With a versioned bucket we can make fewer assertions about the size of
  // lists than we can with nonversioned buckets. For exaple, we can have the
  // right number of returned items and still have a continuation token
  // because there are more versions of the last object to return.
  //
  // This is by way of explaining why a bunch of additional assertions from
  // the nonversioned tests are missing here.
}

TEST_P(SQObjectlistHarness, CompoundQueryNonversioned)
{
  auto count = GetParam();
  fill_bucket_nonversioned(count);

  std::optional<std::string> next_marker;
  int reps = 0;
  std::vector<RGWStoreQueryOp_ObjectList::item_type> items;

  while (true) {
    ASSERT_LT(reps, 10 * kDefaultEntries * count) << "Looks like we're in an infinite loop";
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("MultiQuery iteration {} with next_marker={}"), reps, next_marker.value_or("null")) << dendl;
    DEFINE_REQ_STATE;
    init_op(&s, kDefaultEntries, next_marker);
    op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

    op->execute(null_yield);
    reps++;
    ASSERT_EQ(op->get_ret(), 0);
    std::copy(op->items().begin(), op->items().end(), std::back_inserter(items));
    next_marker = op->return_marker();
    if (!next_marker) {
      break;
    }
  }
  // We should get the same number of items back.
  ASSERT_EQ(count, items.size());
  // ...and those items should be the same as those in the bucket.
  auto bucket_keys = bucket_object_keys();
  std::set<std::string> result_keys;
  for (const auto& item : items) {
    result_keys.insert(item.key());
  }
  ASSERT_EQ(bucket_keys, result_keys);
}

TEST_P(SQObjectlistHarness, CompoundQueryVersionedFiveVersionsNoDeletes)
{
  auto count = GetParam();
  fill_bucket_versioned(count, 5);

  std::optional<std::string> next_marker;
  int reps = 0;
  std::vector<RGWStoreQueryOp_ObjectList::item_type> items;

  while (true) {
    ASSERT_LT(reps, 10 * kDefaultEntries * count) << "Looks like we're in an infinite loop";
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("MultiQuery iteration {} with next_marker={}"), reps, next_marker.value_or("null")) << dendl;
    DEFINE_REQ_STATE;
    init_op(&s, kDefaultEntries, next_marker);
    op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

    op->execute(null_yield);
    reps++;
    ASSERT_EQ(op->get_ret(), 0);
    std::copy(op->items().begin(), op->items().end(), std::back_inserter(items));
    next_marker = op->return_marker();
    if (!next_marker) {
      break;
    }
  }
  // We should get the same number of items back.
  ASSERT_EQ(count, items.size());
  // ...and those items should be the same as those in the bucket.
  auto bucket_keys = bucket_object_keys();
  std::set<std::string> result_keys;
  for (const auto& item : items) {
    result_keys.insert(item.key());
  }
  ASSERT_EQ(bucket_keys, result_keys);
}

TEST_P(SQObjectlistHarness, CompoundQueryVersionedFiveVersionsWithDeletes)
{
  // We'll delete all the prime entries.
  std::vector<size_t> deletions = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97 };
  auto count = GetParam();
  fill_bucket_versioned(count, 5, deletions);

  std::optional<std::string> next_marker;
  int reps = 0;
  std::vector<RGWStoreQueryOp_ObjectList::item_type> items;

  while (true) {
    ASSERT_LT(reps, 10 * kDefaultEntries * count) << "Looks like we're in an infinite loop";
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("MultiQuery iteration {} with next_marker={}"), reps, next_marker.value_or("null")) << dendl;
    DEFINE_REQ_STATE;
    init_op(&s, kDefaultEntries, next_marker);
    op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

    op->execute(null_yield);
    reps++;
    ASSERT_EQ(op->get_ret(), 0);
    std::copy(op->items().begin(), op->items().end(), std::back_inserter(items));
    next_marker = op->return_marker();
    if (!next_marker) {
      break;
    }
  }
  // We should get the same number of items back.
  ASSERT_EQ(count, items.size());
  // ...and those items should be the same as those in the bucket.
  auto bucket_keys = bucket_object_keys();
  std::set<std::string> result_keys;
  for (const auto& item : items) {
    result_keys.insert(item.key());
  }
  ASSERT_EQ(bucket_keys, result_keys);
}

TEST_P(SQObjectlistHarness, CompoundQueryVersionedManyVersionsWithDeletes)
{
  // We'll delete all the prime entries.
  std::vector<size_t> deletions = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83, 89, 97 };
  auto count = GetParam();
  size_t versions = 20;
  fill_bucket_versioned(count, versions, deletions);

  std::optional<std::string> next_marker;
  int reps = 0;
  std::vector<RGWStoreQueryOp_ObjectList::item_type> items;

  while (true) {
    ASSERT_LT(reps, 2 * versions * count) << "Looks like we're in an infinite loop";
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("MultiQuery iteration {} with next_marker={}"), reps, next_marker.value_or("null")) << dendl;
    DEFINE_REQ_STATE;
    init_op(&s, kDefaultEntries, next_marker);
    op->set_list_function(std::bind(&SQObjectlistHarness::list_standard, this,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

    op->execute(null_yield);
    reps++;
    ASSERT_EQ(op->get_ret(), 0);
    std::copy(op->items().begin(), op->items().end(), std::back_inserter(items));
    next_marker = op->return_marker();
    if (!next_marker) {
      break;
    }
  }
  // We should get the same number of items back.
  ASSERT_EQ(count, items.size());
  // ...and those items should be the same as those in the bucket.
  auto bucket_keys = bucket_object_keys();
  std::set<std::string> result_keys;
  for (const auto& item : items) {
    result_keys.insert(item.key());
  }
  ASSERT_EQ(bucket_keys, result_keys);
}

INSTANTIATE_TEST_SUITE_P(StoreQuerySourceSizeParam, SQObjectlistHarness,
    ::testing::Values(1, 2, 9, 10, 11, 19, 20, 21, 99, 100, 101, 999, 1000, 1001, 1999, 2000, 2001, 9999, 10000, 10001),
    [](const ::testing::TestParamInfo<SQObjectlistHarness::ParamType>& info) {
      return fmt::format(FMT_STRING("size_{}"), info.param);
    });

/***************************************************************************/

} // anonymous namespace

int main(int argc, char** argv)
{
  auto args = argv_to_vec(argc, argv);
  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);

  // Let the caller change the library debug level.
  if (std::getenv("TEST_DEBUG")) {
    std::string err;
    int level = strict_strtol(std::getenv("TEST_DEBUG"), 10, &err);
    if (err.empty()) {
      g_ceph_context->_conf->subsys.set_log_level(ceph_subsys_rgw, std::min(level, 30));
    }
  }

  ::testing::InitGoogleTest(&argc, argv);
  int r = RUN_ALL_TESTS();
  return r;
}
