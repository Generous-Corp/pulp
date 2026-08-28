#include <pulp/inspect/control_gpu_health_provider.hpp>
#include <pulp/inspect/control_gpu_health_view_adapter.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>

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
                    .backend = "Metal",
                    .name = "Apple GPU",
                    .vendor = "Apple",
                    .architecture = "Apple Silicon"},
        .capture_valid = true,
        .content_floor_passed = content,
        .non_transparent_pixel_count = 4096,
        .distinct_color_count = content ? 64u : 1u,
        .observed_signature_sha256 = std::string(64, content ? 'a' : 'b'),
        .observed_at = std::chrono::steady_clock::time_point{} + 12ms,
    };
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
    REQUIRE(provider.begin_editor_open(
        pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
        std::chrono::steady_clock::time_point{}));
    REQUIRE(provider.record_presented_frame(frame(true)));
    require_valid(provider);
    REQUIRE(provider.snapshot()->health.verdict == gh::Verdict::pass);
    REQUIRE(provider.snapshot()->startup.verdict == gh::Verdict::unverified);
    REQUIRE(provider.snapshot()->startup.trials.front().diagnostic_code ==
            "gpu.startup.unverified");
}

TEST_CASE("GPU health provider seeded blank first frame fails closed") {
    pulp::inspect::ControlGpuHealthProvider provider({
        .pulp_build_id = "test-build", .seed_blank_first_frame = true});
    REQUIRE(provider.begin_editor_open(
        pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
        std::chrono::steady_clock::time_point{}));
    REQUIRE(provider.record_presented_frame(frame(true)));
    require_valid(provider);
    REQUIRE(provider.snapshot()->health.verdict == gh::Verdict::fail);
    REQUIRE(provider.snapshot()->startup.trials.front().diagnostic_code ==
            "gpu.startup.blank");
}

TEST_CASE("GPU health provider timeout instance loss and event loss remain valid") {
    SECTION("timeout") {
        pulp::inspect::ControlGpuHealthProvider provider(
            {.pulp_build_id = "test-build", .timeout = 5ms});
        REQUIRE(provider.begin_editor_open(
            pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
            std::chrono::steady_clock::time_point{}));
        REQUIRE(provider.record_timeout(std::chrono::steady_clock::time_point{} + 6ms));
        require_valid(provider);
    }
    SECTION("instance loss") {
        pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
        REQUIRE(provider.begin_editor_open(
            pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
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
}

TEST_CASE("GPU health provider rejects producer writes from another thread") {
    pulp::inspect::ControlGpuHealthProvider provider({.pulp_build_id = "test-build"});
    auto future = std::async(std::launch::async, [&provider] {
        return provider.begin_editor_open(
            pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
            std::chrono::steady_clock::now());
    });
    REQUIRE_FALSE(future.get());
    require_valid(provider);
}

TEST_CASE("GPU health view adapter completes a real constrained 20-trial campaign") {
    using pulp::inspect::ControlGpuHealthProvider;
    using pulp::inspect::ControlGpuHealthViewAdapter;

    pulp::view::View root;
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

    auto provider = std::make_shared<ControlGpuHealthProvider>(
        ControlGpuHealthProvider::Config{.pulp_build_id = "headless-constrained-test"});
    auto adapter = ControlGpuHealthViewAdapter::create({
        .provider = provider,
        .capture_back_buffer_png = [&root] {
            return pulp::view::render_to_png(
                root, 160, 90, 1.0f, pulp::view::ScreenshotBackend::coregraphics);
        },
        .gpu_surface = [] { return static_cast<pulp::render::GpuSurface*>(nullptr); },
    });
    REQUIRE(adapter);

    const auto epoch = std::chrono::steady_clock::time_point{};
    for (std::uint32_t trial = 0; trial < 20; ++trial) {
        const auto requested_at = epoch + std::chrono::milliseconds(trial * 20);
        const auto cache_state = trial < 10 ? ControlGpuHealthProvider::CacheState::cold
                                            : ControlGpuHealthProvider::CacheState::warm;
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
    REQUIRE(snapshot->startup.interaction_hitch_percentile_ms == 19.0);
    REQUIRE(snapshot->startup.verdict == gh::Verdict::unverified);
    REQUIRE(snapshot->health.verdict == gh::Verdict::unverified);
    REQUIRE(snapshot->health.probes.front().adapter.status == gh::IdentityStatus::unavailable);
}

TEST_CASE("GPU health view adapter fails closed for a real blank capture") {
    auto provider = std::make_shared<pulp::inspect::ControlGpuHealthProvider>(
        pulp::inspect::ControlGpuHealthProvider::Config{
            .pulp_build_id = "headless-blank-negative"});
    auto adapter = pulp::inspect::ControlGpuHealthViewAdapter::create({
        .provider = provider,
        .capture_back_buffer_png = [] { return std::vector<std::uint8_t>{}; },
        .gpu_surface = [] { return static_cast<pulp::render::GpuSurface*>(nullptr); },
    });
    REQUIRE(adapter);
    REQUIRE(provider->begin_editor_open(
        pulp::inspect::ControlGpuHealthProvider::CacheState::cold,
        std::chrono::steady_clock::time_point{}));
    adapter->poll(std::chrono::steady_clock::time_point{} + 12ms);

    require_valid(*provider);
    REQUIRE(provider->snapshot()->health.verdict == gh::Verdict::fail);
    REQUIRE(provider->snapshot()->startup.trials.front().diagnostic_code ==
            "gpu.startup.blank");
}
