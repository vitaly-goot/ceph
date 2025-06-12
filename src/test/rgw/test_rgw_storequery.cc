// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/ceph_argparse.h"
#include "common/dout.h"
#include "global/global_context.h"
#include "rgw/rgw_rest_storequery.h"

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
  ASSERT_TRUE(p.parse(&dpp, "objectlist 666 cmh1YmFyYgo=", RGWSQHandlerType::Bucket));
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
  ASSERT_TRUE(p.parse(&dpp, "mpuploadlist 666 cmh1YmFyYgo=", RGWSQHandlerType::Bucket));
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

} // anonymous namespace

int main(int argc, char** argv)
{
  auto args = argv_to_vec(argc, argv);
  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  // g_ceph_context->_conf->subsys.set_log_level(ceph_subsys_rgw, 20);
  ::testing::InitGoogleTest(&argc, argv);
  int r = RUN_ALL_TESTS();
  return r;
}
