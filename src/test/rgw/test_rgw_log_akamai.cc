/**
 * @file test_rgw_log_akamai.cc
 * @author André Lucas (alucas@akamai.com)
 * @brief Unit tests for Akamai-specific usage logging functionality.
 * @version 0.1
 * @date 2025-09-03
 * 
 * @copyright Copyright (c) 2025
 * 
 */

#include <gtest/gtest.h>
#include <rgw_op.h>
#include <rgw_rest_s3.h>

#include "common/ceph_argparse.h"
#include "global/global_context.h"
#include "rgw_client_io.h"
#include "rgw_common.h"
#include "rgw_process_env.h"

// Despite clangd's assertions, the following two includes are necessary
// because there are a couple of static asserts in RGWProcessEnv that check
// that rgw::sal::LuaManager and rgw::auth::StrategyRegistry are of size > 0.
#include "rgw/rgw_sal.h"
#include "rgw_auth_registry.h"

#include "rgw/rgw_log_akamai.h"
namespace {

using namespace rgw::akamai;

/***************************************************************************/

#define DEFINE_REQ_STATE \
  RGWProcessEnv pe;      \
  RGWEnv env;            \
  req_state s(g_ceph_context, pe, &env, 0);

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
class RGWLogAkamaiUsageTest : public ::testing::Test {

protected:
  TestClient cio_;
  struct req_state* s_;
  std::unique_ptr<RGWOp> op_;
  DoutPrefixProvider* dpp_;

  template <typename T>
  void init_op(struct req_state* s)
  {
    s_ = s;
    s_->cio = &cio_;
    op_ = std::make_unique<T>();
    op_->init(nullptr, s_, nullptr);
    dpp_ = op_.get();
    s->cct->_conf->rgw_akamai_enable_usage_stats_bypass = true;
  }
}; // class RGWLogAkamaiUsageTest

/// Test basic setup.
TEST_F(RGWLogAkamaiUsageTest, SetupOp)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  // If we didn't set the op up properly, this will most likely crash. It's
  // not an exhastive test by any means, but it's good to know that logging
  // works.
  ldpp_dout(dpp_, 20) << fmt::format(FMT_STRING("Initialized op {}"), op_->name()) << dendl;
}

/// Our default configuration should disable all our programmed bypasses.
TEST_F(RGWLogAkamaiUsageTest, DefaultToDisabled)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  // Call our functions on the op.
  ASSERT_FALSE(query_usage_bypass_for_egress(&s));
  ASSERT_FALSE(query_usage_bypass_for_ingress(&s));
}

TEST_F(RGWLogAkamaiUsageTest, FetchBypassHeaderWorks)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  // Call our function to fetch the bypass header. We've not set it, so it
  // should return std::nullopt.
  auto bypass_header = fetch_bypass_header(&s);
  ASSERT_FALSE(bypass_header.has_value());

  // Set the header and try again. We have to set it via env (declared by
  // DEFINE_REQ_STATE) because the pointer saved in the req_state struct is
  // const.
  env.set(kUsageBypassHeader, "test_value");
  bypass_header = fetch_bypass_header(&s);
  ASSERT_TRUE(bypass_header.has_value());
  ASSERT_EQ(bypass_header.value(), "test_value");
}

TEST_F(RGWLogAkamaiUsageTest, ParseBypassHeaderEmpty)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  // Call our function to parse the bypass header. We've not set it, so it
  // should return std::nullopt.
  auto bypass_flags = parse_bypass_header(&s);
  ASSERT_FALSE(bypass_flags.has_value());
}

TEST_F(RGWLogAkamaiUsageTest, ParseBypassHeaderSingle)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  std::optional<bypass_flag_t> bypass_flags;

  // Set the header to an invalid value and try again.
  env.set(kUsageBypassHeader, "invalid_option");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), 0);

  // Set the header to a valid value and try again.
  env.set(kUsageBypassHeader, "no-egress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassEgressFlag);

  // Set the header to multiple valid values and try again.
  env.set(kUsageBypassHeader, "no-ingress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassIngressFlag);
}

TEST_F(RGWLogAkamaiUsageTest, ParseBypassHeaderMultiple)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  std::optional<bypass_flag_t> bypass_flags;

  // Two valid flags.
  env.set(kUsageBypassHeader, "no-egress,no-ingress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassEgressFlag | kUsageBypassIngressFlag);

  // Two valid flags and an invalid flag (should ignore the invalid one).
  env.set(kUsageBypassHeader, "no-egress,no-ingress,invalid_option");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassEgressFlag | kUsageBypassIngressFlag);
}

TEST_F(RGWLogAkamaiUsageTest, ParseBypassHeaderDegenerateSpacing)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  std::optional<bypass_flag_t> bypass_flags;

  // Comma-space.
  env.set(kUsageBypassHeader, "no-egress, no-ingress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassEgressFlag | kUsageBypassIngressFlag);

  // Leading space.
  env.set(kUsageBypassHeader, " no-egress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassEgressFlag);

  // Comma-space with an empty token in the middle.
  env.set(kUsageBypassHeader, "no-egress, , no-ingress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassEgressFlag | kUsageBypassIngressFlag);

  // Repeated commas with an empty token in the middle.
  env.set(kUsageBypassHeader, "no-egress,,no-ingress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassEgressFlag | kUsageBypassIngressFlag);

  // Leading comma then token
  env.set(kUsageBypassHeader, ",no-egress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), kUsageBypassEgressFlag);

  // Leading comma only.
  env.set(kUsageBypassHeader, ",");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), 0);

  // Leading comma and invalid.
  env.set(kUsageBypassHeader, ",invalid_token");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), 0);
}

TEST_F(RGWLogAkamaiUsageTest, ParseBypassHeaderInvalidChars)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  std::optional<bypass_flag_t> bypass_flags;

  // Invalid UTF-8 leading.
  env.set(kUsageBypassHeader, "\u{00fe}no-egress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), 0);

  // Invalid UTF-8 trailing.
  env.set(kUsageBypassHeader, "no-egress\u{00fe}");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), 0);

  // Invalid UTF-8 inside.
  env.set(kUsageBypassHeader, "no_\u{00fe}egress");
  bypass_flags = parse_bypass_header(&s);
  ASSERT_TRUE(bypass_flags.has_value());
  ASSERT_EQ(bypass_flags.value(), 0);
}

TEST_F(RGWLogAkamaiUsageTest, QueryBypassEgress)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  // Should be false by default.
  ASSERT_FALSE(query_usage_bypass_for_egress(&s));

  // Set the header to an invalid value and try again.
  env.set(kUsageBypassHeader, "invalid_option");
  ASSERT_FALSE(query_usage_bypass_for_egress(&s));

  // Set the header to a valid value and try again.
  env.set(kUsageBypassHeader, "no-egress");
  ASSERT_TRUE(query_usage_bypass_for_egress(&s));

  // Set the header to multiple valid values and try again.
  env.set(kUsageBypassHeader, "no-ingress");
  ASSERT_FALSE(query_usage_bypass_for_egress(&s));

  // Two valid flags.
  env.set(kUsageBypassHeader, "no-egress,no-ingress");
  ASSERT_TRUE(query_usage_bypass_for_egress(&s));
}

TEST_F(RGWLogAkamaiUsageTest, QueryBypassIngress)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  // Should be false by default.
  ASSERT_FALSE(query_usage_bypass_for_ingress(&s));

  // Set the header to an invalid value and try again.
  env.set(kUsageBypassHeader, "invalid_option");
  ASSERT_FALSE(query_usage_bypass_for_ingress(&s));

  // Set the header to a valid value and try again.
  env.set(kUsageBypassHeader, "no-ingress");
  ASSERT_TRUE(query_usage_bypass_for_ingress(&s));

  // Set the header to multiple valid values and try again.
  env.set(kUsageBypassHeader, "no-egress");
  ASSERT_FALSE(query_usage_bypass_for_ingress(&s));

  // Two valid flags.
  env.set(kUsageBypassHeader, "no-egress,no-ingress");
  ASSERT_TRUE(query_usage_bypass_for_ingress(&s));
}

TEST_F(RGWLogAkamaiUsageTest, QueryBypass)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);

  auto bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, 0);

  // Set the header to an invalid value and try again.
  env.set(kUsageBypassHeader, "invalid_option");
  bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, 0);
  // Set the header to a valid value and try again.
  env.set(kUsageBypassHeader, "no-egress");
  bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, kUsageBypassEgressFlag);
  // Set the header to a different valid values and try again.
  env.set(kUsageBypassHeader, "no-ingress");
  bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, kUsageBypassIngressFlag);
  // Two valid flags.
  env.set(kUsageBypassHeader, "no-egress,no-ingress");
  bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, kUsageBypassEgressFlag | kUsageBypassIngressFlag);
}

TEST_F(RGWLogAkamaiUsageTest, QueryBypassDoesNothingWhenDisabled)
{
  DEFINE_REQ_STATE;

  // Set up a get-object operation.
  init_op<RGWGetObj_ObjStore_S3>(&s);
  // Turn the feature off. This is the default, the test harness turns it on
  // in order to test it.
  s.cct->_conf->rgw_akamai_enable_usage_stats_bypass = false;

  auto bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, 0);

  // Set the header to an invalid value and try again.
  env.set(kUsageBypassHeader, "invalid_option");
  bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, 0);
  // Set the header to a valid value and try again.
  env.set(kUsageBypassHeader, "no-egress");
  bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, 0);
  // Set the header to a different valid values and try again.
  env.set(kUsageBypassHeader, "no-ingress");
  bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, 0);
  // Two valid flags.
  env.set(kUsageBypassHeader, "no-egress,no-ingress");
  bypass_flags = query_usage_bypass(&s);
  ASSERT_EQ(bypass_flags, 0);
}

/***************************************************************************/

} // anonymous namespace

int main(int argc, char** argv)
{
  auto args = argv_to_vec(argc, argv);
  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  // g_ceph_context->_conf->subsys.set_log_level(ceph_subsys_rgw, 20);

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
