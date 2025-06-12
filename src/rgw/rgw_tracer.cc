// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "rgw_tracer.h"
#include "global/global_context.h"
#include <string>

#ifdef HAVE_JAEGER
#include "opentelemetry/trace/experimental_semantic_conventions.h"
#endif // HAVE_JAEGER

namespace tracing {
namespace rgw {

tracing::Tracer tracer;

} // namespace rgw
} // namespace tracing

/**
 * @brief Get the OpenTelemetery trace ID from the HTTP traceparent header in
 * \p env.
 *
 * This assumes RGWEnv is set up as would be for process_request(). The
 * traceparent header will be in \p env as 'HTTP_TRACEPARENT'. The format is
 * strictly defined here:
 *   https://uptrace.dev/opentelemetry/opentelemetry-traceparent.html
 *
 * Go to reasonable lengths to ensure the header is well-formed and safe to
 * include in a log file. If the header is not present, or is malformed,
 * return std::nullopt. Otherwise, return the trace ID as a string.
 *
 * @param dpp DoutPrefixProvider. May not be nullptr.
 * @param env An RGWEnv reference, containing the HTTP headers.
 * @return std::optional<std::string> A trace ID as string, otherwise
 * std::nullopt. Errors will be sent to \p dpp at level 1.
 */
std::optional<std::string> get_traceid_from_traceparent(DoutPrefixProvider* dpp, const std::string& traceparent)
{
  static constexpr size_t tp_expected_len = 55;

  ceph_assert(dpp != nullptr);

  // Strictly enforce the header length.
  if (traceparent.size() != tp_expected_len) {
    ldpp_dout(dpp, 1) << fmt::format(FMT_STRING("TRACEPARENT header length {} != expected length {}"), traceparent.size(), tp_expected_len) << dendl;
    return std::nullopt;
  }
  // Only hex digits and hyphens are valid.
  if (!std::all_of(traceparent.begin(), traceparent.end(), [](char c) { return std::isxdigit(c) || c == '-'; })) {
    ldpp_dout(dpp, 1) << fmt::format(FMT_STRING("TRACEPARENT header contents invalid")) << dendl;
    return std::nullopt;
  }
  // Just return the substring. Whilst the trace ID may still be invalid, it
  // is at least safe to output to a log file.
  return traceparent.substr(3, 32);
}

/**
 * @brief Set additional attributes on a trace using request information.
 *
 * Note that we try not to add stuff that might help attackers here, unless
 * jaeger_tracing_verbose_attributes is set (indicating that we're trying to
 * debug something). When adding attributes, consider whether or not it should
 * be added by default.
 *
 * @param s The req_state.
 * @param span A configured span.
 */
void set_extra_trace_attributes(const req_state* s, jspan span)
{
#ifdef HAVE_JAEGER
  if (!s->trace_enabled) {
    return;
  }
  using namespace opentelemetry::trace;
  span->SetAttribute(OTEL_GET_TRACE_ATTR(AttrHttpRequestContentLength), s->content_length);
  if (s->info.method) { // _Should_ never be null, req_info::req_info() defaults it to "".
    span->SetAttribute(OTEL_GET_TRACE_ATTR(AttrHttpMethod), s->info.method);
  }
  span->SetAttribute(tracing::akamai::HOST, s->info.host);

  bool verbose = g_ceph_context->_conf->jaeger_tracing_verbose_attributes;
  span->SetAttribute(tracing::akamai::VERBOSE_ATTR, verbose);

  if (verbose) {
    // These attributes might help attackers, so they are disabled by default.
    span->SetAttribute(tracing::akamai::RELATIVE_URI, s->relative_uri);
    span->SetAttribute(tracing::akamai::REQUEST_URI, s->info.request_uri);
  }
#endif // HAVE_JAEGER
}
