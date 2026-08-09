#include <pulp/inspect/control_motion_executor.hpp>

#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/inspect/motion_scrubber.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace pulp::inspect {
namespace {

constexpr std::string_view kOperationId = "dev.pulp.trace/control@1";
constexpr std::size_t kMaximumTraceIdsPerSample = 32;

ControlExecutionOutcome fail(ControlResultCode code, std::string explanation,
                             ControlRetryClassification retry =
                                 ControlRetryClassification::Never) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .retry = retry,
                       .explanation = std::move(explanation)}};
}

ControlExecutionOutcome cancelled(std::string explanation) {
    return {.terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = explanation,
                       .cancellation_reason = std::move(explanation)}};
}

std::optional<ControlExecutionOutcome> checkpoint(const ControlExecutionContext& context) {
    if (!context.checkpoint)
        return fail(ControlResultCode::InvalidRequest,
                    "motion executor requires an authority checkpoint");
    switch (context.checkpoint()) {
    case ControlExecutionCheckpoint::Continue:
        return std::nullopt;
    case ControlExecutionCheckpoint::DeadlineExceeded:
        return fail(ControlResultCode::DeadlineExceeded,
                    "motion operation deadline elapsed before apply");
    case ControlExecutionCheckpoint::Cancelled:
    case ControlExecutionCheckpoint::AuthorityRevoked:
        return cancelled("motion operation authority unavailable before apply");
    }
    return cancelled("motion operation authority is unavailable");
}

std::optional<ControlExecutionOutcome>
checkpoint_before_apply(const ControlExecutionContext& context,
                        const ControlMotionTarget& target) {
    if (auto stopped = checkpoint(context))
        return stopped;
    if (!target.authority_live || !target.authority_live())
        return cancelled("motion host authority ended before apply");
    return std::nullopt;
}

ControlExecutionOutcome interrupted_replay() {
    return {.terminal_state = ControlReceiptState::UnknownNeedsRefresh,
            .result = {.result_code = ControlResultCode::UnknownNeedsRefresh,
                       .retry = ControlRetryClassification::AfterRefresh,
                       .explanation =
                           "motion replay authority ended after partial event emission"}};
}

bool exact_binding(const ControlMotionTarget& target, const ControlAdmissionPlan& plan,
                   const ControlRequestEnvelope& request) {
    return target.host_tier == ControlHostTier::Standalone &&
           target.registration_id == plan.registration_id &&
           target.session_id == plan.session_id && target.instance_id == plan.instance_id &&
           target.publication_id == plan.publication_id &&
           target.instance_generation == plan.instance_generation &&
           request.registration_id == plan.registration_id.value &&
           request.client_id == plan.client_id.value && request.grant_id == plan.grant_id.value &&
           request.instance_generation == plan.instance_generation &&
           request.operation_id == plan.operation_id &&
           request.operation_version == plan.operation_version && target.authority_live &&
           target.authority_live() && target.subscribe_authority_end &&
           request.deadline_unix_ms == plan.deadline_unix_ms;
}

view::motion::GeometryProperty geometry_property(std::string_view value) {
    using P = view::motion::GeometryProperty;
    if (value == "minY") return P::MinY;
    if (value == "maxX") return P::MaxX;
    if (value == "maxY") return P::MaxY;
    if (value == "midX") return P::MidX;
    if (value == "midY") return P::MidY;
    if (value == "width") return P::Width;
    if (value == "height") return P::Height;
    return P::MinX;
}

view::motion::ScrollProperty scroll_property(std::string_view value) {
    using P = view::motion::ScrollProperty;
    if (value == "contentOffsetY") return P::ContentOffsetY;
    if (value == "visibleRectMinX") return P::VisibleRectMinX;
    if (value == "visibleRectMinY") return P::VisibleRectMinY;
    if (value == "visibleRectWidth") return P::VisibleRectWidth;
    if (value == "visibleRectHeight") return P::VisibleRectHeight;
    if (value == "contentSizeWidth") return P::ContentSizeWidth;
    if (value == "contentSizeHeight") return P::ContentSizeHeight;
    if (value == "insetTop") return P::InsetTop;
    if (value == "insetBottom") return P::InsetBottom;
    if (value == "insetLeft") return P::InsetLeft;
    if (value == "insetRight") return P::InsetRight;
    if (value == "scrollableMaxX") return P::ScrollableMaxX;
    if (value == "scrollableMaxY") return P::ScrollableMaxY;
    return P::ContentOffsetX;
}

view::motion::GeometrySpace geometry_space(std::string_view value) {
    using S = view::motion::GeometrySpace;
    if (value == "view-local") return S::ViewLocal;
    if (value == "view-global") return S::ViewGlobal;
    if (value == "screen") return S::Screen;
    return S::Window;
}

MotionTraceSpec trace_spec(choc::value::ValueView params) {
    MotionTraceSpec spec;
    spec.view_name = params.hasObjectMember("view_name")
                         ? std::string(params["view_name"].getString())
                         : "Motion observation";
    spec.fps = params.hasObjectMember("fps") ? params["fps"].getWithDefault<int>(15) : 15;
    const auto metrics = params["metrics"];
    for (std::uint32_t index = 0; index < metrics.size(); ++index) {
        const auto metric = metrics[index];
        const std::string name = metric.hasObjectMember("name")
                                     ? std::string(metric["name"].getString())
                                     : std::string(metric["kind"].getString());
        const std::string node_id(metric["node_id"].getString());
        const std::string_view kind = metric["kind"].getString();
        if (kind == "geometry") {
            MotionGeometryMetric geometry{.name = name, .node_id = node_id};
            if (metric.hasObjectMember("properties")) {
                const auto properties = metric["properties"];
                for (std::uint32_t i = 0; i < properties.size(); ++i)
                    geometry.properties.push_back(geometry_property(properties[i].getString()));
            }
            if (metric.hasObjectMember("space"))
                geometry.space = geometry_space(metric["space"].getString());
            if (metric.hasObjectMember("source") &&
                metric["source"].getString() == std::string_view{"presentation"})
                geometry.source = view::motion::GeometrySource::Presentation;
            spec.geometry.push_back(std::move(geometry));
        } else {
            MotionScrollMetric scroll{.name = name, .node_id = node_id};
            if (metric.hasObjectMember("properties")) {
                const auto properties = metric["properties"];
                for (std::uint32_t i = 0; i < properties.size(); ++i)
                    scroll.properties.push_back(scroll_property(properties[i].getString()));
            }
            spec.scroll_geometry.push_back(std::move(scroll));
        }
    }
    return spec;
}

double bounded_number(double value, double maximum) {
    return std::isfinite(value) ? std::clamp(value, 0.0, maximum) : 0.0;
}

choc::value::Value base_result(const ControlAdmissionPlan& plan, std::string_view action) {
    auto result = choc::value::createObject("ControlMotionResult");
    result.setMember("action", std::string(action));
    result.setMember("receipt_id", plan.receipt_id.value);
    result.setMember("applied", true);
    return result;
}

std::string trace_owner(const ControlAdmissionPlan& plan) {
    return plan.client_id.value + "\n" + plan.registration_id.value + "\n" +
           plan.grant_id.value + "\n" + plan.session_id + "\n" + plan.publication_id;
}

ControlExecutionOutcome completed(choc::value::Value detail,
                                  const ControlOperationDescriptor& descriptor) {
    auto json = choc::json::toString(detail, true);
    ControlJsonSchemaDiagnostics diagnostics;
    if (!validate_control_output_json_schema(json, descriptor.output_schema_json, &diagnostics))
        return fail(ControlResultCode::InternalError,
                    diagnostics.explanation.empty()
                        ? "motion result violated its canonical schema"
                        : std::move(diagnostics.explanation));
    return {.terminal_state = ControlReceiptState::Completed,
            .result = {.detail_json = std::move(json)}};
}

ControlExecutionOutcome execute_motion(const ControlMotionTargetResolver& resolve_target,
                                       const ControlAdmissionPlan& plan,
                                       const ControlRequestEnvelope& request,
                                       const ControlExecutionContext& context) {
    if (request.operation_id != kOperationId || request.operation_version != 1)
        return fail(ControlResultCode::NotImplemented,
                    "host does not implement the requested control operation");
    const auto* descriptor = resolve_control_operation(request.operation_id,
                                                       request.operation_version);
    ControlJsonSchemaDiagnostics diagnostics;
    if (!descriptor || !validate_control_json_schema(request.params_json,
                                                      descriptor->input_schema_json,
                                                      &diagnostics))
        return fail(ControlResultCode::InvalidRequest,
                    diagnostics.explanation.empty() ? "invalid motion request"
                                                    : std::move(diagnostics.explanation));
    if (auto stopped = checkpoint(context)) return std::move(*stopped);
    if (!resolve_target)
        return fail(ControlResultCode::HostUnavailable, "motion target resolver is unavailable",
                    ControlRetryClassification::AfterRefresh);
    auto target = resolve_target(plan);
    if (!target || !exact_binding(*target, plan, request) || !target->inspector)
        return fail(ControlResultCode::SessionStale,
                    "exact motion target no longer matches the admitted T1 session",
                    ControlRetryClassification::AfterRefresh);
    if (auto stopped = checkpoint(context)) return std::move(*stopped);

    choc::value::Value params;
    try {
        params = choc::json::parse(request.params_json);
    } catch (...) {
        return fail(ControlResultCode::InvalidRequest, "motion request is not valid JSON");
    }
    const std::string_view action = params["action"].getString();
    auto detail = base_result(plan, action);

    if (action == "motion-start-trace") {
        if (auto stopped = checkpoint_before_apply(context, *target)) return std::move(*stopped);
        auto attached = target->inspector->attach_trace(trace_spec(params), trace_owner(plan),
                                                        target->authority_live,
                                                        target->subscribe_authority_end);
        if (!attached)
            return fail(attached.resource_exhausted ? ControlResultCode::ResourceExhausted
                                                    : ControlResultCode::HostUnavailable,
                        std::move(attached.error),
                        attached.resource_exhausted ? ControlRetryClassification::AfterBackoff
                                                    : ControlRetryClassification::AfterRefresh);
        detail.setMember("trace_id", attached.trace_id);
    } else if (action == "motion-stop-trace") {
        if (auto stopped = checkpoint_before_apply(context, *target)) return std::move(*stopped);
        if (!target->inspector->detach_trace(params["trace_id"].getInt64(), trace_owner(plan)))
            return fail(ControlResultCode::Inactive, "motion trace is not active",
                        ControlRetryClassification::AfterRefresh);
    } else if (action == "motion-scrub-to" || action == "motion-play" ||
               action == "motion-pause") {
        if (!target->scrubber || !target->scrubber->loaded())
            return fail(ControlResultCode::Inactive, "motion fixture is not loaded",
                        ControlRetryClassification::AfterRefresh);
        const auto maximum = params.hasObjectMember("maximum_events")
                                 ? static_cast<std::size_t>(params["maximum_events"].getInt64())
                                 : std::size_t{1024};
        bool truncated = false;
        bool interrupted = false;
        std::size_t emitted = 0;
        constexpr std::uint64_t kMaximumJsonSafeInteger = 9007199254740991ULL;
        if (action == "motion-play" && target->scrubber->max_frame() > kMaximumJsonSafeInteger)
            return fail(ControlResultCode::InvalidRequest,
                        "motion fixture playhead exceeds the canonical frame bound");
        if (auto stopped = checkpoint_before_apply(context, *target)) return std::move(*stopped);
        const auto continue_emission = [&] {
            return context.checkpoint &&
                   context.checkpoint() == ControlExecutionCheckpoint::Continue &&
                   target->authority_live && target->authority_live();
        };
        if (action == "motion-scrub-to")
            emitted = target->scrubber->scrub_to_bounded(
                static_cast<std::uint64_t>(params["frame"].getInt64()), maximum, truncated,
                continue_emission, &interrupted);
        else if (action == "motion-play")
            emitted = target->scrubber->play_bounded(maximum, truncated, continue_emission,
                                                     &interrupted);
        else
            target->scrubber->pause();
        if (interrupted)
            return interrupted_replay();
        detail.setMember("emitted_count", static_cast<std::int64_t>(emitted));
        detail.setMember("playhead_frame",
                         static_cast<std::int64_t>(target->scrubber->playhead_frame()));
        detail.setMember("playing", target->scrubber->playing());
        detail.setMember("truncated", truncated);
    } else if (action == "motion-enable-cost" || action == "motion-disable-cost") {
        if (auto stopped = checkpoint_before_apply(context, *target)) return std::move(*stopped);
        if (action == "motion-enable-cost") {
            if (!target->inspector->begin_cost_observation(trace_owner(plan),
                                                           target->authority_live))
                return fail(ControlResultCode::LeaseConflict,
                            "motion cost observation belongs to another live authority",
                            ControlRetryClassification::AfterRefresh);
            view::motion::CostAttributor::instance().set_enabled(true);
        } else {
            if (!target->inspector->end_cost_observation(trace_owner(plan)))
                return fail(ControlResultCode::Inactive,
                            "motion cost observation belongs to another authority",
                            ControlRetryClassification::AfterRefresh);
        }
    } else if (action == "motion-sample-cost") {
        if (auto stopped = checkpoint_before_apply(context, *target)) return std::move(*stopped);
        const auto maximum = static_cast<std::size_t>(params["maximum_samples"].getInt64());
        const auto samples = target->inspector->recent_cost_samples(maximum, trace_owner(plan));
        auto values = choc::value::createEmptyArray();
        for (const auto& sample : samples) {
            auto value = choc::value::createObject("ControlMotionCostSample");
            value.setMember("frame", static_cast<std::int64_t>(std::min<std::uint64_t>(
                                         sample.frame, 9007199254740991ULL)));
            value.setMember("t", bounded_number(sample.t_seconds, 1.0e12));
            value.setMember("render_pass_duration_ms",
                            bounded_number(sample.render_pass_duration_ms, 60000.0));
            value.setMember("dirty_rect_area_px",
                            bounded_number(sample.dirty_rect_area_px, 1.0e15));
            value.setMember("dirty_rect_count", std::clamp(sample.dirty_rect_count, 0, 100000));
            auto traces = choc::value::createEmptyArray();
            auto provenance = choc::value::createEmptyArray();
            const auto trace_count =
                std::min(sample.active_trace_ids.size(), kMaximumTraceIdsPerSample);
            for (std::size_t index = 0; index < trace_count; ++index) {
                traces.addArrayElement(sample.active_trace_ids[index]);
            }
            value.setMember("active_trace_ids", std::move(traces));
            value.setMember("active_provenance", std::move(provenance));
            values.addArrayElement(std::move(value));
        }
        detail.setMember("samples", std::move(values));
        detail.setMember("redacted", true);
    } else {
        return fail(ControlResultCode::NotImplemented,
                    "host does not implement this trace-control action");
    }
    return completed(std::move(detail), *descriptor);
}

} // namespace

ControlMotionExecutor::ControlMotionExecutor(ControlMainThreadExecutor main_thread)
    : main_thread_(std::move(main_thread)) {}

std::unique_ptr<ControlMotionExecutor>
ControlMotionExecutor::create(ControlMotionExecutorConfig config) {
    if (!config.main_thread_rpc || !config.resolve_target)
        return nullptr;
    auto resolver = std::make_shared<ControlMotionTargetResolver>(std::move(config.resolve_target));
    ControlMainThreadExecutor main_thread{
        std::move(config.main_thread_rpc),
        [resolver](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                   const ControlExecutionContext& context) {
            return execute_motion(*resolver, plan, request, context);
        }};
    return std::unique_ptr<ControlMotionExecutor>(
        new ControlMotionExecutor(std::move(main_thread)));
}

ControlOperationExecutor ControlMotionExecutor::executor() const {
    return main_thread_.executor();
}

} // namespace pulp::inspect
