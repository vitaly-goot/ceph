// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

/**
 * @file rgw_log_akamai.h
 * @author André Lucas (alucas@akamai.com)
 * @brief Header file for Akamai-specific logging functionality
 * @version 0.1
 * @date 2025-09-03
 * 
 * @copyright Copyright (c) 2025
 */

#pragma once

#include "rgw_common.h"

namespace rgw::akamai {

static constexpr char kUsageBypassHeader[] = "HTTP_X_RGW_AKAMAI_USAGE_STATS_BYPASS";

using bypass_flag_t = uint64_t;
static constexpr bypass_flag_t kUsageBypassEgressFlag = 1 << 0;
static constexpr bypass_flag_t kUsageBypassIngressFlag = 1 << 1;

static constexpr bypass_flag_t kUsageBypassAllFlags = kUsageBypassEgressFlag | kUsageBypassIngressFlag;

// This is the top-level API we're expecting to use in production code.

/**
 * @brief If the bypass feature is enabled, return true if the request has an
 * HTTP header indicating that egress usage logging should be bypassed.
 *
 * Calls query_usage_bypass() internally, so follows the same rules.
 *
 * @param s The current request.
 * @return true Egress logging should be bypassed.
 * @return false Egress logging should not be bypassed.
 */
bool query_usage_bypass_for_egress(const struct req_state* s);

/**
 * @brief If the bypass feature is enabled, return true if the request has an
 * HTTP header indicating that ingress usage logging should be bypassed.
 *
 * Calls query_usage_bypass() internally, so follows the same rules.
 *
 * @param s The current request.
 * @return true Ingress logging should be bypassed.
 * @return false Ingress logging should not be bypassed.
 */
bool query_usage_bypass_for_ingress(const struct req_state* s);

/**
 * @brief If the bypass feature is enabled, returns the usage bypass flags for
 * the request. If no header is present or there was an problem parsing it,
 * return
 * 0.
 *
 * If the configuration option rgw_akamai_enable_usage_stats_bypass is false
 * (the default), always return 0.
 *
 * @param s The current request.
 * @return bypass_flag_t The usage bypass flags.
 */
bypass_flag_t query_usage_bypass(const struct req_state* s);

// These APIs are exposed for unit tests.

/**
 * @brief Given request \p s, fetch the value of the usage bypass header if
 * present, otherwise return std::nullopt.
 *
 * @param s The current request.
 * @return std::optional<std::string_view> The header value, or std::nullopt if
 * the header is not present.
 */
std::optional<std::string_view> fetch_bypass_header(const struct req_state* s);

/**
 * @brief Given request \p s, fetch and parse the header value and return the
 * corresponding bypass flags.
 *
 * @param s The current request.
 * @return std::optional<bypass_flag_t> The parsed bypass flags, or
 * std::nullopt if the header value is invalid.
 */
std::optional<bypass_flag_t> parse_bypass_header(const struct req_state* s);

} // namespace rgw::akamai
