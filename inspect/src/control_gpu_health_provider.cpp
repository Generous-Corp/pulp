#include <pulp/inspect/control_gpu_health_provider.hpp>

#include <pulp/runtime/trace.hpp>
#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::inspect {
namespace gh = tooling::gpu_health;
namespace {

constexpr std::uint64_t kMaximumExactJsonInteger = 9'007'199'254'740'991ULL;

bool bounded_string(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum;
}

gh::AdapterClass classify(const ControlGpuHealthProvider::AdapterIdentity& adapter) {
    if (!adapter.available || !adapter.native_bridge)
        return gh::AdapterClass::unknown;
    auto text = adapter.backend + " " + adapter.name + " " + adapter.vendor;
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text.find("null") != std::string::npos)
        return gh::AdapterClass::null_adapter;
    if (text.find("software") != std::string::npos ||
        text.find("swiftshader") != std::string::npos || text.find("llvmpipe") != std::string::npos)
        return gh::AdapterClass::software;
    return gh::AdapterClass::hardware;
}

gh::AdapterIdentity identity(const ControlGpuHealthProvider::AdapterIdentity& value) {
    gh::AdapterIdentity result;
    result.classification = classify(value);
    if (!value.available) {
        result.status = gh::IdentityStatus::unavailable;
        return result;
    }
    result.status = value.native_bridge && bounded_string(value.backend, 256)
                        ? gh::IdentityStatus::authentic
                        : gh::IdentityStatus::unverified;
    if (result.status != gh::IdentityStatus::authentic)
        result.classification = gh::AdapterClass::unknown;
    if (bounded_string(value.backend, 256))
        result.backend = value.backend;
    if (bounded_string(value.name, 256))
        result.name = value.name;
    if (bounded_string(value.vendor, 256))
        result.vendor = value.vendor;
    if (bounded_string(value.architecture, 256))
        result.architecture = value.architecture;
    return result;
}

gh::CacheProvenance provenance(ControlGpuHealthProvider::CacheProvenance value) {
    using Source = ControlGpuHealthProvider::CacheProvenance;
    switch (value) {
        case Source::fresh_process: return gh::CacheProvenance::fresh_process;
        case Source::explicit_cache_reset: return gh::CacheProvenance::explicit_cache_reset;
        case Source::same_process_editor_reopen:
            return gh::CacheProvenance::same_process_editor_reopen;
        case Source::unknown: return gh::CacheProvenance::unknown;
    }
    return gh::CacheProvenance::unknown;
}

bool sha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](unsigned char character) {
               return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
           });
}

bool evidence_id(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](unsigned char character) {
               return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
           });
}

bool valid_reference_host(const ControlGpuHealthProvider::ReferenceHost& host) {
    return bounded_string(host.host_id, 128) && std::isfinite(host.refresh_rate_hz) &&
           host.refresh_rate_hz >= 1.0 && host.refresh_rate_hz <= 1000.0;
}

std::optional<double> bounded_measurement(std::optional<double> value) {
    if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 300'000.0)
        return std::nullopt;
    return value;
}

void add_missing_category(gh::StartupCapture& capture, std::string_view category) {
    if (!bounded_string(category, 64) ||
        std::ranges::find(capture.missing_trace_categories, category) !=
            capture.missing_trace_categories.end() ||
        capture.missing_trace_categories.size() >= gh::kMaximumMissingTraceCategories)
        return;
    capture.missing_trace_categories.emplace_back(category);
}

gh::ProbeEvidence initial_probe() {
    gh::ProbeEvidence probe;
    probe.probe_id = "first-visible-frame";
    probe.events.push_back({0, gh::Stage::adapter, gh::Verdict::unverified,
                            "gpu.adapter.unverified",
                            "editor-open measurement is armed but no frame has been observed"});
    return probe;
}

void record_capture_event(gh::StartupCapture& capture) {
    if (capture.event_count < capture.event_capacity) {
        ++capture.event_count;
    } else {
        ++capture.dropped_event_count;
        capture.truncated = true;
    }
}

std::optional<double> trial_percentile(const std::vector<gh::StartupTrial>& trials,
                                       double percentile,
                                       std::optional<double> gh::StartupTrial::* measurement) {
    std::vector<double> values;
    values.reserve(trials.size());
    for (const auto& trial : trials)
        if (trial.*measurement)
            values.push_back(*(trial.*measurement));
    if (values.empty())
        return std::nullopt;
    std::ranges::sort(values);
    const auto rank = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::ceil(percentile * values.size() / 100.0)), 1, values.size());
    return values[rank - 1];
}

void derive_startup_state(gh::StartupMeasurements& startup) {
    startup.observed_percentile_ms =
        trial_percentile(startup.trials, startup.budget.percentile,
                         &gh::StartupTrial::editor_open_to_first_nonblank_ms);
    startup.interaction_hitch_percentile_ms = trial_percentile(
        startup.trials, startup.budget.percentile, &gh::StartupTrial::interaction_hitch_ms);

    const bool exact_trial_count = startup.trials.size() == startup.budget.trial_count;
    const auto cold_count = std::ranges::count_if(startup.trials, [](const auto& value) {
        return value.cache_state == gh::CacheState::cold;
    });
    const auto warm_count = startup.trials.size() - cold_count;
    const bool exact_composition = exact_trial_count &&
                                   cold_count == startup.budget.cold_trial_count &&
                                   warm_count == startup.budget.warm_trial_count;
    const bool unresolved = std::ranges::any_of(startup.trials, [](const auto& value) {
        return value.verdict == gh::Verdict::unavailable ||
               value.verdict == gh::Verdict::unverified;
    });
    const bool capture_integrity =
        !startup.capture.truncated && startup.capture.dropped_event_count == 0;
    if (exact_composition && capture_integrity && !unresolved) {
        startup.status = gh::MeasurementStatus::complete;
    } else if (exact_composition && capture_integrity) {
        startup.status = gh::MeasurementStatus::unverified;
    } else {
        startup.status = gh::MeasurementStatus::incomplete;
    }

    startup.verdict = gh::Verdict::unverified;
    if (startup.status == gh::MeasurementStatus::complete &&
        startup.budget.status == gh::BudgetStatus::ratified && startup.observed_percentile_ms &&
        startup.interaction_hitch_percentile_ms) {
        const bool blank_failure = std::ranges::any_of(startup.trials, [](const auto& value) {
            return value.diagnostic_code == "gpu.startup.blank";
        });
        startup.verdict =
            blank_failure || *startup.observed_percentile_ms > *startup.budget.threshold_ms
                ? gh::Verdict::fail
                : gh::Verdict::pass;
    }
}

} // namespace

struct ControlGpuHealthProvider::Impl {
    explicit Impl(Config value) : config(std::move(value)), writer(std::this_thread::get_id()) {
        auto result = std::make_shared<gh::HealthReadResult>();
        result->health.run_id = "editor-open-pending";
        result->health.probes.push_back(initial_probe());
        result->startup.status = gh::MeasurementStatus::incomplete;
        result->startup.measurement_endpoint =
            config.measurement_endpoint == MeasurementEndpoint::headless_capture_complete
                ? gh::MeasurementEndpoint::headless_capture_complete
                : gh::MeasurementEndpoint::native_compositor_presentation;
        result->startup.budget.budget_id = bounded_string(config.budget_id, 128)
                                               ? config.budget_id
                                               : "pulp.editor-first-visible.v1";
        result->startup.budget.version = 1;
        std::set<std::string> reference_host_ids;
        const bool ratified =
            config.budget_ratified && config.budget_version == 1 && config.threshold_ms &&
            std::isfinite(*config.threshold_ms) && *config.threshold_ms > 0.0 &&
            *config.threshold_ms <= 300'000.0 && config.threshold_source &&
            bounded_string(*config.threshold_source, 512) && !config.reference_hosts.empty() &&
            config.reference_hosts.size() <= gh::kMaximumReferenceHosts &&
            std::ranges::all_of(config.reference_hosts, [&](const auto& host) {
                return valid_reference_host(host) && reference_host_ids.insert(host.host_id).second;
            });
        if (ratified) {
            result->startup.budget.status = gh::BudgetStatus::ratified;
            result->startup.budget.threshold_ms = config.threshold_ms;
            result->startup.budget.threshold_source = config.threshold_source;
            for (const auto& host : config.reference_hosts)
                result->startup.budget.reference_hosts.push_back(
                    {.host_id = host.host_id, .refresh_rate_hz = host.refresh_rate_hz});
        }
        result->startup.capture.event_capacity =
            std::clamp<std::uint32_t>(config.event_capacity, 1, 4096);
        result->startup.capture.missing_trace_categories = {"frame_lifecycle", "a2t_correlation"};
        result->startup.identity.pulp_build_id =
            bounded_string(config.pulp_build_id, 128) ? config.pulp_build_id : "unknown-build";
        if (config.vellum_revision && bounded_string(*config.vellum_revision, 128))
            result->startup.identity.vellum_revision = config.vellum_revision;
        if (config.source_signature_sha256 && sha256(*config.source_signature_sha256))
            result->startup.identity.source_signature_sha256 = config.source_signature_sha256;
        if (config.shader_signature_sha256 && sha256(*config.shader_signature_sha256))
            result->startup.identity.shader_signature_sha256 = config.shader_signature_sha256;
        if (config.expected_target_signature_sha256 &&
            sha256(*config.expected_target_signature_sha256))
            result->startup.identity.expected_target_signature_sha256 =
                config.expected_target_signature_sha256;
        publish(std::move(result));
    }

    bool on_writer() const noexcept {
        return std::this_thread::get_id() == writer;
    }
    void publish(std::shared_ptr<gh::HealthReadResult> next) noexcept {
        std::atomic_store_explicit(&current,
                                   std::shared_ptr<const gh::HealthReadResult>(std::move(next)),
                                   std::memory_order_release);
    }
    std::shared_ptr<gh::HealthReadResult> edit() const {
        return std::make_shared<gh::HealthReadResult>(
            *std::atomic_load_explicit(&current, std::memory_order_acquire));
    }
    bool has_trace_identity() const noexcept {
        return config.gpu_evidence_id && evidence_id(*config.gpu_evidence_id) &&
               config.trace_evidence_id && bounded_string(*config.trace_evidence_id, 128) &&
               *config.gpu_evidence_id != *config.trace_evidence_id;
    }
    void begin_trace(std::uint32_t frame_index, CacheState observed_cache_state) noexcept {
        if (!has_trace_identity())
            return;
        PULP_TRACE_BEGIN_ARGS(
            "render", "frame_first_visible_health",
            "gpu_evidence_id", config.gpu_evidence_id->c_str(),
            "trace_evidence_id", config.trace_evidence_id->c_str(),
            "frame_index", frame_index,
            "cache_state", observed_cache_state == CacheState::cold ? "cold" : "warm");
        trace_span_open = true;
    }
    void trace_transition(std::uint32_t sequence, std::string_view diagnostic_code,
                          gh::HealthState health_state) noexcept {
        if (!has_trace_identity())
            return;
        PULP_TRACE_SCOPE_NAMED_ARGS(
            "gpu", "gpu_health_transition_first_visible",
            "gpu_evidence_id", config.gpu_evidence_id->c_str(),
            "trace_evidence_id", config.trace_evidence_id->c_str(),
            "sequence", sequence,
            "frame_index", sequence,
            "diagnostic_code", diagnostic_code.data(),
            "health_state", gh::to_string(health_state).data());
    }
    void end_trace() noexcept {
        if (!trace_span_open)
            return;
        PULP_TRACE_END("render");
        trace_span_open = false;
    }

    Config config;
    std::thread::id writer;
    std::shared_ptr<const gh::HealthReadResult> current;
    std::optional<std::chrono::steady_clock::time_point> requested_at;
    CacheState cache_state = CacheState::cold;
    bool all_trace_observations_complete = true;
    std::set<std::string> lifecycle_ids;
    bool trace_span_open = false;
};

ControlGpuHealthProvider::ControlGpuHealthProvider(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
ControlGpuHealthProvider::~ControlGpuHealthProvider() {
    if (impl_->on_writer())
        impl_->end_trace();
}

bool ControlGpuHealthProvider::begin_editor_open(
    CacheState cache_state, std::chrono::steady_clock::time_point requested_at) noexcept {
    const auto snapshot = std::atomic_load_explicit(&impl_->current, std::memory_order_acquire);
    if (!impl_->on_writer() || impl_->requested_at ||
        snapshot->startup.trials.size() >= snapshot->startup.budget.trial_count)
        return false;
    impl_->cache_state = cache_state;
    impl_->requested_at = requested_at;
    impl_->begin_trace(static_cast<std::uint32_t>(snapshot->startup.trials.size()), cache_state);
    return true;
}

bool ControlGpuHealthProvider::record_presented_frame(const FrameObservation& frame) noexcept {
    if (!impl_->on_writer() || !impl_->requested_at)
        return false;
    auto result = impl_->edit();
    const bool headless_endpoint =
        impl_->config.measurement_endpoint == MeasurementEndpoint::headless_capture_complete;
    const auto endpoint_at =
        !headless_endpoint && frame.native_present_observed && frame.native_presented_at
            ? *frame.native_presented_at
            : frame.observed_at;
    const auto endpoint_ms =
        std::chrono::duration<long double, std::milli>(endpoint_at.time_since_epoch()).count();
    const auto requested_ms =
        std::chrono::duration<long double, std::milli>(impl_->requested_at->time_since_epoch())
            .count();
    const auto elapsed_wide = endpoint_ms - requested_ms;
    const auto elapsed = static_cast<double>(elapsed_wide);
    const bool capture_unavailable = !frame.capture_valid;
    const bool blank = !capture_unavailable &&
                       (impl_->config.seed_blank_first_frame || !frame.content_floor_passed);
    const auto adapter = identity(frame.adapter);
    auto probe = gh::ProbeEvidence{};
    probe.probe_id = "first-visible-frame";
    probe.adapter = adapter;
    if (frame.gpu_submission_observed)
        probe.measurements.command_submitted = true;
    probe.measurements.readback_completed = frame.capture_valid;
    probe.measurements.pixel_output_produced = frame.capture_valid;
    if (frame.capture_valid) {
        probe.measurements.content_floor_passed = !blank;
        constexpr auto kMaximumJsonInteger =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (frame.non_transparent_pixel_count <= kMaximumJsonInteger)
            probe.measurements.non_transparent_pixel_count = frame.non_transparent_pixel_count;
        if (frame.distinct_color_count <= kMaximumJsonInteger)
            probe.measurements.distinct_color_count = frame.distinct_color_count;
    }
    std::uint32_t sequence = 0;
    if (adapter.status == gh::IdentityStatus::authentic) {
        probe.events.push_back({sequence++, gh::Stage::adapter, gh::Verdict::pass,
                                "gpu.adapter.pass", "native GPU adapter identity observed"});
    } else {
        probe.events.push_back({sequence++, gh::Stage::adapter, gh::Verdict::unverified,
                                "gpu.adapter.unverified",
                                "capture lacks an authentic native GPU adapter identity"});
    }
    const auto submission_verdict =
        frame.gpu_submission_observed
            ? gh::Verdict::pass
            : (!capture_unavailable && adapter.status == gh::IdentityStatus::authentic
                   ? gh::Verdict::unverified
                   : gh::Verdict::unavailable);
    probe.events.push_back(
        {sequence++, gh::Stage::submit, submission_verdict,
         submission_verdict == gh::Verdict::pass
             ? "gpu.submit.pass"
             : (submission_verdict == gh::Verdict::unverified ? "gpu.submit.unverified"
                                                              : "gpu.submit.unavailable"),
         frame.gpu_submission_observed ? "GPU submission was observed by the host producer"
                                       : "GPU submission was not proven by capture"});
    probe.events.push_back({sequence++, gh::Stage::readback,
                            frame.capture_valid ? gh::Verdict::pass : gh::Verdict::unavailable,
                            frame.capture_valid ? "gpu.readback.pass" : "gpu.readback.unavailable",
                            frame.capture_valid ? "host back buffer readback completed"
                                                : "host back buffer readback was unavailable"});
    probe.events.push_back({sequence++, gh::Stage::content,
                            capture_unavailable ? gh::Verdict::unavailable
                                                : (blank ? gh::Verdict::fail : gh::Verdict::pass),
                            capture_unavailable ? "gpu.content.unavailable"
                                                : (blank ? "gpu.content.fail" : "gpu.content.pass"),
                            capture_unavailable
                                ? "frame content was unavailable because capture failed"
                                : (blank ? "blank-first-frame negative control triggered"
                                         : "captured frame passed the content floor")});
    probe.verdict =
        blank ? gh::Verdict::fail
        : capture_unavailable
            ? gh::Verdict::unavailable
            : (adapter.status == gh::IdentityStatus::authentic
                   ? (frame.gpu_submission_observed ? gh::Verdict::pass : gh::Verdict::unverified)
                   : (frame.gpu_submission_observed ? gh::Verdict::unverified
                                                    : gh::Verdict::unavailable));
    result->health.run_id =
        bounded_string(impl_->config.campaign_id, 128) ? impl_->config.campaign_id : "editor-open";
    result->health.probes = {std::move(probe)};
    result->health.verdict = result->health.probes.front().verdict;
    result->health.health_state =
        result->health.verdict == gh::Verdict::fail
            ? gh::HealthState::failed
            : (result->health.verdict == gh::Verdict::unavailable
                   ? gh::HealthState::unavailable
                   : (result->health.verdict == gh::Verdict::pass ? gh::HealthState::healthy
                                                                  : gh::HealthState::unverified));
    result->startup.identity.adapter_class = classify(frame.adapter);
    record_capture_event(result->startup.capture);
    gh::StartupTrial trial;
    trial.sequence = static_cast<std::uint32_t>(result->startup.trials.size());
    trial.cache_state =
        impl_->cache_state == CacheState::cold ? gh::CacheState::cold : gh::CacheState::warm;
    if (bounded_string(frame.lifecycle_id, 128))
        trial.lifecycle_id = frame.lifecycle_id;
    trial.cache_provenance = provenance(frame.cache_provenance);
    if (!capture_unavailable && !blank && std::isfinite(elapsed_wide) && elapsed_wide >= 0.0L &&
        elapsed_wide <= 300'000.0L)
        trial.editor_open_to_first_nonblank_ms = elapsed;
    trial.interaction_hitch_ms = bounded_measurement(frame.interaction_hitch_ms);
    trial.shader_compile_ms = bounded_measurement(frame.shader_compile_ms);
    trial.upload_ms = bounded_measurement(frame.upload_ms);
    trial.hidden_frame_ms = bounded_measurement(frame.hidden_frame_ms);
    trial.present_ms = bounded_measurement(frame.present_ms);
    if (sha256(frame.observed_signature_sha256))
        trial.observed_target_signature_sha256 = frame.observed_signature_sha256;
    if (!capture_unavailable)
        trial.content_floor_passed = !blank;
    trial.visible_state = capture_unavailable
                              ? gh::VisibleState::unknown
                              : (blank ? gh::VisibleState::fallback : gh::VisibleState::prepared);
    const bool configured_trace_identity = impl_->has_trace_identity();
    const bool valid_frame_trace_identity =
        frame.trace_evidence_id && bounded_string(*frame.trace_evidence_id, 128);
    const bool trace_identity_consistent =
        configured_trace_identity && valid_frame_trace_identity &&
        frame.trace_evidence_id == impl_->config.trace_evidence_id;
    if (configured_trace_identity)
        result->startup.correlation.trace_evidence_id = impl_->config.trace_evidence_id;
    if (!trace_identity_consistent)
        add_missing_category(result->startup.capture, "a2t_correlation");
    const bool trace_complete = trace_identity_consistent && frame.missing_trace_categories.empty();
    if (!trace_complete) {
        impl_->all_trace_observations_complete = false;
        if (frame.missing_trace_categories.empty()) {
            add_missing_category(result->startup.capture, "frame_lifecycle");
            add_missing_category(result->startup.capture, "a2t_correlation");
        } else {
            for (const auto& category : frame.missing_trace_categories)
                add_missing_category(result->startup.capture, category);
        }
    } else if (impl_->all_trace_observations_complete) {
        result->startup.capture.missing_trace_categories.clear();
    }
    const bool target_identity_complete =
        result->startup.identity.expected_target_signature_sha256 &&
        trial.observed_target_signature_sha256 ==
            result->startup.identity.expected_target_signature_sha256;
    const bool lifecycle_complete = bounded_string(frame.lifecycle_id, 128) &&
                                    frame.observed_cache_state == impl_->cache_state &&
                                    impl_->lifecycle_ids.insert(frame.lifecycle_id).second;
    const bool endpoint_complete =
        headless_endpoint ? frame.capture_valid
                          : frame.native_present_observed && frame.native_presented_at.has_value();
    const bool startup_trial_complete =
        result->startup.budget.status == gh::BudgetStatus::ratified &&
        impl_->config.gpu_evidence_id && evidence_id(*impl_->config.gpu_evidence_id) &&
        frame.gpu_submission_observed && endpoint_complete && lifecycle_complete &&
        adapter.status == gh::IdentityStatus::authentic &&
        adapter.classification == gh::AdapterClass::hardware &&
        trial.editor_open_to_first_nonblank_ms && trial.interaction_hitch_ms &&
        trace_identity_consistent && target_identity_complete;
    if (capture_unavailable) {
        trial.verdict = gh::Verdict::unavailable;
        trial.diagnostic_code = "gpu.startup.unavailable";
    } else if (blank) {
        trial.verdict = gh::Verdict::fail;
        trial.diagnostic_code = "gpu.startup.blank";
    } else if (startup_trial_complete &&
               *trial.editor_open_to_first_nonblank_ms > *result->startup.budget.threshold_ms) {
        trial.verdict = gh::Verdict::fail;
        trial.diagnostic_code = "gpu.startup.budget_exceeded";
    } else if (startup_trial_complete) {
        trial.verdict = gh::Verdict::pass;
        trial.diagnostic_code = "gpu.startup.pass";
    } else {
        trial.verdict = gh::Verdict::unverified;
        trial.diagnostic_code =
            trace_complete ? "gpu.startup.unverified" : "gpu.startup.trace_incomplete";
    }
    result->startup.trials.push_back(std::move(trial));
    if (impl_->config.gpu_evidence_id && evidence_id(*impl_->config.gpu_evidence_id))
        result->startup.correlation.gpu_evidence_id = impl_->config.gpu_evidence_id;
    derive_startup_state(result->startup);
    const auto& recorded = result->startup.trials.back();
    impl_->trace_transition(recorded.sequence, recorded.diagnostic_code,
                            result->health.health_state);
    impl_->end_trace();
    impl_->requested_at.reset();
    impl_->publish(std::move(result));
    return true;
}

bool ControlGpuHealthProvider::record_timeout(
    std::chrono::steady_clock::time_point observed_at) noexcept {
    if (!impl_->on_writer() || !impl_->requested_at ||
        observed_at - *impl_->requested_at < impl_->config.timeout)
        return false;
    auto result = impl_->edit();
    auto& probe = result->health.probes.front();
    probe = {};
    probe.probe_id = "first-visible-frame";
    probe.events = {{0, gh::Stage::readback, gh::Verdict::unavailable, "gpu.readback.unavailable",
                     "first visible frame timed out"}};
    probe.verdict = gh::Verdict::unavailable;
    result->health.run_id = "editor-open-timeout";
    result->health.verdict = gh::Verdict::unavailable;
    result->health.health_state = gh::HealthState::unavailable;
    gh::StartupTrial trial;
    trial.sequence = static_cast<std::uint32_t>(result->startup.trials.size());
    trial.cache_state =
        impl_->cache_state == CacheState::cold ? gh::CacheState::cold : gh::CacheState::warm;
    trial.verdict = gh::Verdict::unavailable;
    trial.diagnostic_code = "gpu.startup.timeout";
    result->startup.trials.push_back(std::move(trial));
    record_capture_event(result->startup.capture);
    derive_startup_state(result->startup);
    impl_->trace_transition(result->startup.trials.back().sequence,
                            result->startup.trials.back().diagnostic_code,
                            result->health.health_state);
    impl_->end_trace();
    impl_->requested_at.reset();
    impl_->publish(std::move(result));
    return true;
}

bool ControlGpuHealthProvider::record_instance_lost() noexcept {
    if (!impl_->on_writer() || !impl_->requested_at)
        return false;
    auto result = impl_->edit();
    auto& probe = result->health.probes.front();
    probe = {};
    probe.probe_id = "first-visible-frame";
    probe.events = {{0, gh::Stage::device_state, gh::Verdict::fail, "gpu.device_state.fail",
                     "bound editor instance was lost"}};
    probe.verdict = gh::Verdict::fail;
    probe.measurements.device_lost = true;
    result->health.run_id = "editor-open-instance-lost";
    result->health.verdict = gh::Verdict::fail;
    result->health.health_state = gh::HealthState::lost;
    gh::StartupTrial trial;
    trial.sequence = static_cast<std::uint32_t>(result->startup.trials.size());
    trial.cache_state =
        impl_->cache_state == CacheState::cold ? gh::CacheState::cold : gh::CacheState::warm;
    trial.verdict = gh::Verdict::unavailable;
    trial.diagnostic_code = "gpu.startup.instance_lost";
    result->startup.trials.push_back(std::move(trial));
    record_capture_event(result->startup.capture);
    derive_startup_state(result->startup);
    impl_->trace_transition(result->startup.trials.back().sequence,
                            result->startup.trials.back().diagnostic_code,
                            result->health.health_state);
    impl_->end_trace();
    impl_->requested_at.reset();
    impl_->publish(std::move(result));
    return true;
}

bool ControlGpuHealthProvider::record_dropped_events(std::uint64_t count) noexcept {
    if (!impl_->on_writer() || count == 0)
        return false;
    auto result = impl_->edit();
    result->startup.capture.dropped_event_count =
        count > kMaximumExactJsonInteger - result->startup.capture.dropped_event_count
            ? kMaximumExactJsonInteger
            : result->startup.capture.dropped_event_count + count;
    result->startup.capture.truncated = true;
    if (result->startup.trials.empty()) {
        gh::StartupTrial trial;
        trial.sequence = 0;
        trial.cache_state =
            impl_->cache_state == CacheState::cold ? gh::CacheState::cold : gh::CacheState::warm;
        trial.verdict = gh::Verdict::unverified;
        trial.diagnostic_code = "gpu.startup.event_loss";
        result->startup.trials.push_back(std::move(trial));
    } else {
        auto& trial = result->startup.trials.back();
        trial.verdict = gh::Verdict::unverified;
        trial.diagnostic_code = "gpu.startup.event_loss";
    }
    derive_startup_state(result->startup);
    impl_->publish(std::move(result));
    return true;
}

bool ControlGpuHealthProvider::awaiting_frame() const noexcept {
    return impl_->requested_at.has_value();
}

std::shared_ptr<const gh::HealthReadResult> ControlGpuHealthProvider::snapshot() const noexcept {
    return std::atomic_load_explicit(&impl_->current, std::memory_order_acquire);
}

} // namespace pulp::inspect
