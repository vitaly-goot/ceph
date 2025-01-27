// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tracer.h"
#include "common/ceph_context.h"
#include "global/global_context.h"

#ifdef HAVE_JAEGER

#include "common/debug.h"
#include "common/dout.h"
#include <memory>

#include "opentelemetry/context/propagation/global_propagator.h"
#include "opentelemetry/exporters/jaeger/jaeger_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/trace/batch_span_processor.h"
#include "opentelemetry/sdk/trace/samplers/always_off.h"
#include "opentelemetry/sdk/trace/samplers/parent.h"
#include "opentelemetry/sdk/trace/tracer_provider.h"
#include "opentelemetry/trace/propagation/http_trace_context.h"
#include "opentelemetry/trace/span_startoptions.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_trace

namespace tracing {

const opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> Tracer::noop_tracer = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer("no-op", OPENTELEMETRY_SDK_VERSION);
const jspan Tracer::noop_span = noop_tracer->StartSpan("noop");

using bufferlist = ceph::buffer::list;

namespace ilog = opentelemetry::sdk::common::internal_log;

/**
 * @brief OpenTelemetry custom log handler that shims into Ceph's log system.
 *
 * opentelemetry-cpp wants derivation of
 * opentelemetry::sdk::internal_log::LogHandler to handle log messages. This
 * class starts from the example code here:
 * https://opentelemetry-cpp.readthedocs.io/en/v1.4.1/sdk/GettingStarted.html#logging-and-error-handling
 * and adds a Handle() body that maps otel's levels onto Ceph debug levels.
 * Warning and Error are at level 0, Info is at level 1, and Debug is at level
 * 10.
 *
 * We need to shim otel's syslog-style logging levels into Ceph's dout().
 * There's no useful way to use higher-context calls such as ldpp_dout() here
 * because we have no way to pass that much context into the log handler.
 */
class OtelLogHandler : public ilog::LogHandler {
  void Handle(ilog::LogLevel level, const char* file, int line, const char* msg,
      const opentelemetry::sdk::common::AttributeMap& attributes) noexcept override

  {
    int dout_level;
    switch (level) {
    case ilog::LogLevel::Warning:
    case ilog::LogLevel::Error:
      dout_level = 0;
      break;
    case ilog::LogLevel::Info:
      dout_level = 1;
      break;
    default:
      dout_level = 10;
      break;
    }
    // Only do work if the message will be output at Ceph's current log level.
    if (!g_ceph_context->_conf->subsys.should_gather(ceph_subsys_trace,
            dout_level)) {
      return;
    }
    // For debug messages, show the level, file, line number and message.
    // Everything else, just the level and message.
    std::string log_msg;
    if (level == ilog::LogLevel::Debug) {
      log_msg = fmt::format(FMT_STRING("opentelemetry-cpp:{}:{}:{}: {}"),
          ilog::LevelToString(level), file, line, msg);
    } else {
      log_msg = fmt::format(FMT_STRING("opentelemetry-cpp:{}: {}"),
          ilog::LevelToString(level), msg);
    }
    // dout() needs a lexical int, not a variable containing an int. We need
    // one statement per supported (Ceph) level.
    switch (dout_level) {
    case 0:
      dout(0) << log_msg << dendl;
      break;
    case 1:
      dout(1) << log_msg << dendl;
      break;
    default:
      dout(10) << log_msg << dendl;
      break;
    }
  }
}; // class OtelLogHandler

Tracer::Tracer(opentelemetry::nostd::string_view service_name)
{
  init(service_name);
}

void Tracer::init(opentelemetry::nostd::string_view service_name) {
  if (!tracer) {
    if (g_ceph_context->_conf->otlp_tracing_enable) {
      return init_otlp(service_name);
    }
    using namespace opentelemetry;

    exporter::jaeger::JaegerExporterOptions exporter_options;
    if (g_ceph_context) {
      exporter_options.endpoint = g_ceph_context->_conf.get_val<std::string>("jaeger_agent_host");
      exporter_options.server_port = g_ceph_context->_conf.get_val<int64_t>("jaeger_agent_port");
    }
    const auto resource = sdk::resource::Resource::Create(std::move(
        sdk::resource::ResourceAttributes { { "service.name", service_name } }));
    auto exporter = std::unique_ptr<sdk::trace::SpanExporter>(
        new exporter::jaeger::JaegerExporter(exporter_options));

    const opentelemetry::sdk::trace::BatchSpanProcessorOptions processor_options;
    auto processor = std::unique_ptr<sdk::trace::SpanProcessor>(
        new sdk::trace::BatchSpanProcessor(std::move(exporter),
            processor_options));
    auto provider = nostd::shared_ptr<trace::TracerProvider>(
        new sdk::trace::TracerProvider(std::move(processor),
            resource));

    trace::Provider::SetTracerProvider(provider);
    tracer = provider->GetTracer(service_name, OPENTELEMETRY_SDK_VERSION);
  }
}

void Tracer::init_otlp(opentelemetry::nostd::string_view service_name)
{
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::TracerProvider> provider;
  const opentelemetry::sdk::trace::BatchSpanProcessorOptions processor_options;

  using namespace opentelemetry;
  // Log via Ceph's logging system. The default just writes to stdout.
  // This needs to be done before creating the provider, according to the
  // source for SetLogHandler().
  ilog::GlobalLogHandler::SetLogHandler(
      nostd::shared_ptr<ilog::LogHandler>(new OtelLogHandler()));
  ilog::LogLevel log_level = (g_ceph_context->_conf->otlp_tracing_log_level_debug)
      ? ilog::LogLevel::Debug
      : ilog::LogLevel::Info;
  ilog::GlobalLogHandler::SetLogLevel(log_level); // XXX customize!

  opentelemetry::exporter::otlp::OtlpGrpcExporterOptions
      exporter_options;
  auto endpoint = g_ceph_context->_conf->otlp_endpoint_url;
  exporter_options.endpoint = endpoint;
  if (endpoint.starts_with("https")) {
    exporter_options.use_ssl_credentials = true;
    std::string ca_cert_file = g_ceph_context->_conf->otlp_endpoint_ca_cert_file;
    if (!ca_cert_file.empty()) {
      exporter_options.ssl_credentials_cacert_path = ca_cert_file;
    }
  }

  namespace sdktrace = ::opentelemetry::sdk::trace;

  std::unique_ptr<sdktrace::Sampler> sampler;
  if (!g_ceph_context->_conf->otlp_sampler_parent_based) {
    // The ParentBasedSampler delegate is not used.
    sampler = std::unique_ptr<sdktrace::Sampler>(new sdktrace::AlwaysOnSampler());
  } else {
    // The ParentBasedSampler delegate (fallback) is configured using option
    // 'otlp_sampler_delegate_defaults_to_on'.
    std::unique_ptr<sdktrace::Sampler> delegate_sampler;
    if (g_ceph_context->_conf->otlp_sampler_delegate_defaults_to_on) {
      delegate_sampler = std::unique_ptr<sdktrace::Sampler>(
          new sdktrace::AlwaysOnSampler());
    } else {
      delegate_sampler = std::unique_ptr<sdktrace::Sampler>(
          new sdktrace::AlwaysOffSampler());
    }
    sampler = std::unique_ptr<sdktrace::Sampler>(
        new sdktrace::ParentBasedSampler(std::move(delegate_sampler)));
  }

  auto exporter = std::unique_ptr<sdk::trace::SpanExporter>(
      new exporter::otlp::OtlpGrpcExporter(exporter_options));
  auto processor = std::unique_ptr<sdktrace::SpanProcessor>(
      new sdktrace::BatchSpanProcessor(std::move(exporter),
          std::move(processor_options)));
  const auto resource = sdk::resource::Resource::Create(std::move(
      sdk::resource::ResourceAttributes { { "service.name", service_name } }));
  provider = nostd::shared_ptr<trace::TracerProvider>(new sdktrace::TracerProvider(
      std::move(processor), std::move(resource),
      std::move(sampler)));

  trace::Provider::SetTracerProvider(provider);
  tracer = provider->GetTracer(service_name, OPENTELEMETRY_SDK_VERSION);

  // Set global propagator
  context::propagation::GlobalTextMapPropagator::SetGlobalPropagator(
      nostd::shared_ptr<context::propagation::TextMapPropagator>(
          new trace::propagation::HttpTraceContext()));
}

jspan Tracer::start_trace(opentelemetry::nostd::string_view trace_name) {
  if (is_enabled()) {
    return tracer->StartSpan(trace_name);
  }
  return noop_span;
}

jspan Tracer::start_trace(opentelemetry::nostd::string_view trace_name, bool trace_is_enabled) {
  if (trace_is_enabled) {
    return tracer->StartSpan(trace_name);
  }
  return noop_tracer->StartSpan(trace_name);
}

jspan Tracer::start_trace_with_req_state_parent(opentelemetry::nostd::string_view trace_name,
    bool trace_is_enabled,
    const std::string& traceparent_header, const std::string& tracestate_header)
{
  if (!trace_is_enabled) {
    return noop_tracer->StartSpan(trace_name);
  }

  using namespace opentelemetry;

  // Get global propagator
  HttpTextMapCarrier<opentelemetry::ext::http::client::Headers> carrier;
  carrier.Set("traceparent", traceparent_header);
  carrier.Set("tracestate", tracestate_header);
  auto propagator = opentelemetry::context::propagation::GlobalTextMapPropagator::GetGlobalPropagator();

  // Extract headers to context
  auto current_ctx = opentelemetry::context::RuntimeContext::GetCurrent();
  auto new_context = propagator->Extract(carrier, current_ctx);
  auto remote_span = opentelemetry::trace::GetSpan(new_context);

  trace::StartSpanOptions span_opts;
  span_opts.parent = remote_span->GetContext();

  return tracer->StartSpan(trace_name, {}, span_opts);
}

jspan Tracer::add_span(opentelemetry::nostd::string_view span_name, const jspan& parent_span) {
  if (is_enabled() && parent_span->IsRecording()) {
    opentelemetry::trace::StartSpanOptions span_opts;
    span_opts.parent = parent_span->GetContext();
    return tracer->StartSpan(span_name, span_opts);
  }
  return noop_span;
}

jspan Tracer::add_span(opentelemetry::nostd::string_view span_name, const jspan_context& parent_ctx) {
  if (is_enabled() && parent_ctx.IsValid()) {
    opentelemetry::trace::StartSpanOptions span_opts;
    span_opts.parent = parent_ctx;
    return tracer->StartSpan(span_name, span_opts);
  }
  return noop_span;
}

bool Tracer::is_enabled() const {
  return g_ceph_context->_conf->jaeger_tracing_enable || g_ceph_context->_conf->otlp_tracing_enable;
}

void encode(const jspan_context& span_ctx, bufferlist& bl, uint64_t f) {
  ENCODE_START(1, 1, bl);
  using namespace opentelemetry;
  using namespace trace;
  auto is_valid = span_ctx.IsValid();
  encode(is_valid, bl);
  if (is_valid) {
    encode_nohead(std::string_view(reinterpret_cast<const char*>(span_ctx.trace_id().Id().data()), TraceId::kSize), bl);
    encode_nohead(std::string_view(reinterpret_cast<const char*>(span_ctx.span_id().Id().data()), SpanId::kSize), bl);
    encode(span_ctx.trace_flags().flags(), bl);
  }
  ENCODE_FINISH(bl);
}

void decode(jspan_context& span_ctx, bufferlist::const_iterator& bl) {
  using namespace opentelemetry;
  using namespace trace;
  DECODE_START(1, bl);
  bool is_valid;
  decode(is_valid, bl);
  if (is_valid) {
    std::array<uint8_t, TraceId::kSize> trace_id;
    std::array<uint8_t, SpanId::kSize> span_id;
    uint8_t flags;
    decode(trace_id, bl);
    decode(span_id, bl);
    decode(flags, bl);
    span_ctx = SpanContext(
      TraceId(nostd::span<uint8_t, TraceId::kSize>(trace_id)),
      SpanId(nostd::span<uint8_t, SpanId::kSize>(span_id)),
      TraceFlags(flags),
      true);
  }
  DECODE_FINISH(bl);
}
} // namespace tracing

#endif // HAVE_JAEGER
