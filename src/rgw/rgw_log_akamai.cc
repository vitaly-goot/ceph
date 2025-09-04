// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab ft=cpp

/**
 * @file rgw_log_akamai.cc
 * @author André Lucas (alucas@akamai.com)
 * @brief Implementation file for Akamai-specific logging functionality
 * @version 0.1
 * @date 2025-09-03
 * 
 * @copyright Copyright (c) 2025
 * 
 */

#include "rgw_log_akamai.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/tokenizer.hpp>
#include <boost/utility/string_view.hpp>
#include <string_view>
#include <unordered_map>

#define dout_subsys ceph_subsys_rgw

namespace rgw::akamai {

/***************************************************************************/

bool query_usage_bypass_for_egress(const struct req_state* s)
{
  auto flags = query_usage_bypass(s);
  return (flags & kUsageBypassEgressFlag) != 0;
}

bool query_usage_bypass_for_ingress(const struct req_state* s)
{
  auto flags = query_usage_bypass(s);
  return (flags & kUsageBypassIngressFlag) != 0;
}

bypass_flag_t query_usage_bypass(const struct req_state* s)
{
  assert(s != nullptr);
  assert(s->info.env != nullptr);
  if (s->cct->_conf->rgw_akamai_enable_usage_stats_bypass == false) {
    return 0;
  }
  auto flags = parse_bypass_header(s);
  return flags.value_or(0);
}

/***************************************************************************/

std::optional<std::string_view> fetch_bypass_header(const struct req_state* s)
{
  assert(s != nullptr);
  assert(s->info.env != nullptr);
  // Unfortunately it's a boost::optional and not a std::optional.
  auto opt_hdr = s->info.env->get_optional(kUsageBypassHeader);
  if (opt_hdr.has_value()) {
    // It's safe to return a string_view because s->info.env is const for the
    // duration of the request, therefore the underlying string object's
    // lifetime exceeds that of the string_view.
    return std::string_view(opt_hdr.value());
  }
  return std::nullopt;
}

struct bypass_option {
  bypass_flag_t flag;
};

static std::unordered_map<std::string, bypass_option> bypass_options = {
  { "no-egress", { kUsageBypassEgressFlag } },
  { "no-ingress", { kUsageBypassIngressFlag } },
};

std::optional<bypass_flag_t> parse_bypass_header(const struct req_state* s)
{
  auto opt_hdr = fetch_bypass_header(s);
  if (!opt_hdr.has_value()) {
    return std::nullopt;
  }
  std::string hdr_lc(*opt_hdr);
  boost::to_lower(hdr_lc);
  bypass_flag_t out_token = 0;

  typedef boost::tokenizer<boost::char_separator<char>> tokenizer;
  boost::char_separator<char> sep { "," };
  tokenizer tok(hdr_lc, sep);

  for (auto token : tok) {
    boost::trim(token);
    if (token.empty()) {
      continue;
    }
    auto found = bypass_options.find(token);
    if (found != bypass_options.end()) {
      out_token |= found->second.flag;
    } else {
      ldout(s->cct, 5) << "rgw::akamai::parse_bypass_header: unknown bypass option '" << token << "'" << dendl;
    }
  }
  return out_token;
}

/***************************************************************************/

} // namespace rgw::akamai
