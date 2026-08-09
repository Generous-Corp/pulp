#include <pulp/inspect/control_host_observability_bundle.hpp>

#include <pulp/inspect/control_manifest.hpp>
#include <pulp/runtime/crypto.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace pulp::inspect {
namespace {

constexpr std::string_view kTraceOperation = "dev.pulp.trace/session-control@1";
constexpr std::string_view kTelemetryOperation = "dev.pulp.telemetry/subscribe@1";

ControlExecutionOutcome
failure(ControlResultCode code, std::string explanation,
        ControlRetryClassification retry = ControlRetryClassification::Never) {
    return {.terminal_state = ControlReceiptState::Failed,
            .result = {.result_code = code, .retry = retry, .explanation = std::move(explanation)}};
}

ControlExecutionOutcome checkpoint_failure(ControlExecutionCheckpoint checkpoint) {
    if (checkpoint == ControlExecutionCheckpoint::DeadlineExceeded)
        return failure(ControlResultCode::DeadlineExceeded, "observability deadline exceeded");
    if (checkpoint == ControlExecutionCheckpoint::AuthorityRevoked)
        return failure(ControlResultCode::PolicyDenied, "observability authority was revoked");
    return {.terminal_state = ControlReceiptState::Cancelled,
            .result = {.result_code = ControlResultCode::Cancelled,
                       .explanation = "observability operation was cancelled",
                       .cancellation_reason = "control-checkpoint"}};
}

bool exact_authority(const ControlAuthorityBinding& plan, const ControlRequestEnvelope& request,
                     const ControlHostObservabilityBinding& binding) {
    return plan.registration_id == binding.registration_id &&
           plan.session_id == binding.session_id && plan.instance_id == binding.instance_id &&
           plan.publication_id == binding.publication_id && plan.client_id && plan.grant_id &&
           request.client_id == plan.client_id.value &&
           request.registration_id == plan.registration_id.value &&
           request.grant_id == plan.grant_id.value &&
           request.instance_generation == plan.instance_generation &&
           request.operation_id == plan.operation_id &&
           request.operation_version == plan.operation_version &&
           request.deadline_unix_ms == plan.deadline_unix_ms;
}

bool same_authenticated_binding(const ControlHostObservabilityBinding& supplied,
                                const ControlHostObservabilityBinding& expected) {
    return supplied.registration_id == expected.registration_id &&
           supplied.session_id == expected.session_id &&
           supplied.instance_id == expected.instance_id &&
           supplied.publication_id == expected.publication_id &&
           supplied.authentication_token.size() == expected.authentication_token.size() &&
           runtime::constant_time_equal(
               reinterpret_cast<const std::uint8_t*>(supplied.authentication_token.data()),
               reinterpret_cast<const std::uint8_t*>(expected.authentication_token.data()),
               expected.authentication_token.size());
}

std::string shape_id(view::ValueChannelShape shape) {
    switch (shape) {
    case view::ValueChannelShape::scalar:
        return "scalar";
    case view::ValueChannelShape::meter:
        return "meter";
    case view::ValueChannelShape::vector:
        return "vector";
    case view::ValueChannelShape::events:
        return "events";
    }
    return "unknown";
}

choc::value::Value wire_number(double value) {
    if (std::isnan(value))
        return choc::value::createString("NaN");
    if (std::isinf(value) && value > 0)
        return choc::value::createString("Infinity");
    if (std::isinf(value) && value < 0)
        return choc::value::createString("-Infinity");
    return choc::value::createFloat64(value);
}

} // namespace

struct ControlHostObservabilityBundle::State {
    mutable std::mutex mutex;
    mutable std::mutex execution_mutex;
    ControlHostObservabilityBinding binding;
    ControlOperationExecutor trace;
    std::shared_ptr<ControlTelemetryTap> telemetry;
    std::chrono::milliseconds ttl{};
    std::function<std::chrono::steady_clock::time_point()> clock;
    std::chrono::steady_clock::time_point expires_at{};
    bool connected = true;
    std::mutex expiry_wait_mutex;
    std::condition_variable expiry_changed;
    std::uint64_t expiry_generation = 0;
    std::jthread expiry_worker;

    ~State() {
        expiry_worker.request_stop();
        expiry_changed.notify_all();
        if (expiry_worker.joinable())
            expiry_worker.join();
    }

    std::chrono::steady_clock::time_point now() const {
        return clock ? clock() : std::chrono::steady_clock::now();
    }

    void deactivate_locked() {
        connected = false;
        trace = {};
    }

    void run_expiry(std::stop_token stop) {
        std::unique_lock wait_lock(expiry_wait_mutex);
        while (!stop.stop_requested()) {
            std::chrono::steady_clock::time_point deadline;
            std::uint64_t generation = 0;
            {
                std::lock_guard lock(mutex);
                if (!connected)
                    return;
                deadline = expires_at;
                generation = expiry_generation;
            }
            const auto remaining = deadline - now();
            const bool changed = remaining > std::chrono::steady_clock::duration::zero() &&
                                 expiry_changed.wait_for(wait_lock, remaining, [&] {
                                     if (stop.stop_requested())
                                         return true;
                                     std::lock_guard lock(mutex);
                                     return !connected || expiry_generation != generation;
                                 });
            if (changed)
                continue;
            std::lock_guard execution_lock(execution_mutex);
            std::lock_guard lock(mutex);
            if (connected && now() >= expires_at) {
                deactivate_locked();
                if (telemetry)
                    telemetry->detach();
                return;
            }
        }
    }

    ControlExecutionOutcome execute(const ControlAdmissionPlan& plan,
                                    const ControlRequestEnvelope& request,
                                    const ControlExecutionContext& context) {
        ControlOperationExecutor acquired_trace;
        std::shared_ptr<ControlTelemetryTap> acquired_telemetry;
        bool stale = false;
        {
            std::lock_guard lock(mutex);
            if (!connected || now() >= expires_at) {
                deactivate_locked();
                stale = true;
            } else {
                if (!exact_authority(plan, request, binding))
                    return failure(ControlResultCode::PolicyDenied,
                                   "observability authority does not match the bound host");
                acquired_trace = trace;
                acquired_telemetry = telemetry;
            }
        }
        if (stale) {
            std::lock_guard execution_lock(execution_mutex);
            if (telemetry)
                telemetry->detach();
            return failure(ControlResultCode::SessionStale, "observability host lease is inactive",
                           ControlRetryClassification::AfterRefresh);
        }

        if (context.checkpoint) {
            const auto checkpoint = context.checkpoint();
            if (checkpoint != ControlExecutionCheckpoint::Continue)
                return checkpoint_failure(checkpoint);
        }
        if (request.operation_id == kTraceOperation) {
            if (plan.capability != InspectorCapability::TraceSessionControl || !acquired_trace)
                return failure(ControlResultCode::NotImplemented,
                               "trace session control is not installed on this host");
            std::lock_guard execution_lock(execution_mutex);
            {
                std::lock_guard lock(mutex);
                if (!connected || now() >= expires_at) {
                    deactivate_locked();
                    if (telemetry)
                        telemetry->detach();
                    return failure(ControlResultCode::SessionStale,
                                   "observability host lease is inactive",
                                   ControlRetryClassification::AfterRefresh);
                }
            }
            if (context.checkpoint) {
                const auto checkpoint = context.checkpoint();
                if (checkpoint != ControlExecutionCheckpoint::Continue)
                    return checkpoint_failure(checkpoint);
            }
            return acquired_trace(plan, request, context);
        }
        if (request.operation_id != kTelemetryOperation || request.operation_version != 1 ||
            plan.capability != InspectorCapability::TelemetryStream || !acquired_telemetry)
            return failure(ControlResultCode::NotImplemented,
                           "host does not implement the requested observability operation");

        // ControlService may dispatch independent client sessions concurrently,
        // while the transport-free tap has one serialized control-thread contract.
        std::lock_guard execution_lock(execution_mutex);
        {
            std::lock_guard lock(mutex);
            if (!connected || now() >= expires_at) {
                deactivate_locked();
                acquired_telemetry->detach();
                return failure(ControlResultCode::SessionStale,
                               "observability host lease is inactive",
                               ControlRetryClassification::AfterRefresh);
            }
        }
        if (context.checkpoint) {
            const auto checkpoint = context.checkpoint();
            if (checkpoint != ControlExecutionCheckpoint::Continue)
                return checkpoint_failure(checkpoint);
        }

        const auto* descriptor = resolve_control_operation(request.operation_id, 1);
        ControlJsonSchemaDiagnostics diagnostics;
        if (!descriptor || !validate_control_json_schema(
                               request.params_json, descriptor->input_schema_json, &diagnostics))
            return failure(ControlResultCode::InvalidRequest, diagnostics.explanation.empty()
                                                                  ? "invalid telemetry request"
                                                                  : diagnostics.explanation);
        choc::value::Value params;
        try {
            params = choc::json::parse(request.params_json);
        } catch (...) {
            return failure(ControlResultCode::InvalidRequest,
                           "telemetry request is not valid JSON");
        }
        const ControlTelemetryAuthority authority{
            .client_id = plan.client_id.value,
            .registration_id = plan.registration_id.value,
            .instance_id = plan.instance_id,
            .grant_id = plan.grant_id.value,
            .allow_sensitive = false,
        };
        auto result = choc::value::createObject("ControlTelemetryResult");
        const auto action = params["action"].getString();
        if (action == "subscribe") {
            std::vector<std::string> channels;
            const auto supplied = params["channel_ids"];
            channels.reserve(supplied.size());
            for (std::uint32_t index = 0; index < supplied.size(); ++index)
                channels.emplace_back(supplied[index].getString());
            const double rate = params["max_hz"].get<double>();
            const auto buffer = static_cast<std::size_t>(params["buffer_samples"].get<int64_t>());
            auto id = acquired_telemetry->subscribe(authority, {.channels = std::move(channels),
                                                                .rate_hz = rate,
                                                                .maximum_vector_values = buffer});
            if (!id)
                return failure(ControlResultCode::ResourceExhausted,
                               "telemetry subscription was rejected by host bounds");
            result.addMember("action", "subscribed");
            result.addMember("stream_id", *id);
            result.addMember("accepted_hz", acquired_telemetry->effective_rate_hz(rate));
        } else if (action == "poll") {
            acquired_telemetry->poll();
            const auto stream = params["stream_id"].getString();
            auto frame = acquired_telemetry->try_pop(stream, authority);
            result.addMember("action", "polled");
            result.addMember("stream_id", stream);
            result.addMember("available", static_cast<bool>(frame));
            if (frame) {
                result.addMember("sequence", static_cast<int64_t>(frame->sequence));
                result.addMember("sampled_at_ns", static_cast<int64_t>(frame->sampled_at_ns));
                result.addMember("dropped", static_cast<int64_t>(frame->dropped_since_previous));
                auto samples = choc::value::createEmptyArray();
                for (const auto& sample : frame->channels) {
                    auto item = choc::value::createObject("");
                    item.addMember("channel", sample.channel);
                    item.addMember("shape", shape_id(sample.shape));
                    item.addMember("redacted", sample.redacted);
                    item.addMember("source_alive", sample.source_alive);
                    item.addMember("source_publication",
                                   static_cast<int64_t>(sample.source_publication));
                    item.addMember("source_dropped", static_cast<int64_t>(sample.source_dropped));
                    auto values = choc::value::createEmptyArray();
                    for (const auto value : sample.values)
                        values.addArrayElement(wire_number(value));
                    item.addMember("values", std::move(values));
                    samples.addArrayElement(std::move(item));
                }
                result.addMember("samples", std::move(samples));
            }
        } else {
            const auto stream = params["stream_id"].getString();
            if (!acquired_telemetry->unsubscribe(stream, authority))
                return failure(ControlResultCode::InvalidRequest,
                               "telemetry subscription was not found for this authority");
            result.addMember("action", "unsubscribed");
            result.addMember("stream_id", stream);
        }
        auto detail = choc::json::toString(result, false);
        if (!validate_control_output_json_schema(detail, descriptor->output_schema_json,
                                                 &diagnostics))
            return failure(ControlResultCode::InternalError, diagnostics.explanation.empty()
                                                                 ? "invalid telemetry result"
                                                                 : diagnostics.explanation);
        return {.terminal_state = ControlReceiptState::Completed,
                .result = {.detail_json = std::move(detail)}};
    }
};

ControlHostObservabilityBundle::ControlHostObservabilityBundle(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

ControlHostObservabilityBundle::~ControlHostObservabilityBundle() {
    disconnect();
}

std::unique_ptr<ControlHostObservabilityBundle>
ControlHostObservabilityBundle::create(ControlHostObservabilityBundleConfig config) {
    if (!config.binding.registration_id || config.binding.session_id.empty() ||
        config.binding.instance_id.empty() || config.binding.publication_id.empty() ||
        config.binding.authentication_token.size() < 32 || !config.trace_executor ||
        !config.telemetry || config.heartbeat_ttl.count() <= 0)
        return nullptr;
    auto state = std::make_shared<State>();
    state->binding = std::move(config.binding);
    state->trace = std::move(config.trace_executor);
    state->telemetry = std::move(config.telemetry);
    state->ttl = config.heartbeat_ttl;
    state->clock = std::move(config.clock);
    state->expires_at = state->now() + state->ttl;
    state->expiry_worker =
        std::jthread([raw = state.get()](std::stop_token stop) { raw->run_expiry(stop); });
    return std::unique_ptr<ControlHostObservabilityBundle>(
        new ControlHostObservabilityBundle(std::move(state)));
}

ControlOperationExecutor ControlHostObservabilityBundle::executor() const {
    const auto state = state_;
    return [state](const ControlAdmissionPlan& plan, const ControlRequestEnvelope& request,
                   const ControlExecutionContext& context) {
        return state->execute(plan, request, context);
    };
}

bool ControlHostObservabilityBundle::heartbeat(
    const ControlHostObservabilityBinding& authenticated_binding) {
    std::lock_guard execution_lock(state_->execution_mutex);
    std::lock_guard lock(state_->mutex);
    if (!state_->connected || state_->now() >= state_->expires_at) {
        state_->deactivate_locked();
        if (state_->telemetry)
            state_->telemetry->detach();
        return false;
    }
    if (!same_authenticated_binding(authenticated_binding, state_->binding))
        return false;
    state_->expires_at = state_->now() + state_->ttl;
    ++state_->expiry_generation;
    state_->expiry_changed.notify_all();
    return true;
}

bool ControlHostObservabilityBundle::ready() const {
    std::lock_guard execution_lock(state_->execution_mutex);
    std::lock_guard lock(state_->mutex);
    if (state_->connected && state_->now() >= state_->expires_at) {
        state_->deactivate_locked();
        if (state_->telemetry)
            state_->telemetry->detach();
    }
    return state_->connected && state_->trace && state_->telemetry;
}

void ControlHostObservabilityBundle::end_authority(
    std::string_view opaque_authority_id) noexcept {
    if (opaque_authority_id.empty())
        return;
    std::lock_guard execution_lock(state_->execution_mutex);
    std::lock_guard lock(state_->mutex);
    if (state_->telemetry) {
        (void)state_->telemetry->end_authority({.client_id = std::string(opaque_authority_id),
                                               .registration_id =
                                                   state_->binding.registration_id.value,
                                               .instance_id = state_->binding.instance_id,
                                               .grant_id = std::string(opaque_authority_id)});
    }
}

void ControlHostObservabilityBundle::disconnect() noexcept {
    if (!state_)
        return;
    std::lock_guard execution_lock(state_->execution_mutex);
    std::lock_guard lock(state_->mutex);
    state_->deactivate_locked();
    if (state_->telemetry)
        state_->telemetry->detach();
    state_->expiry_changed.notify_all();
}

} // namespace pulp::inspect
