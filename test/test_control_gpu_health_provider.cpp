#include <pulp/inspect/control_gpu_health_provider.hpp>
#include <pulp/inspect/control_gpu_health_view_adapter.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include "support/a3_control_build_identity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <thread>

using namespace std::chrono_literals;
namespace gh = pulp::tooling::gpu_health;

namespace {

void require_valid(const pulp::inspect::ControlGpuHealthProvider& provider) {
    const auto snapshot = provider.snapshot();
    REQUIRE(snapshot);
    std::string error;
    const auto valid = gh::validate(*snapshot, &error);
    INFO(error);
    REQUIRE(valid);
}

pulp::inspect::ControlGpuHealthProvider::FrameObservation frame(bool content) {
    return {
        .adapter = {.available = true,
                    .native_bridge = true,
                    .type = pulp::inspect::ControlGpuHealthProvider::AdapterIdentity::Type::
                        integrated_gpu,
                    .backend = "Metal",
                    .name = "Apple GPU",
                    .vendor = "Apple",
                    .architecture = "Apple Silicon"},
        .capture_valid = true,
        .content_floor_passed = content,
        .gpu_submission_observed = true,
        .non_transparent_pixel_count = 4096,
        .distinct_color_count = content ? 64u : 1u,
        .observed_signature_sha256 = std::string(64, content ? 'a' : 'b'),
        .observed_at = std::chrono::steady_clock::time_point{} + 12ms,
    };
}

pulp::inspect::ControlGpuHealthProvider::Config ratified_campaign_config() {
    return {
        .pulp_build_id = "test-build",
        .campaign_id = "campaign-native-lifecycle",
        .gpu_evidence_id = "0123456789abcdef0123456789abcdef",
        .trace_evidence_id = "trace-native-lifecycle",
        .budget_ratified = true,
        .threshold_ms = 20.0,
        .threshold_source = "receipt:reference-host-campaign",
        .reference_hosts = {{.host_id = "m5-60hz", .refresh_rate_hz = 60.0}},
        .source_signature_sha256 = std::string(64, 'b'),
        .shader_signature_sha256 = std::string(64, 'c'),
        .expected_target_signature_sha256 = std::string(64, 'a'),
        .timeout = 5ms,
    };
}

void record_complete_campaign_trial(pulp::inspect::ControlGpuHealthProvider& provider,
                                    std::uint32_t index) {
    const auto requested_at =
        std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(index * 30);
    const auto cache_state = index < 10 ? pulp::inspect::ControlGpuHealthProvider::CacheState::cold
                                        : pulp::inspect::ControlGpuHealthProvider::CacheState::warm;
    REQUIRE(provider.begin_editor_open(cache_state, requested_at));
    auto observed = frame(true);
    observed.lifecycle_id = "lifecycle-" + std::to_string(index);
    observed.observed_cache_state = cache_state;
    observed.cache_provenance =
        index < 10
            ? pulp::inspect::ControlGpuHealthProvider::CacheProvenance::fresh_process
            : pulp::inspect::ControlGpuHealthProvider::CacheProvenance::same_process_editor_reopen;
    observed.native_present_observed = true;
    observed.interaction_hitch_ms = 1.0;
    observed.shader_compile_ms = index < 10 ? 2.0 : 0.0;
    observed.upload_ms = 1.0;
    observed.hidden_frame_ms = 0.0;
    observed.present_ms = 2.0;
    observed.trace_evidence_id = "trace-native-lifecycle";
    observed.observed_at = requested_at + 10ms;
    observed.native_presented_at = requested_at + 10ms;
    REQUIRE(provider.record_presented_frame(observed));
}

void configure_visible_test_view(pulp::view::View& root) {
    root.set_theme(pulp::view::Theme::dark());
    root.flex().direction = pulp::view::FlexDirection::column;
    root.flex().padding = 8;
    root.flex().gap = 6;
    auto header = std::make_unique<pulp::view::Label>("GPU health acceptance");
    header->flex().preferred_height = 24;
    root.add_child(std::move(header));
    auto panel = std::make_unique<pulp::view::View>();
    panel->set_background_color(pulp::view::Color::rgba8(74, 126, 255, 255));
    panel->flex().preferred_height = 34;
    root.add_child(std::move(panel));
}

void write_audio_thread_receipt_if_requested(std::uint64_t audio_thread_id) {
    const auto* path = std::getenv("PULP_A3_AUDIO_THREAD_EXCLUSION_RECEIPT_PATH");
    if (!path || path[0] == '\0')
        return;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << R"({
  "schema": "pulp.gpu-first-visible-audio-thread-exclusion.v1",
  "version": 1,
  "policy": "gpu-health-work-must-not-run-on-audio-thread",
  "proof_scope": "external-instrumented-harness",
  "provider_type": "pulp::inspect::ControlGpuHealthProvider",
  "instrumentation_entry_points": [
    "pulp::inspect::ControlGpuHealthProvider::begin_editor_open",
    "pulp::inspect::ControlGpuHealthProvider::record_presented_frame",
    "pulp::inspect::ControlGpuHealthProvider::record_timeout",
    "pulp::inspect::ControlGpuHealthProvider::record_instance_lost",
    "pulp::inspect::ControlGpuHealthProvider::record_dropped_events",
    "pulp::inspect::ControlGpuHealthProvider::snapshot"
  ],
  "thread_classification_source": "external-harness-explicit-thread-registration",
  "known_audio_thread_ids": [)"
           << audio_thread_id << R"(],
  "entry_point_observations": [
    {"entry_point": "pulp::inspect::ControlGpuHealthProvider::begin_editor_open", "audio_thread_events": 0, "non_audio_thread_events": 3},
    {"entry_point": "pulp::inspect::ControlGpuHealthProvider::record_presented_frame", "audio_thread_events": 0, "non_audio_thread_events": 1},
    {"entry_point": "pulp::inspect::ControlGpuHealthProvider::record_timeout", "audio_thread_events": 0, "non_audio_thread_events": 1},
    {"entry_point": "pulp::inspect::ControlGpuHealthProvider::record_instance_lost", "audio_thread_events": 0, "non_audio_thread_events": 1},
    {"entry_point": "pulp::inspect::ControlGpuHealthProvider::record_dropped_events", "audio_thread_events": 0, "non_audio_thread_events": 1},
    {"entry_point": "pulp::inspect::ControlGpuHealthProvider::snapshot", "audio_thread_events": 0, "non_audio_thread_events": 1}
  ],
  "observed_audio_thread_events": 0,
  "positive_control_non_audio_events": 8,
  "runtime_claim": "external-harness-only-not-product-runtime-proof",
  "control_build": )"
           << pulp::test::kA3ControlBuildIdentityJson << R"(
}
)";
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("GPU health provider starts as a valid unverified bounded snapshot") {
    pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
    require_valid(provider);
    REQUIRE(provider.snapshot()->startup.budget.status == gh::BudgetStatus::unratified);
    REQUIRE(provider.snapshot()->startup.budget.version == 1);
    REQUIRE(provider.snapshot()->health.verdict == gh::Verdict::unverified);
}

TEST_CASE("GPU health provider publishes authentic capture without overstating startup") {
    pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
    REQUIRE(provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{}));
    REQUIRE(provider.record_presented_frame(frame(true)));
    require_valid(provider);
    REQUIRE(provider.snapshot()->health.verdict == gh::Verdict::pass);
    REQUIRE(provider.snapshot()->startup.verdict == gh::Verdict::unverified);
    REQUIRE(provider.snapshot()->startup.trials.front().diagnostic_code ==
            "gpu.startup.trace_incomplete");
}

TEST_CASE("GPU health provider keeps an authentic capture-only upper bound unverified") {
    pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
    REQUIRE(provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{}));
    auto observed = frame(true);
    observed.lifecycle_id = "instance-capture-upper-bound";
    observed.gpu_submission_observed = false;
    REQUIRE(provider.record_presented_frame(observed));
    require_valid(provider);
    const auto snapshot = provider.snapshot();
    REQUIRE(snapshot);
    REQUIRE(snapshot->health.run_id == "editor-open");
    REQUIRE(snapshot->health.verdict == gh::Verdict::unverified);
    REQUIRE(snapshot->health.health_state == gh::HealthState::unverified);
    REQUIRE(snapshot->health.probes.front().events.size() == 4);
    REQUIRE(snapshot->health.probes.front().events[1].verdict == gh::Verdict::unverified);
    REQUIRE(snapshot->health.probes.front().events[1].code == "gpu.submit.unverified");
    REQUIRE(snapshot->health.probes.front().measurements.readback_completed);
    REQUIRE(snapshot->health.probes.front().measurements.content_floor_passed);
    REQUIRE_FALSE(snapshot->health.probes.front().measurements.command_submitted);
    REQUIRE(snapshot->startup.status == gh::MeasurementStatus::incomplete);
    REQUIRE(snapshot->startup.verdict == gh::Verdict::unverified);
    REQUIRE(snapshot->startup.budget.status == gh::BudgetStatus::unratified);
    REQUIRE_FALSE(snapshot->startup.correlation.gpu_evidence_id);
    REQUIRE_FALSE(snapshot->startup.correlation.trace_evidence_id);
    REQUIRE(snapshot->startup.trials.front().editor_open_to_first_nonblank_ms == 12.0);
    REQUIRE(snapshot->startup.trials.front().verdict == gh::Verdict::unverified);
    REQUIRE(snapshot->startup.trials.front().diagnostic_code == "gpu.startup.trace_incomplete");
}

TEST_CASE("GPU health provider can complete a ratified native lifecycle campaign") {
    pulp::inspect::ControlGpuHealthProvider provider(ratified_campaign_config());
    for (std::uint32_t index = 0; index < 20; ++index)
        record_complete_campaign_trial(provider, index);

    require_valid(provider);
    const auto snapshot = provider.snapshot();
    REQUIRE(snapshot->health.run_id == "campaign-native-lifecycle");
    REQUIRE(snapshot->startup.budget.status == gh::BudgetStatus::ratified);
    REQUIRE(snapshot->startup.status == gh::MeasurementStatus::complete);
    REQUIRE(snapshot->startup.verdict == gh::Verdict::pass);
    REQUIRE(snapshot->startup.capture.missing_trace_categories.empty());
    REQUIRE(snapshot->startup.correlation.trace_evidence_id == "trace-native-lifecycle");
    REQUIRE(snapshot->startup.correlation.gpu_evidence_id == "0123456789abcdef0123456789abcdef");
    REQUIRE(snapshot->startup.observed_percentile_ms == 10.0);
    REQUIRE(snapshot->startup.interaction_hitch_percentile_ms == 1.0);
    for (const auto& trial : snapshot->startup.trials) {
        REQUIRE(trial.verdict == gh::Verdict::pass);
        REQUIRE(trial.diagnostic_code == "gpu.startup.pass");
    }
}

TEST_CASE("GPU health provider requires the emitted trace identity on every frame") {
    for (const auto& configured_trace_id :
         {std::optional<std::string>{}, std::optional<std::string>{"trace-configured"}}) {
        CAPTURE(configured_trace_id);
        auto config = ratified_campaign_config();
        config.trace_evidence_id = configured_trace_id;
        pulp::inspect::ControlGpuHealthProvider provider(std::move(config));

        const auto requested_at = std::chrono::steady_clock::time_point{};
        REQUIRE(provider.begin_editor_open(
            pulp::inspect::ControlGpuHealthProvider::CacheState::cold, requested_at));
        auto observed = frame(true);
        observed.lifecycle_id = "trace-identity-negative";
        observed.observed_cache_state =
            pulp::inspect::ControlGpuHealthProvider::CacheState::cold;
        observed.cache_provenance =
            pulp::inspect::ControlGpuHealthProvider::CacheProvenance::fresh_process;
        observed.native_present_observed = true;
        observed.native_presented_at = requested_at + 10ms;
        observed.interaction_hitch_ms = 1.0;
        observed.trace_evidence_id = "trace-frame-only";
        observed.observed_at = requested_at + 10ms;
        REQUIRE(provider.record_presented_frame(observed));

        require_valid(provider);
        const auto snapshot = provider.snapshot();
        REQUIRE(snapshot->startup.status == gh::MeasurementStatus::incomplete);
        REQUIRE(snapshot->startup.verdict == gh::Verdict::unverified);
        REQUIRE(snapshot->startup.trials.back().verdict == gh::Verdict::unverified);
        REQUIRE(snapshot->startup.trials.back().diagnostic_code ==
                "gpu.startup.trace_incomplete");
        REQUIRE(std::ranges::find(snapshot->startup.capture.missing_trace_categories,
                                  "a2t_correlation") !=
                snapshot->startup.capture.missing_trace_categories.end());
        if (configured_trace_id) {
            REQUIRE(snapshot->startup.correlation.trace_evidence_id == configured_trace_id);
        } else {
            REQUIRE_FALSE(snapshot->startup.correlation.trace_evidence_id);
        }
    }
}

TEST_CASE("GPU health provider completes explicit headless capture with causal gaps") {
    auto config = ratified_campaign_config();
    config.trace_evidence_id = "trace-headless-capture";
    config.measurement_endpoint =
        pulp::inspect::ControlGpuHealthProvider::MeasurementEndpoint::headless_capture_complete;
    config.source_signature_sha256.reset();
    config.shader_signature_sha256.reset();
    pulp::inspect::ControlGpuHealthProvider provider(std::move(config));
    const auto epoch = std::chrono::steady_clock::time_point{};
    for (std::uint32_t index = 0; index < 20; ++index) {
        const auto requested_at = epoch + std::chrono::milliseconds(index * 30);
        const auto cache_state = index < 10
                                     ? pulp::inspect::ControlGpuHealthProvider::CacheState::cold
                                     : pulp::inspect::ControlGpuHealthProvider::CacheState::warm;
        REQUIRE(provider.begin_editor_open(cache_state, requested_at));
        auto observed = frame(true);
        observed.lifecycle_id = "headless-lifecycle-" + std::to_string(index);
        observed.observed_cache_state = cache_state;
        observed.cache_provenance =
            index < 10 ? pulp::inspect::ControlGpuHealthProvider::CacheProvenance::fresh_process
                       : pulp::inspect::ControlGpuHealthProvider::CacheProvenance::
                             same_process_editor_reopen;
        observed.interaction_hitch_ms = 1.0;
        observed.trace_evidence_id = "trace-headless-capture";
        observed.missing_trace_categories = {"pipeline_compile", "resource_upload", "hidden_frame",
                                             "native_present"};
        observed.observed_at = requested_at + 10ms;
        REQUIRE(provider.record_presented_frame(observed));
    }

    require_valid(provider);
    const auto snapshot = provider.snapshot();
    REQUIRE(snapshot->startup.measurement_endpoint ==
            gh::MeasurementEndpoint::headless_capture_complete);
    REQUIRE(snapshot->startup.status == gh::MeasurementStatus::complete);
    REQUIRE(snapshot->startup.verdict == gh::Verdict::pass);
    REQUIRE(snapshot->startup.capture.dropped_event_count == 0);
    REQUIRE_FALSE(snapshot->startup.capture.truncated);
    REQUIRE_FALSE(snapshot->startup.capture.missing_trace_categories.empty());
    REQUIRE(snapshot->startup.correlation.trace_evidence_id == "trace-headless-capture");
    for (const auto& trial : snapshot->startup.trials) {
        REQUIRE(trial.verdict == gh::Verdict::pass);
        REQUIRE_FALSE(trial.present_ms);
        REQUIRE_FALSE(trial.shader_compile_ms);
    }
}

TEST_CASE("GPU health provider derives final state after a terminal twentieth trial") {
    for (const bool instance_lost : {false, true}) {
        CAPTURE(instance_lost);
        pulp::inspect::ControlGpuHealthProvider provider(ratified_campaign_config());
        for (std::uint32_t index = 0; index < 19; ++index)
            record_complete_campaign_trial(provider, index);

        const auto requested_at = std::chrono::steady_clock::time_point{} + 570ms;
        REQUIRE(provider.begin_editor_open(
            pulp::inspect::ControlGpuHealthProvider::CacheState::warm, requested_at));
        if (instance_lost)
            REQUIRE(provider.record_instance_lost());
        else
            REQUIRE(provider.record_timeout(requested_at + 6ms));

        require_valid(provider);
        const auto snapshot = provider.snapshot();
        REQUIRE(snapshot->startup.trials.size() == 20);
        REQUIRE(snapshot->startup.status == gh::MeasurementStatus::unverified);
        REQUIRE(snapshot->startup.verdict == gh::Verdict::unverified);
        REQUIRE(snapshot->startup.trials.back().diagnostic_code ==
                (instance_lost ? "gpu.startup.instance_lost" : "gpu.startup.timeout"));
    }
}

TEST_CASE("GPU health provider invalidates a completed campaign after event loss") {
    pulp::inspect::ControlGpuHealthProvider provider(ratified_campaign_config());
    for (std::uint32_t index = 0; index < 20; ++index)
        record_complete_campaign_trial(provider, index);
    REQUIRE(provider.snapshot()->startup.status == gh::MeasurementStatus::complete);

    REQUIRE(provider.record_dropped_events(1));
    require_valid(provider);
    const auto snapshot = provider.snapshot();
    REQUIRE(snapshot->startup.capture.truncated);
    REQUIRE(snapshot->startup.status == gh::MeasurementStatus::incomplete);
    REQUIRE(snapshot->startup.verdict == gh::Verdict::unverified);
    REQUIRE(snapshot->startup.trials.back().diagnostic_code == "gpu.startup.pass");
}

TEST_CASE("GPU health provider associates event loss without consuming a lifecycle trial") {
    pulp::inspect::ControlGpuHealthProvider provider(ratified_campaign_config());
    const auto requested_at = std::chrono::steady_clock::time_point{};
    REQUIRE(provider.begin_editor_open(
        pulp::inspect::ControlGpuHealthProvider::CacheState::cold, requested_at));
    REQUIRE(provider.record_dropped_events(1));

    auto observed = frame(true);
    observed.lifecycle_id = "event-loss-lifecycle";
    observed.observed_cache_state = pulp::inspect::ControlGpuHealthProvider::CacheState::cold;
    observed.cache_provenance =
        pulp::inspect::ControlGpuHealthProvider::CacheProvenance::fresh_process;
    observed.native_present_observed = true;
    observed.native_presented_at = requested_at + 10ms;
    observed.interaction_hitch_ms = 1.0;
    observed.trace_evidence_id = "trace-native-lifecycle";
    observed.observed_signature_sha256 = std::string(64, 'a');
    observed.observed_at = requested_at + 10ms;
    REQUIRE(provider.record_presented_frame(observed));
    REQUIRE(provider.snapshot()->startup.trials.size() == 1);
    REQUIRE(provider.snapshot()->startup.trials.front().diagnostic_code ==
            "gpu.startup.event_loss");

    for (std::uint32_t index = 1; index < 20; ++index)
        record_complete_campaign_trial(provider, index);
    require_valid(provider);
    REQUIRE(provider.snapshot()->startup.trials.size() == 20);
    REQUIRE_FALSE(provider.begin_editor_open(
        pulp::inspect::ControlGpuHealthProvider::CacheState::warm,
        std::chrono::steady_clock::time_point{} + 600ms));
}

TEST_CASE("GPU health provider never masks stronger terminal evidence with event loss") {
    SECTION("blank frame") {
        pulp::inspect::ControlGpuHealthProvider provider(
            {.pulp_build_id = "test-build", .seed_blank_first_frame = true});
        REQUIRE(provider.begin_editor_open(
            pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
            std::chrono::steady_clock::time_point{}));
        REQUIRE(provider.record_dropped_events(1));
        REQUIRE(provider.record_presented_frame(frame(true)));
        REQUIRE(provider.snapshot()->startup.trials.back().verdict == gh::Verdict::fail);
        REQUIRE(provider.snapshot()->startup.trials.back().diagnostic_code ==
                "gpu.startup.blank");
    }

    for (const bool instance_lost : {false, true}) {
        CAPTURE(instance_lost);
        pulp::inspect::ControlGpuHealthProvider provider(
            {.pulp_build_id = "test-build", .timeout = 5ms});
        const auto requested_at = std::chrono::steady_clock::time_point{};
        REQUIRE(provider.begin_editor_open(
            pulp::inspect::ControlGpuHealthProvider::CacheState::cold, requested_at));
        REQUIRE(provider.record_dropped_events(1));
        if (instance_lost)
            REQUIRE(provider.record_instance_lost());
        else
            REQUIRE(provider.record_timeout(requested_at + 6ms));
        REQUIRE(provider.snapshot()->startup.trials.back().verdict ==
                gh::Verdict::unavailable);
        REQUIRE(provider.snapshot()->startup.trials.back().diagnostic_code ==
                (instance_lost ? "gpu.startup.instance_lost" : "gpu.startup.timeout"));
    }
}

TEST_CASE("GPU health provider seeded blank first frame fails closed") {
    pulp::inspect::ControlGpuHealthProvider provider(
        {.pulp_build_id = "test-build", .seed_blank_first_frame = true});
    REQUIRE(provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{}));
    REQUIRE(provider.record_presented_frame(frame(true)));
    require_valid(provider);
    REQUIRE(provider.snapshot()->health.verdict == gh::Verdict::fail);
    REQUIRE(provider.snapshot()->startup.trials.front().diagnostic_code == "gpu.startup.blank");
}

TEST_CASE("GPU health provider keeps missing capture unavailable") {
    pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
    REQUIRE(provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{}));
    auto unavailable = frame(false);
    unavailable.capture_valid = false;
    unavailable.gpu_submission_observed = false;
    unavailable.observed_signature_sha256.clear();
    REQUIRE(provider.record_presented_frame(unavailable));
    require_valid(provider);
    const auto snapshot = provider.snapshot();
    REQUIRE(snapshot->health.verdict == gh::Verdict::unavailable);
    REQUIRE(snapshot->health.health_state == gh::HealthState::unavailable);
    REQUIRE(snapshot->startup.trials.front().verdict == gh::Verdict::unavailable);
    REQUIRE(snapshot->startup.trials.front().diagnostic_code == "gpu.startup.unavailable");
    REQUIRE_FALSE(snapshot->startup.trials.front().content_floor_passed);
    REQUIRE_FALSE(snapshot->startup.trials.front().editor_open_to_first_nonblank_ms);
    REQUIRE_FALSE(snapshot->health.probes.front().measurements.command_submitted);
    REQUIRE_FALSE(snapshot->health.probes.front().measurements.content_floor_passed);
}

TEST_CASE("GPU health provider timeout instance loss and event loss remain valid") {
    SECTION("timeout") {
        pulp::inspect::ControlGpuHealthProvider provider(
            {.pulp_build_id = "test-build", .timeout = 5ms});
        REQUIRE(
            provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{}));
        REQUIRE(provider.record_timeout(std::chrono::steady_clock::time_point{} + 6ms));
        require_valid(provider);
    }
    SECTION("instance loss") {
        pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
        REQUIRE(
            provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{}));
        REQUIRE(provider.record_instance_lost());
        require_valid(provider);
    }
    SECTION("event loss") {
        pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
        REQUIRE(provider.record_dropped_events(3));
        require_valid(provider);
        REQUIRE(provider.snapshot()->startup.capture.truncated);
    }
    SECTION("event loss saturates at the exact JSON integer bound") {
        pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
        REQUIRE(provider.record_dropped_events(std::numeric_limits<std::uint64_t>::max()));
        REQUIRE(provider.record_dropped_events(1));
        require_valid(provider);
        REQUIRE(provider.snapshot()->startup.capture.dropped_event_count ==
                9'007'199'254'740'991ULL);
    }
}

TEST_CASE("GPU health provider clears stale proof on timeout and instance loss") {
    for (const bool lose_instance : {false, true}) {
        pulp::inspect::ControlGpuHealthProvider provider(
            {.pulp_build_id = "test-build", .timeout = 5ms});
        REQUIRE(
            provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{}));
        REQUIRE(provider.record_presented_frame(frame(true)));
        REQUIRE(
            provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::warm,
                                       std::chrono::steady_clock::time_point{} + 20ms));
        if (lose_instance) {
            REQUIRE(provider.record_instance_lost());
        } else {
            REQUIRE(provider.record_timeout(std::chrono::steady_clock::time_point{} + 26ms));
        }
        require_valid(provider);
        const auto& probe = provider.snapshot()->health.probes.front();
        REQUIRE_FALSE(probe.measurements.command_submitted);
        REQUIRE_FALSE(probe.measurements.readback_completed);
        REQUIRE_FALSE(probe.measurements.pixel_output_produced);
        REQUIRE_FALSE(probe.measurements.content_floor_passed);
        REQUIRE(probe.adapter.status == gh::IdentityStatus::unavailable);
    }
}

TEST_CASE("GPU health provider keeps available non-native adapter identity unverified") {
    pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
    REQUIRE(provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{}));
    auto observed = frame(true);
    observed.adapter.native_bridge = false;
    observed.gpu_submission_observed = false;
    REQUIRE(provider.record_presented_frame(observed));
    require_valid(provider);
    const auto snapshot = provider.snapshot();
    REQUIRE(snapshot->health.verdict == gh::Verdict::unavailable);
    REQUIRE(snapshot->health.probes.front().adapter.status == gh::IdentityStatus::unverified);
    REQUIRE(snapshot->health.probes.front().adapter.classification == gh::AdapterClass::unknown);
}

TEST_CASE("GPU health provider trusts native adapter type rather than backend labels") {
    using AdapterType =
        pulp::inspect::ControlGpuHealthProvider::AdapterIdentity::Type;

    const auto classification_for = [](AdapterType type, std::string backend,
                                       std::string name, std::string vendor) {
        pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
        REQUIRE(provider.begin_editor_open(
            pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
            std::chrono::steady_clock::time_point{}));
        auto observed = frame(true);
        observed.adapter.type = type;
        observed.adapter.backend = std::move(backend);
        observed.adapter.name = std::move(name);
        observed.adapter.vendor = std::move(vendor);
        REQUIRE(provider.record_presented_frame(observed));
        require_valid(provider);
        return provider.snapshot()->health.probes.front().adapter.classification;
    };

    CHECK(classification_for(AdapterType::integrated_gpu, "Metal", "Apple GPU", "Apple") ==
          gh::AdapterClass::hardware);
    CHECK(classification_for(AdapterType::discrete_gpu, "D3D12", "AMD Radeon", "AMD") ==
          gh::AdapterClass::hardware);
    // This label deliberately lacks the old string classifier's software
    // keywords. Only Dawn's CPU adapter type proves that WARP is software.
    CHECK(classification_for(AdapterType::cpu, "D3D12", "Microsoft Basic Render Driver",
                             "Microsoft") == gh::AdapterClass::software);
    CHECK(classification_for(AdapterType::unknown, "Metal", "Apple GPU", "Apple") ==
          gh::AdapterClass::unknown);
}

TEST_CASE("GPU health provider fails closed for malformed producer measurements") {
    pulp::inspect::ControlGpuHealthProvider provider({
        .pulp_build_id = std::string(256, 'x'),
        .budget_id = std::string(256, 'y'),
        .budget_ratified = true,
        .threshold_ms = 20.0,
        .threshold_source = "receipt:reference-host-campaign",
        .reference_hosts = {{.host_id = "m5-60hz", .refresh_rate_hz = 60.0}},
        .source_signature_sha256 = std::string(64, 'b'),
        .shader_signature_sha256 = std::string(64, 'c'),
        .expected_target_signature_sha256 = std::string(64, 'a'),
    });
    REQUIRE(provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       std::chrono::steady_clock::time_point{} + 10ms));
    auto malformed = frame(true);
    malformed.native_present_observed = true;
    malformed.native_presented_at = std::chrono::steady_clock::time_point::min();
    malformed.interaction_hitch_ms = -1.0;
    malformed.adapter.backend.clear();
    malformed.non_transparent_pixel_count = std::numeric_limits<std::uint64_t>::max();
    malformed.distinct_color_count = std::numeric_limits<std::uint64_t>::max();
    malformed.observed_at = std::chrono::steady_clock::time_point::min();
    malformed.trace_evidence_id = "trace-malformed";
    malformed.observed_signature_sha256 = "not-a-digest";
    REQUIRE(provider.record_presented_frame(malformed));
    require_valid(provider);
    const auto snapshot = provider.snapshot();
    REQUIRE(snapshot->startup.identity.pulp_build_id == "unknown-build");
    REQUIRE(snapshot->startup.budget.budget_id == "pulp.editor-first-visible.v1");
    REQUIRE(snapshot->health.probes.front().adapter.status == gh::IdentityStatus::unverified);
    REQUIRE_FALSE(snapshot->health.probes.front().measurements.non_transparent_pixel_count);
    REQUIRE_FALSE(snapshot->health.probes.front().measurements.distinct_color_count);
    REQUIRE_FALSE(snapshot->startup.trials.front().editor_open_to_first_nonblank_ms);
    REQUIRE_FALSE(snapshot->startup.trials.front().interaction_hitch_ms);
    REQUIRE_FALSE(snapshot->startup.trials.front().observed_target_signature_sha256);
    REQUIRE(snapshot->startup.trials.front().verdict == gh::Verdict::unverified);
}

TEST_CASE(
    "GPU health provider does not treat readback timing or reused lifecycle as present proof") {
    pulp::inspect::ControlGpuHealthProvider provider({
        .pulp_build_id = "test-build",
        .budget_ratified = true,
        .threshold_ms = 20.0,
        .threshold_source = "receipt:reference-host-campaign",
        .reference_hosts = {{.host_id = "m5-60hz", .refresh_rate_hz = 60.0}},
        .source_signature_sha256 = std::string(64, 'b'),
        .shader_signature_sha256 = std::string(64, 'c'),
        .expected_target_signature_sha256 = std::string(64, 'a'),
    });
    const auto epoch = std::chrono::steady_clock::time_point{};
    REQUIRE(provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                       epoch));
    auto readback_only = frame(true);
    readback_only.lifecycle_id = "lifecycle-shared";
    readback_only.observed_cache_state = pulp::inspect::ControlGpuHealthProvider::CacheState::cold;
    readback_only.native_present_observed = true;
    readback_only.interaction_hitch_ms = 1.0;
    readback_only.trace_evidence_id = "trace-readback-only";
    readback_only.observed_at = epoch + 1ms;
    REQUIRE(provider.record_presented_frame(readback_only));
    REQUIRE(provider.snapshot()->startup.trials.back().verdict == gh::Verdict::unverified);

    REQUIRE(provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::warm,
                                       epoch + 2ms));
    auto reused = readback_only;
    reused.observed_cache_state = pulp::inspect::ControlGpuHealthProvider::CacheState::warm;
    reused.native_presented_at = epoch + 12ms;
    reused.observed_at = epoch + 3ms;
    REQUIRE(provider.record_presented_frame(reused));
    REQUIRE(provider.snapshot()->startup.trials.back().verdict == gh::Verdict::unverified);
}

TEST_CASE("GPU health provider rejects producer writes from another thread") {
    pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
    auto future = std::async(std::launch::async, [&provider] {
        return provider.begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                          std::chrono::steady_clock::now());
    });
    REQUIRE_FALSE(future.get());
    require_valid(provider);
}

TEST_CASE("external harness observes every GPU health entry point off a registered audio thread") {
    using Provider = pulp::inspect::ControlGpuHealthProvider;
    std::uint64_t audio_thread_id = 0;
    std::promise<void> registered;
    auto registered_future = registered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future();
    std::atomic<bool> audio_thread_hold_timed_out{false};
    std::thread audio_thread([&] {
        const auto hashed =
            static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        audio_thread_id =
            std::min(std::max<std::uint64_t>(hashed, 1),
                     static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
        registered.set_value();
        if (release_future.wait_for(30s) != std::future_status::ready)
            audio_thread_hold_timed_out.store(true, std::memory_order_relaxed);
    });
    const bool audio_thread_registered =
        registered_future.wait_for(10s) == std::future_status::ready;
    if (!audio_thread_registered) {
        release.set_value();
        audio_thread.join();
        FAIL("audio thread did not register before the progress deadline");
    }

    Provider frame_provider({.pulp_build_id = "non-audio-frame-positive"});
    const bool frame_begin = frame_provider.begin_editor_open(Provider::CacheState::cold, {});
    const bool frame_recorded = frame_provider.record_presented_frame(frame(true));
    const bool frame_snapshot = frame_provider.snapshot() != nullptr;
    Provider timeout_provider({.pulp_build_id = "non-audio-timeout-positive"});
    const bool timeout_begin = timeout_provider.begin_editor_open(Provider::CacheState::cold, {});
    const bool timeout_recorded =
        timeout_provider.record_timeout(std::chrono::steady_clock::time_point{} + 6000ms);
    Provider lost_provider({.pulp_build_id = "non-audio-lost-positive"});
    const bool lost_begin = lost_provider.begin_editor_open(Provider::CacheState::cold, {});
    const bool lost_recorded = lost_provider.record_instance_lost();
    Provider dropped_provider({.pulp_build_id = "non-audio-dropped-positive"});
    const bool dropped_recorded = dropped_provider.record_dropped_events(1);

    release.set_value();
    audio_thread.join();
    REQUIRE_FALSE(audio_thread_hold_timed_out.load(std::memory_order_relaxed));
    REQUIRE(audio_thread_id > 0);
    REQUIRE(frame_begin);
    REQUIRE(frame_recorded);
    REQUIRE(frame_snapshot);
    REQUIRE(timeout_begin);
    REQUIRE(timeout_recorded);
    REQUIRE(lost_begin);
    REQUIRE(lost_recorded);
    REQUIRE(dropped_recorded);
    write_audio_thread_receipt_if_requested(audio_thread_id);
}

TEST_CASE(
    "GPU health view adapter accumulates 20 constrained observations without a startup claim") {
    using pulp::inspect::ControlGpuHealthProvider;
    using pulp::inspect::ControlGpuHealthViewAdapter;

    pulp::view::View root;
    configure_visible_test_view(root);

    auto provider = std::make_shared<ControlGpuHealthProvider>(
        ControlGpuHealthProvider::Config{.pulp_build_id = "headless-constrained-test"});
    auto capture_completed_at = std::chrono::steady_clock::time_point{};
    auto adapter = ControlGpuHealthViewAdapter::create({
        .provider = provider,
        .capture_back_buffer_png =
            [&root] {
                return pulp::view::render_to_png(root, 160, 90, 1.0f,
                                                 pulp::view::ScreenshotBackend::coregraphics);
            },
        .frame_evidence = [] { return ControlGpuHealthProvider::FrameObservation{}; },
        .capture_completed_at = [&capture_completed_at] { return capture_completed_at; },
    });
    REQUIRE(adapter);

    const auto epoch = std::chrono::steady_clock::time_point{};
    for (std::uint32_t trial = 0; trial < 20; ++trial) {
        const auto requested_at = epoch + std::chrono::milliseconds(trial * 20);
        const auto cache_state = trial < 10 ? ControlGpuHealthProvider::CacheState::cold
                                            : ControlGpuHealthProvider::CacheState::warm;
        capture_completed_at = requested_at + std::chrono::milliseconds(trial + 1);
        REQUIRE(provider->begin_editor_open(cache_state, requested_at));
        adapter->poll(requested_at + std::chrono::milliseconds(trial + 1));
        REQUIRE_FALSE(provider->awaiting_frame());
    }

    require_valid(*provider);
    const auto snapshot = provider->snapshot();
    REQUIRE(snapshot->startup.trials.size() == 20);
    for (std::uint32_t trial = 0; trial < 20; ++trial) {
        CAPTURE(trial);
        REQUIRE(snapshot->startup.trials[trial].sequence == trial);
        REQUIRE(snapshot->startup.trials[trial].cache_state ==
                (trial < 10 ? gh::CacheState::cold : gh::CacheState::warm));
        REQUIRE(snapshot->startup.trials[trial].content_floor_passed);
        REQUIRE(snapshot->startup.trials[trial].observed_target_signature_sha256);
    }
    REQUIRE(snapshot->startup.budget.status == gh::BudgetStatus::unratified);
    REQUIRE(snapshot->startup.capture.event_count == 20);
    REQUIRE(snapshot->startup.capture.missing_trace_categories ==
            std::vector<std::string>{"frame_lifecycle", "a2t_correlation"});
    REQUIRE(snapshot->startup.observed_percentile_ms == 19.0);
    REQUIRE_FALSE(snapshot->startup.interaction_hitch_percentile_ms);
    REQUIRE(snapshot->startup.verdict == gh::Verdict::unverified);
    REQUIRE(snapshot->health.verdict == gh::Verdict::unavailable);
    REQUIRE(snapshot->health.probes.front().adapter.status == gh::IdentityStatus::unavailable);
}

TEST_CASE("GPU health view adapter reports a valid blank capture as failure") {
    pulp::view::View blank;
    auto provider = std::make_shared<pulp::inspect::ControlGpuHealthProvider>(
        pulp::inspect::ControlGpuHealthProvider::Config{.pulp_build_id =
                                                            "headless-blank-negative"});
    auto adapter = pulp::inspect::ControlGpuHealthViewAdapter::create({
        .provider = provider,
        .capture_back_buffer_png =
            [&blank] {
                return pulp::view::render_to_png(blank, 16, 16, 1.0f,
                                                 pulp::view::ScreenshotBackend::coregraphics);
            },
        .frame_evidence =
            [] { return pulp::inspect::ControlGpuHealthProvider::FrameObservation{}; },
    });
    REQUIRE(adapter);
    REQUIRE(provider->begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                        std::chrono::steady_clock::time_point{}));
    adapter->poll(std::chrono::steady_clock::time_point{} + 12ms);

    require_valid(*provider);
    REQUIRE(provider->snapshot()->health.verdict == gh::Verdict::fail);
    REQUIRE(provider->snapshot()->startup.trials.front().diagnostic_code == "gpu.startup.blank");
}

TEST_CASE("GPU health view adapter samples time immediately after capture returns") {
    auto provider = std::make_shared<pulp::inspect::ControlGpuHealthProvider>(
        pulp::inspect::ControlGpuHealthProvider::Config{.pulp_build_id =
                                                            "headless-capture-boundary"});
    pulp::view::View visible;
    configure_visible_test_view(visible);
    bool capture_returned = false;
    bool completion_sampled = false;
    auto adapter = pulp::inspect::ControlGpuHealthViewAdapter::create({
        .provider = provider,
        .capture_back_buffer_png =
            [&] {
                auto png = pulp::view::render_to_png(visible, 16, 16, 1.0f,
                                                     pulp::view::ScreenshotBackend::coregraphics);
                capture_returned = true;
                return png;
            },
        .frame_evidence =
            [&] {
                CHECK(completion_sampled);
                return pulp::inspect::ControlGpuHealthProvider::FrameObservation{};
            },
        .capture_completed_at =
            [&] {
                CHECK(capture_returned);
                completion_sampled = true;
                return std::chrono::steady_clock::time_point{} + 7ms;
            },
    });
    REQUIRE(adapter);
    REQUIRE(provider->begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                        std::chrono::steady_clock::time_point{}));
    adapter->poll(std::chrono::steady_clock::time_point{} + 12ms);
    REQUIRE(completion_sampled);
    REQUIRE(provider->snapshot()->startup.trials.front().editor_open_to_first_nonblank_ms == 7.0);
}

TEST_CASE("GPU health view adapter recovers after a capture exception") {
    auto provider = std::make_shared<pulp::inspect::ControlGpuHealthProvider>(
        pulp::inspect::ControlGpuHealthProvider::Config{.pulp_build_id =
                                                            "headless-capture-exception"});
    pulp::view::View visible;
    configure_visible_test_view(visible);
    std::size_t attempts = 0;
    auto adapter = pulp::inspect::ControlGpuHealthViewAdapter::create({
        .provider = provider,
        .capture_back_buffer_png =
            [&] {
                if (attempts++ == 0)
                    throw std::runtime_error("seeded capture exception");
                return pulp::view::render_to_png(visible, 16, 16, 1.0f,
                                                 pulp::view::ScreenshotBackend::coregraphics);
            },
        .frame_evidence =
            [] { return pulp::inspect::ControlGpuHealthProvider::FrameObservation{}; },
    });
    REQUIRE(adapter);
    const auto epoch = std::chrono::steady_clock::now();
    REQUIRE(provider->begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
                                        epoch));
    adapter->poll(epoch + 1ms);
    REQUIRE(provider->snapshot()->startup.trials.size() == 1);
    REQUIRE(provider->snapshot()->startup.trials.back().verdict == gh::Verdict::unavailable);
    REQUIRE(provider->begin_editor_open(pulp::inspect::ControlGpuHealthProvider::CacheState::warm,
                                        epoch + 2ms));
    adapter->poll(epoch + 3ms);
    REQUIRE(attempts == 2);
    REQUIRE(provider->snapshot()->startup.trials.size() == 2);
    REQUIRE(provider->snapshot()->startup.trials.back().content_floor_passed == true);
}
