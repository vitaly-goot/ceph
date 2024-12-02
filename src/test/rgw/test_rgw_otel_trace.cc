/**
 * @file test_rgw_otel_trace.cc
 * @author André Lucas (alucas@akamai.com)
 * @brief Unit tests for OpenTelemetry-related functions.
 * @version 0.1
 * @date 2024-11-27
 *
 * @copyright Copyright (c) 2024
 *
 */

#include <memory>

#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "common/ceph_argparse.h"
#include "common/common_init.h"
#include "common/dout.h"
#include "global/global_init.h"
#include "rgw/rgw_process.h"

namespace {

class GetTraceIdFromTraceparent : public ::testing::Test {
protected:
  std::shared_ptr<DoutPrefix> dpp_;
  DoutPrefixProvider* dpp;

public:
  void SetUp() override
  {
    dpp_ = std::make_shared<DoutPrefix>(g_ceph_context, ceph_subsys_rgw, "unittest ");
    dpp = dpp_.get();
    ldpp_dout(dpp, 0) << "SetUp" << dendl;
  }
};

// Test when the HTTP_TRACEPARENT header is not set.
TEST_F(GetTraceIdFromTraceparent, NoHeader)
{
  RGWEnv env;
  std::string traceparent;
  auto opt_traceid = get_traceid_from_traceparent(dpp, env);
  ASSERT_THAT(opt_traceid, ::testing::Eq(std::nullopt));
}

// Test when the HTTP_TRACEPARENT header is set to a valid value.
TEST_F(GetTraceIdFromTraceparent, ValidHeader)
{
  RGWEnv env;
  env.set("HTTP_TRACEPARENT", "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01");
  std::string traceparent;
  auto opt_traceid = get_traceid_from_traceparent(dpp, env);
  ASSERT_THAT(opt_traceid, ::testing::Ne(std::nullopt));
  ASSERT_EQ(*opt_traceid, "0123456789abcdef0123456789abcdef");
}

// Test when the HTTP_TRACEPARENT header contains a bogus character.
TEST_F(GetTraceIdFromTraceparent, HeaderBogusCharacter)
{
  RGWEnv env;
  env.set("HTTP_TRACEPARENT", "00-x123456789abcdef0123456789abcdef-0123456789abcdef-01");
  std::string traceparent;
  auto opt_traceid = get_traceid_from_traceparent(dpp, env);
  ASSERT_THAT(opt_traceid, ::testing::Eq(std::nullopt));
}

// Test when the HTTP_TRACEPARENT header is set but the trace ID is too long.
TEST_F(GetTraceIdFromTraceparent, TraceIdTooLong)
{
  RGWEnv env;
  env.set("HTTP_TRACEPARENT", "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01-");
  std::string traceparent;
  auto opt_traceid = get_traceid_from_traceparent(dpp, env);
  ASSERT_THAT(opt_traceid, ::testing::Eq(std::nullopt));
}

// Test when the HTTP_TRACEPARENT header is set but the trace ID is too short.
TEST_F(GetTraceIdFromTraceparent, TraceIdTooShort)
{
  RGWEnv env;
  env.set("HTTP_TRACEPARENT", "00-0123456789abcdef0123456789abcdef-0123456789abcdef-0");
  std::string traceparent;
  auto opt_traceid = get_traceid_from_traceparent(dpp, env);
  ASSERT_THAT(opt_traceid, ::testing::Eq(std::nullopt));
}

} // namespace (anonymous)

// main() cribbed from test_rgw_auth_handoff.cc

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
