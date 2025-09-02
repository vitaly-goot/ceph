// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest-param-test.h>
#include <gtest/gtest.h>
#include <initializer_list>
#include <stdexcept>

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

#include "test_rgw_storequery_util.h"

namespace {

using namespace std::string_literals;

using namespace rgw;

using namespace storequery_util;

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

/**
 * @brief Common code for the versioned and nonversioned test harnesses for
 * objectlist.
 */
class SQObjectListHarnessBase {
protected:
  /// The default number of entries to return in a list operation. When
  /// debugging, it will help you *a lot* to reduce this to a much smaller
  /// number, like 10.
  static constexpr uint64_t kDefaultEntries = 1000;
  // static constexpr uint64_t kDefaultEntries = 10;

  RGWStoreQueryOp_ObjectList_Unittest* op = nullptr;

protected:
  req_state* s_;
  TestClient cio_;

  std::shared_ptr<DoutPrefix> dpp_;
  // It just looks weird to not have 'dpp' as the pointer.
  DoutPrefixProvider* dpp;

  using SALBucket = rgw::sal::Bucket;

public:
  BucketDirSim sim_;

protected:
  void init_op(req_state* s, uint64_t max_entries, std::optional<std::string> marker)
  {
    s_ = s;
    s_->cio = &cio_;
    op = new RGWStoreQueryOp_ObjectList_Unittest(max_entries, marker);
    op->set_req_state(s_);
    // By default, have the list function throw an exception. That way,
    // forgetting to set the list function should be hard to do.
    op->set_list_function(std::bind(&BucketDirSim::list_always_throw, &sim_,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
    op->init(nullptr, s_, nullptr);
    ldpp_dout(dpp, 1) << "op configured" << dendl;
  }

  // Quickly initialise the source bucket from an existing vector, destroying
  // the original vector.
  void move_bucket(std::vector<SrcKey>&& keys)
  {
    sim_.set_bucket(std::move(keys));
  }

  // Allow tests to index the source bucket.
  const std::vector<SrcKey>& src_bucket() const
  {
    return sim_.get_bucket();
  }

}; // class StoreQueryObjectListHarness

/**
 * @brief Nonversioned harness, parameterised solely by bucket size.
 *
 * It's still using std::tuple<> simply to minimise the diff to the versioned
 * harness, and maybe to make it easier to extend later.
 */
class SQObjectlistHarnessNonversioned : public SQObjectListHarnessBase, public testing::TestWithParam<std::tuple<size_t>> {

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

}; // class StoreQueryObjectListHarness

TEST_F(SQObjectlistHarnessNonversioned, MetaHarnessDefaults)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  // The default setup sets the list function to list_always_throw.
  ASSERT_THROW(op->execute(null_yield), std::runtime_error);
}

TEST_F(SQObjectlistHarnessNonversioned, MetaAlwaysFail)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&BucketDirSim::list_always_fail, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), -ERR_INTERNAL_ERROR);
}

// This catches a meaningful special case where the bucket is empty. Make sure
// we exit the loop properly when there are no items at all.
TEST_F(SQObjectlistHarnessNonversioned, MetaAlwaysEmpty)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&BucketDirSim::list_always_empty, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 0U);
}

TEST_F(SQObjectlistHarnessNonversioned, MetaStdEmpty)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 0U);
}

// Check that the token works the way we expect.
TEST_F(SQObjectlistHarnessNonversioned, MetaTokenOrdering)
{
  DEFINE_REQ_STATE;
  sim_.fill_bucket_nonversioned(10);

  SrcKey search("obj000002x");
  SrcKey prev("obj000001");
  SrcKey next("obj000003");
  ASSERT_LT(prev, search);
  ASSERT_GT(next, search);
  ASSERT_EQ(search, search);

  // A token 'in between' obj000002 and obj000003.
  rgw_obj_key search_key("obj000002x");
  auto token = to_base64(RGWStoreQueryOp_ObjectList::create_continuation_token(dpp, search_key));
  ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("continuation token: {}"), token) << dendl;

  init_op(&s, kDefaultEntries, token);
  op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_GE(op->items().size(), 1U);
  auto first = op->items()[0];
  ASSERT_EQ(first.key(), "obj000003");
}

// Check the first page of potentially paginated output.
TEST_P(SQObjectlistHarnessNonversioned, StdNonVersionedFirstPage)
{
  size_t count = std::get<0>(GetParam());
  sim_.fill_bucket_nonversioned(count);

  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

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
      auto opt_token_marker = RGWStoreQueryOp_ObjectList::read_continuation_token(dpp, token_json);
      ASSERT_TRUE(opt_token_marker != std::nullopt);
      ASSERT_EQ(last.key(), opt_token_marker->name);
    }
  }
}

// Check the last page of paginated output. It's easy to predict what the
// continuation token will be for nonversioned buckets.
TEST_P(SQObjectlistHarnessNonversioned, StdNonVersionedLastPage)
{
  size_t count = std::get<0>(GetParam());
  sim_.fill_bucket_nonversioned(count);

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
  auto marker_src_key = sim_.get_bucket()[last_page_index - 1];
  auto marker_obj_key = marker_src_key.to_marker();
  auto token = RGWStoreQueryOp_ObjectList::create_continuation_token(dpp, marker_obj_key);
  auto token_b64 = to_base64(token);

  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, token_b64);
  op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
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
  auto first_item = sim_.get_bucket()[last_page_index];
  ASSERT_EQ(op->items()[0].key(), first_item.to_marker());

  // Check for exact-page-boundary special case.
  if (last_page_size == kDefaultEntries) {
    // If we have an exact page boundary, we shouldn't return a marker.
    ASSERT_TRUE(op->return_marker() == std::nullopt);
  }
}

// Test that a sequence of calls to objectlist (including in most cases
// pagination) results in the proper list of keys. First for a nonversioned bucket.
TEST_P(SQObjectlistHarnessNonversioned, CompoundQueryNonversioned)
{
  auto count = std::get<0>(GetParam());
  sim_.fill_bucket_nonversioned(count);

  std::optional<std::string> next_marker;
  int reps = 0;
  std::vector<RGWStoreQueryOp_ObjectList::item_type> items;

  while (true) {
    ASSERT_LT(reps, 10 * kDefaultEntries * count) << "Looks like we're in an infinite loop";
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("MultiQuery iteration {} with next_marker={}"), reps, next_marker.value_or("null")) << dendl;
    DEFINE_REQ_STATE;
    init_op(&s, kDefaultEntries, next_marker);
    op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
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
  auto bucket_keys = sim_.bucket_object_keys();
  std::set<std::string> result_keys;
  for (const auto& item : items) {
    result_keys.insert(item.key());
  }
  ASSERT_EQ(bucket_keys, result_keys);
}

INSTANTIATE_TEST_SUITE_P(SQObjectlistSourceSizeParamNonversioned, SQObjectlistHarnessNonversioned,
    ::testing::Combine(
        ::testing::Values(1, 2, 9, 10, 11, 99, 100, 101, 999, 1000, 1001, 1999, 2000, 2001, 9999, 10000, 10001)),
    [](const ::testing::TestParamInfo<SQObjectlistHarnessNonversioned::ParamType>& info) {
      return fmt::format(FMT_STRING("size_{}"), std::get<0>(info.param));
    });

/**
 * @brief Versioned harness, parameterised by bucket size and number of
 * versions of each object to test.
 */
class SQObjectlistHarnessVersioned : public SQObjectListHarnessBase, public testing::TestWithParam<std::tuple<size_t, size_t>> {

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

}; // class StoreQueryObjectListHarness

TEST_F(SQObjectlistHarnessVersioned, StdVersionedOneItemFiveVersions)
{
  // One item with five versions, only the last item will be current.
  sim_.fill_bucket_versioned(1, 5);

  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 1U);
}

TEST_F(SQObjectlistHarnessVersioned, StdVersionedOneItemFiveVersionsWithOneDeleted)
{
  // One item with five versions, only the last item will be current.
  sim_.fill_bucket_versioned(1, 5, { 0 });

  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 1U);
}

// Check the first page of potentially paginated output.
TEST_P(SQObjectlistHarnessVersioned, StdVersionedFirstPage)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5));

  size_t count = std::get<0>(GetParam());
  size_t versions = std::get<1>(GetParam());
  sim_.fill_bucket_versioned(count, versions);

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

// Test that a sequence of calls to objectlist (including in most cases
// pagination) results in the proper list of keys. Versioned bucket this time.
TEST_P(SQObjectlistHarnessVersioned, CompoundQueryVersionedNoDeletes)
{
  auto count = std::get<0>(GetParam());
  auto versions = std::get<1>(GetParam());
  sim_.fill_bucket_versioned(count, versions);

  std::optional<std::string> next_marker;
  int reps = 0;
  std::vector<RGWStoreQueryOp_ObjectList::item_type> items;

  while (true) {
    ASSERT_LT(reps, 10 * kDefaultEntries * count) << "Looks like we're in an infinite loop";
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("MultiQuery iteration {} with next_marker={}"), reps, next_marker.value_or("null")) << dendl;
    DEFINE_REQ_STATE;
    init_op(&s, kDefaultEntries, next_marker);
    op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
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
  auto bucket_keys = sim_.bucket_object_keys();
  std::set<std::string> result_keys;
  for (const auto& item : items) {
    result_keys.insert(item.key());
  }
  ASSERT_EQ(bucket_keys, result_keys);
}

constexpr bool is_prime(size_t n)
{
  if (n <= 1)
    return false;
  for (int i = 2; i * i <= n; ++i) {
    if (n % i == 0)
      return false;
  }
  return true;
}

constexpr std::array<size_t, 1229> generate_primes()
{
  std::array<size_t, 1229> primes = {};
  int index = 0;
  for (int i = 2; i <= 10000; ++i) {
    if (is_prime(i)) {
      primes[index++] = i;
    }
  }
  return primes;
}

// Test that a sequence of calls to objectlist (including in most cases
// pagination) results in the proper list of keys. This time a versioned
// bucket with a number of keys deleted.
constexpr auto primes_under_10000 = generate_primes();
TEST_P(SQObjectlistHarnessVersioned, CompoundQueryVersionedWithDeletes)
{
  // We'll delete all the prime entries.
  std::vector<size_t> deletions;
  // deletions.reserve(primes_under_10000.size());
  deletions.insert(deletions.end(), primes_under_10000.begin(), primes_under_10000.end());

  auto count = std::get<0>(GetParam());
  auto versions = std::get<1>(GetParam());
  sim_.fill_bucket_versioned(count, versions, deletions);

  std::optional<std::string> next_marker;
  int reps = 0;
  std::vector<RGWStoreQueryOp_ObjectList::item_type> items;

  while (true) {
    ASSERT_LT(reps, 10 * kDefaultEntries * count) << "Looks like we're in an infinite loop";
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("MultiQuery iteration {} with next_marker={}"), reps, next_marker.value_or("null")) << dendl;
    DEFINE_REQ_STATE;
    init_op(&s, kDefaultEntries, next_marker);
    op->set_list_function(std::bind(&BucketDirSim::list_standard, &sim_,
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
  auto bucket_keys = sim_.bucket_object_keys();
  std::set<std::string> result_keys;
  for (const auto& item : items) {
    result_keys.insert(item.key());
  }
  ASSERT_EQ(bucket_keys, result_keys);
}

INSTANTIATE_TEST_SUITE_P(SQObjectlistSourceSizeParamVersioned, SQObjectlistHarnessVersioned,
    ::testing::Combine(
        ::testing::Values(1, 2, 9, 10, 11, 99, 100, 101, 999, 1000, 1001, 1999, 2000, 2001, 9999, 10000, 10001),
        ::testing::Values(1, 2, 5)),
    [](const ::testing::TestParamInfo<SQObjectlistHarnessVersioned::ParamType>& info) {
      return fmt::format(FMT_STRING("size_{}_versions_{}"), std::get<0>(info.param), std::get<1>(info.param));
    });

/***************************************************************************/

/* mpuploadlist test harness */

// Use DEFINE_REQ_STATE and BasicClient from the objectlist harness.

class SQMpuploadlistHarness : public testing::TestWithParam<std::tuple<size_t, size_t>> {
protected:
  /// The default number of entries to return in a list operation. When
  /// debugging, it will help you *a lot* to reduce this to a much smaller
  /// number, like 10.
  static constexpr uint64_t kDefaultEntries = 1000;
  // static constexpr uint64_t kDefaultEntries = 10;

  RGWStoreQueryOp_MPUploadList_Unittest* op = nullptr;

protected:
  req_state* s_;
  TestClient cio_;

  std::shared_ptr<DoutPrefix> dpp_;
  // It just looks weird to not have 'dpp' as the pointer.
  DoutPrefixProvider* dpp;

  using SALBucket = rgw::sal::Bucket;

public:
  MpuBucketDirSim sim_;

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
    op = new RGWStoreQueryOp_MPUploadList_Unittest(max_entries, marker);
    op->set_req_state(s_);
    // By default, have the list function throw an exception. That way,
    // forgetting to set the list function should be hard to do.
    op->set_list_multiparts_function(std::bind(&MpuBucketDirSim::list_multiparts_always_throw, &sim_,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
        std::placeholders::_4, std::placeholders::_5, std::placeholders::_6,
        std::placeholders::_7, std::placeholders::_8));
    op->init(nullptr, s_, nullptr);
    ldpp_dout(dpp, 1) << "op configured" << dendl;
  }

  // Quickly initialise the source bucket from an existing vector, destroying
  // the original vector.
  void move_bucket(std::vector<MpuSrcKey>&& keys)
  {
    sim_.set_bucket(std::move(keys));
  }

  // Allow tests to index the source bucket.
  const std::vector<MpuSrcKey>& src_bucket() const
  {
    return sim_.get_bucket();
  }

}; // class StoreQueryObjectListHarness

TEST_F(SQMpuploadlistHarness, MetaHarnessDefaults)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);

  // Run the operation.
  ASSERT_THROW(op->execute(null_yield), std::runtime_error);
}

TEST_F(SQMpuploadlistHarness, MetaHarnessAlwaysFail)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_multiparts_function(std::bind(&MpuBucketDirSim::list_multiparts_always_fail, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
      std::placeholders::_4, std::placeholders::_5, std::placeholders::_6,
      std::placeholders::_7, std::placeholders::_8));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), -ERR_INTERNAL_ERROR);
}

TEST_F(SQMpuploadlistHarness, MetaStdEmpty)
{
  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_multiparts_function(std::bind(&MpuBucketDirSim::list_multiparts_standard, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
      std::placeholders::_4, std::placeholders::_5, std::placeholders::_6,
      std::placeholders::_7, std::placeholders::_8));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  ASSERT_EQ(op->items().size(), 0U);
}

// Check the first page of potentially paginated output. Allows for multiple
// uploads on the same key.
TEST_P(SQMpuploadlistHarness, StdFirstPage)
{
  size_t uploads_per_key = std::get<1>(GetParam());

  size_t count = std::get<0>(GetParam());
  sim_.fill_bucket(count, uploads_per_key);

  DEFINE_REQ_STATE;
  init_op(&s, kDefaultEntries, std::nullopt);
  op->set_list_multiparts_function(std::bind(&MpuBucketDirSim::list_multiparts_standard, &sim_,
      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
      std::placeholders::_4, std::placeholders::_5, std::placeholders::_6,
      std::placeholders::_7, std::placeholders::_8));
  op->execute(null_yield);
  ASSERT_EQ(op->get_ret(), 0);
  // We should never return more results than the max.
  ASSERT_LE(op->items().size(), kDefaultEntries);
  // If we requested <= kDefaultItems entries, we should match exactly and we
  // should see the EOF flag.
  // This catches important special-case handling of the marker. If we're on
  // an exact page boundary we don't set the marker.
  if (count * uploads_per_key <= kDefaultEntries) {
    ASSERT_EQ(op->items().size(), std::min(count * uploads_per_key, kDefaultEntries));
    ASSERT_TRUE(op->seen_eof());
    ASSERT_TRUE(op->return_marker() == std::nullopt);

  } else {
    // If there were more than kDefaultItems entries, we shouldn't see EOF.
    ASSERT_FALSE(op->seen_eof());
    auto last = op->items()[op->items().size() - 1];
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("last item: key={}"), last.key()) << dendl;
    auto opt_token = op->return_marker();
    ASSERT_TRUE(opt_token != std::nullopt);
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("continuation token: {}"), *opt_token) << dendl;

    if (opt_token.has_value()) {
      // If this base64 decode throws, the test will fail.
      auto token_json = from_base64(*opt_token);
      auto opt_token_marker = RGWStoreQueryOp_MPUploadList::read_continuation_token(dpp, token_json);
      ASSERT_TRUE(opt_token_marker != std::nullopt);
      ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("continuation token: {}"), *opt_token_marker) << dendl;
    }
  }
}

// Test that a sequence of calls to mpuploadlist (including in most cases
// pagination) results in the proper list of keys. Allows for multiple uploads
// on the same key.
TEST_P(SQMpuploadlistHarness, CompoundQuery)
{
  auto count = std::get<0>(GetParam());
  auto uploads_per_version = std::get<1>(GetParam());
  sim_.fill_bucket(count, uploads_per_version);

  std::optional<std::string> next_marker;
  int reps = 0;
  std::vector<RGWStoreQueryOp_MPUploadList::item_type> items;

  while (true) {
    ASSERT_LT(reps, 10 * kDefaultEntries * count) << "Looks like we're in an infinite loop";
    ldpp_dout(dpp, 20) << fmt::format(FMT_STRING("MultiQuery iteration {} with next_marker={}"), reps, next_marker.value_or("null")) << dendl;
    DEFINE_REQ_STATE;
    init_op(&s, kDefaultEntries, next_marker);
    op->set_list_multiparts_function(std::bind(&MpuBucketDirSim::list_multiparts_standard, &sim_,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
        std::placeholders::_4, std::placeholders::_5, std::placeholders::_6,
        std::placeholders::_7, std::placeholders::_8));

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
  ASSERT_EQ(count * uploads_per_version, items.size());
  // ...and those items should be the same as those in the bucket.
  auto bucket_keys = sim_.bucket_object_keys();
  std::set<std::string> result_keys;
  for (const auto& item : items) {
    result_keys.insert(item.key());
  }
  ASSERT_EQ(bucket_keys, result_keys);

  // Same again, but with markers == <key>.<upload_id>.meta.
  auto bucket_markers = sim_.bucket_object_markers();
  std::set<std::string> result_markers;
  for (const auto& item : items) {
    result_markers.insert(item.make_marker());
  }
  ASSERT_EQ(bucket_markers, result_markers);
}

INSTANTIATE_TEST_SUITE_P(SQMpuloadlistUploadsSizeParam, SQMpuploadlistHarness,
    ::testing::Combine(
        ::testing::Values(1, 2, 9, 10, 11, 99, 100, 101, 999, 1000, 1001, 1999, 2000, 2001, 9999, 10000, 10001),
        ::testing::Values(1, 2, 5)),
    [](const ::testing::TestParamInfo<SQMpuploadlistHarness::ParamType>& info) {
      return fmt::format(FMT_STRING("size_{}_uploads_{}"), std::get<0>(info.param), std::get<1>(info.param));
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
