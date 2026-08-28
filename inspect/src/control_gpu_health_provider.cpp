#include <pulp/inspect/control_gpu_health_provider.hpp>

#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <utility>

namespace pulp::inspect {
namespace gh = tooling::gpu_health;
namespace {

gh::AdapterClass classify(const ControlGpuHealthProvider::AdapterIdentity& adapter) {
    if (!adapter.available)
        return gh::AdapterClass::unknown;
    auto text = adapter.backend + " " + adapter.name + " " + adapter.vendor;
    std::ranges::transform(text, text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (text.find("null") != std::string::npos)
        return gh::AdapterClass::null_adapter;
    if (text.find("software") != std::string::npos ||
        text.find("swiftshader") != std::string::npos ||
        text.find("llvmpipe") != std::string::npos)
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
    result.status = value.native_bridge ? gh::IdentityStatus::authentic
                                        : gh::IdentityStatus::unverified;
    if (!value.backend.empty()) result.backend = value.backend;
    if (!value.name.empty()) result.name = value.name;
    if (!value.vendor.empty()) result.vendor = value.vendor;
    if (!value.architecture.empty()) result.architecture = value.architecture;
    return result;
}

gh::ProbeEvidence initial_probe() {
    gh::ProbeEvidence probe;
    probe.probe_id = "first-visible-frame";
    probe.events.push_back({0, gh::Stage::adapter, gh::Verdict::unverified,
                            "gpu.adapter.unverified",
                            "editor-open measurement is armed but no frame has been observed"});
    return probe;
}

} // namespace

struct ControlGpuHealthProvider::Impl {
    explicit Impl(Config value) : config(std::move(value)), writer(std::this_thread::get_id()) {
        auto result = std::make_shared<gh::HealthReadResult>();
        result->health.run_id = "editor-open-pending";
        result->health.probes.push_back(initial_probe());
        result->startup.status = gh::MeasurementStatus::incomplete;
        result->startup.budget.budget_id = config.budget_id;
        result->startup.budget.version = std::max<std::uint32_t>(1, config.budget_version);
        result->startup.capture.event_capacity =
            std::clamp<std::uint32_t>(config.event_capacity, 1, 4096);
        result->startup.identity.pulp_build_id = config.pulp_build_id;
        publish(std::move(result));
    }

    bool on_writer() const noexcept { return std::this_thread::get_id() == writer; }
    void publish(std::shared_ptr<gh::HealthReadResult> next) noexcept {
        std::atomic_store_explicit(&current,
            std::shared_ptr<const gh::HealthReadResult>(std::move(next)),
            std::memory_order_release);
    }
    std::shared_ptr<gh::HealthReadResult> edit() const {
        return std::make_shared<gh::HealthReadResult>(
            *std::atomic_load_explicit(&current, std::memory_order_acquire));
    }

    Config config;
    std::thread::id writer;
    std::shared_ptr<const gh::HealthReadResult> current;
    std::optional<std::chrono::steady_clock::time_point> requested_at;
    CacheState cache_state = CacheState::cold;
};

ControlGpuHealthProvider::ControlGpuHealthProvider(Config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
ControlGpuHealthProvider::~ControlGpuHealthProvider() = default;

bool ControlGpuHealthProvider::begin_editor_open(
    CacheState cache_state, std::chrono::steady_clock::time_point requested_at) noexcept {
    if (!impl_->on_writer() || impl_->requested_at)
        return false;
    impl_->cache_state = cache_state;
    impl_->requested_at = requested_at;
    return true;
}

bool ControlGpuHealthProvider::record_presented_frame(const FrameObservation& frame) noexcept {
    if (!impl_->on_writer() || !impl_->requested_at)
        return false;
    auto result = impl_->edit();
    const auto elapsed = std::chrono::duration<double, std::milli>(
        frame.observed_at - *impl_->requested_at).count();
    const bool blank = impl_->config.seed_blank_first_frame || !frame.capture_valid ||
                       !frame.content_floor_passed;
    const auto adapter = identity(frame.adapter);
    auto probe = gh::ProbeEvidence{};
    probe.probe_id = "first-visible-frame";
    probe.adapter = adapter;
    probe.measurements.command_submitted = true;
    probe.measurements.readback_completed = frame.capture_valid;
    probe.measurements.pixel_output_produced = frame.capture_valid;
    probe.measurements.content_floor_passed = !blank;
    if (frame.capture_valid) {
        probe.measurements.non_transparent_pixel_count = frame.non_transparent_pixel_count;
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
    probe.events.push_back({sequence++, gh::Stage::submit, gh::Verdict::pass,
                            "gpu.submit.pass", "host frame reached capture observation"});
    probe.events.push_back({sequence++, gh::Stage::readback,
                            frame.capture_valid ? gh::Verdict::pass : gh::Verdict::unavailable,
                            frame.capture_valid ? "gpu.readback.pass" : "gpu.readback.unavailable",
                            frame.capture_valid ? "host back buffer readback completed"
                                                : "host back buffer readback was unavailable"});
    probe.events.push_back({sequence++, gh::Stage::content,
                            blank ? gh::Verdict::fail : gh::Verdict::pass,
                            blank ? "gpu.content.fail" : "gpu.content.pass",
                            blank ? "blank-first-frame negative control triggered"
                                  : "captured frame passed the content floor"});
    probe.verdict = blank ? gh::Verdict::fail
                          : (adapter.status == gh::IdentityStatus::authentic
                                 ? gh::Verdict::pass : gh::Verdict::unverified);
    result->health.run_id = frame.observed_signature_sha256.empty()
                                ? "editor-open-frame" : frame.observed_signature_sha256;
    result->health.probes = {std::move(probe)};
    result->health.verdict = result->health.probes.front().verdict;
    result->health.health_state = blank ? gh::HealthState::failed
        : (result->health.verdict == gh::Verdict::pass ? gh::HealthState::healthy
                                                       : gh::HealthState::unverified);
    result->startup.identity.adapter_class = classify(frame.adapter);
    result->startup.capture.event_count = 1;
    gh::StartupTrial trial;
    trial.sequence = static_cast<std::uint32_t>(result->startup.trials.size());
    trial.cache_state = impl_->cache_state == CacheState::cold ? gh::CacheState::cold
                                                               : gh::CacheState::warm;
    trial.editor_open_to_first_nonblank_ms = std::max(0.0, elapsed);
    trial.interaction_hitch_ms = std::max(0.0, elapsed);
    if (!frame.observed_signature_sha256.empty())
        trial.observed_target_signature_sha256 = frame.observed_signature_sha256;
    trial.content_floor_passed = !blank;
    trial.visible_state = blank ? gh::VisibleState::fallback : gh::VisibleState::prepared;
    trial.verdict = blank ? gh::Verdict::fail : gh::Verdict::unverified;
    trial.diagnostic_code = blank ? "gpu.startup.blank" : "gpu.startup.unverified";
    result->startup.trials.push_back(std::move(trial));
    result->startup.observed_percentile_ms = std::max(0.0, elapsed);
    result->startup.interaction_hitch_percentile_ms = std::max(0.0, elapsed);
    result->startup.correlation.gpu_evidence_id = result->health.run_id;
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
    probe.events = {{0, gh::Stage::readback, gh::Verdict::unavailable,
                     "gpu.readback.unavailable", "first visible frame timed out"}};
    probe.verdict = gh::Verdict::unavailable;
    result->health.run_id = "editor-open-timeout";
    result->health.verdict = gh::Verdict::unavailable;
    result->health.health_state = gh::HealthState::unavailable;
    gh::StartupTrial trial;
    trial.sequence = static_cast<std::uint32_t>(result->startup.trials.size());
    trial.cache_state = impl_->cache_state == CacheState::cold ? gh::CacheState::cold
                                                               : gh::CacheState::warm;
    trial.verdict = gh::Verdict::unavailable;
    trial.diagnostic_code = "gpu.startup.timeout";
    result->startup.trials.push_back(std::move(trial));
    result->startup.capture.event_count = 1;
    impl_->requested_at.reset();
    impl_->publish(std::move(result));
    return true;
}

bool ControlGpuHealthProvider::record_instance_lost() noexcept {
    if (!impl_->on_writer() || !impl_->requested_at)
        return false;
    auto result = impl_->edit();
    auto& probe = result->health.probes.front();
    probe.events = {{0, gh::Stage::device_state, gh::Verdict::fail,
                     "gpu.device_state.fail", "bound editor instance was lost"}};
    probe.verdict = gh::Verdict::fail;
    probe.measurements.device_lost = true;
    result->health.run_id = "editor-open-instance-lost";
    result->health.verdict = gh::Verdict::fail;
    result->health.health_state = gh::HealthState::lost;
    gh::StartupTrial trial;
    trial.sequence = static_cast<std::uint32_t>(result->startup.trials.size());
    trial.cache_state = impl_->cache_state == CacheState::cold ? gh::CacheState::cold
                                                               : gh::CacheState::warm;
    trial.verdict = gh::Verdict::unavailable;
    trial.diagnostic_code = "gpu.startup.instance_lost";
    result->startup.trials.push_back(std::move(trial));
    result->startup.capture.event_count = 1;
    impl_->requested_at.reset();
    impl_->publish(std::move(result));
    return true;
}

bool ControlGpuHealthProvider::record_dropped_events(std::uint64_t count) noexcept {
    if (!impl_->on_writer() || count == 0)
        return false;
    auto result = impl_->edit();
    result->startup.capture.dropped_event_count += count;
    result->startup.capture.truncated = true;
    if (result->startup.trials.empty()) {
        gh::StartupTrial trial;
        trial.sequence = 0;
        trial.cache_state = impl_->cache_state == CacheState::cold ? gh::CacheState::cold
                                                                   : gh::CacheState::warm;
        trial.verdict = gh::Verdict::unverified;
        trial.diagnostic_code = "gpu.startup.event_loss";
        result->startup.trials.push_back(std::move(trial));
    } else {
        auto& trial = result->startup.trials.back();
        trial.verdict = gh::Verdict::unverified;
        trial.diagnostic_code = "gpu.startup.event_loss";
    }
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
