/**
 * @file rgw_handoff_authz.h
 * @author André Lucas (alucas@akamai.com)
 * @brief Forward declarations for Authn/Authz Handoff.
 * @version 0.1
 * @date 2024-07-26
 *
 * @copyright Copyright (c) 2024
 *
 * Set up forward declarations so we can at least point to Handoff things
 * without getting into a big circular dependency mess in rgw_common.h .
 */

#pragma once

namespace rgw {

class HandoffAuthzState;
class HandoffHelper;

} // namespace rgw
