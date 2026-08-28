#include <pulp/inspect/control_gpu_health_provider.hpp>
#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>

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
