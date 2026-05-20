// Phase 6.5 — Dawn GPU timestamp queries.
//
// These tests cover the *pure* timestamp-resolution logic: raw tick ->
// nanosecond -> millisecond conversion, the feature-absent fallback, and
// the non-monotonic-miss rule. They run without a GPU because
// `GpuTimestampResolver` is deliberately decoupled from Dawn (it consumes
// a plain buffer of resolved ticks — exactly what `ResolveQuerySet`
// produces). The live-device path (`GpuPassTimer`, query-set lifecycle)
// is exercised by CI's GPU matrix; here we mock the resolved buffer.
//
// Spike reference: planning/2026-05-19-inspector-phase6-gpu-perf-spike.md
// § Phase 6.5.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/render/gpu_timestamp.hpp>
#include <pulp/render/render_pass.hpp>

#include <vector>

using namespace pulp::render;
using Catch::Matchers::WithinAbs;

// ── ns -> ms conversion ─────────────────────────────────────────────────

TEST_CASE("GpuTimestampResolver converts nanosecond ticks to milliseconds",
          "[render][gpu][timestamp][issue-290]") {
    GpuTimestampResolver resolver;  // 1.0 ns/tick — WebGPU default

    // 1,000,000 ns == 1.0 ms.
    REQUIRE_THAT(resolver.ticks_to_ms(1'000'000), WithinAbs(1.0, 1e-9));
    // 16,670,000 ns == one 60fps frame budget.
    REQUIRE_THAT(resolver.ticks_to_ms(16'670'000), WithinAbs(16.67, 1e-6));
    // Zero ticks is exactly zero ms.
    REQUIRE_THAT(resolver.ticks_to_ms(0), WithinAbs(0.0, 1e-12));
}

TEST_CASE("GpuTimestampResolver honours a non-nanosecond tick period",
          "[render][gpu][timestamp][issue-290]") {
    // A backend reporting in 0.5 ns ticks: 2 ticks == 1 ns.
    GpuTimestampResolver resolver(0.5);
    REQUIRE(resolver.nanoseconds_per_tick() == 0.5);
    // 2,000,000 ticks * 0.5 ns = 1,000,000 ns = 1.0 ms.
    REQUIRE_THAT(resolver.ticks_to_ms(2'000'000), WithinAbs(1.0, 1e-9));

    // A non-positive period is rejected and falls back to 1.0 ns/tick so
    // a misconfigured backend never produces NaN/inf durations.
    GpuTimestampResolver bad(-3.0);
    REQUIRE(bad.nanoseconds_per_tick() == 1.0);
    GpuTimestampResolver zero(0.0);
    REQUIRE(zero.nanoseconds_per_tick() == 1.0);
}

// ── feature-absent fallback ─────────────────────────────────────────────

TEST_CASE("resolve_pair returns invalid timing when the feature is absent",
          "[render][gpu][timestamp][issue-290]") {
    GpuTimestampResolver resolver;
    REQUIRE_FALSE(resolver.feature_available());

    // Even with a perfectly valid tick pair, an unavailable feature must
    // yield an invalid timing so callers fall back to CPU time.
    GpuPassTiming t = resolver.resolve_pair(1'000, 2'000'000);
    REQUIRE_FALSE(t.valid);
    REQUIRE(t.gpu_time_ms == 0.0);

    // Flip the feature on — the same pair now resolves.
    resolver.set_feature_available(true);
    GpuPassTiming ok = resolver.resolve_pair(1'000, 1'001'000);
    REQUIRE(ok.valid);
    REQUIRE_THAT(ok.gpu_time_ms, WithinAbs(1.0, 1e-9));
}

TEST_CASE("resolve_pair flags non-monotonic tick pairs as a miss",
          "[render][gpu][timestamp][issue-290]") {
    GpuTimestampResolver resolver;
    resolver.set_feature_available(true);

    // end < begin — a query slot was never written, or the GPU clock
    // wrapped. The spec does not guarantee monotonicity across a wrap,
    // so this is treated as an unresolved miss, not a negative duration.
    GpuPassTiming miss = resolver.resolve_pair(5'000'000, 1'000'000);
    REQUIRE_FALSE(miss.valid);
    REQUIRE(miss.gpu_time_ms == 0.0);

    // A zero-length pass (end == begin) IS valid: trivially cheap GPU
    // work legitimately resolves to 0 ms.
    GpuPassTiming zero = resolver.resolve_pair(7'000'000, 7'000'000);
    REQUIRE(zero.valid);
    REQUIRE_THAT(zero.gpu_time_ms, WithinAbs(0.0, 1e-12));
}

// ── interleaved [begin,end,...] buffer resolution ───────────────────────

TEST_CASE("resolve_passes decodes an interleaved ResolveQuerySet buffer",
          "[render][gpu][timestamp][issue-290]") {
    GpuTimestampResolver resolver;
    resolver.set_feature_available(true);

    // Mock exactly what ResolveQuerySet writes: [begin0,end0,begin1,end1].
    // Pass 0: 2.0 ms. Pass 1: 0.5 ms.
    std::vector<uint64_t> ticks = {
        10'000'000, 12'000'000,   // pass 0 -> 2.0 ms
        20'000'000, 20'500'000,   // pass 1 -> 0.5 ms
    };
    auto timings = resolver.resolve_passes(ticks, 2);
    REQUIRE(timings.size() == 2);
    REQUIRE(timings[0].valid);
    REQUIRE_THAT(timings[0].gpu_time_ms, WithinAbs(2.0, 1e-9));
    REQUIRE(timings[1].valid);
    REQUIRE_THAT(timings[1].gpu_time_ms, WithinAbs(0.5, 1e-9));
}

TEST_CASE("resolve_passes leaves a short buffer's tail invalid",
          "[render][gpu][timestamp][issue-290]") {
    GpuTimestampResolver resolver;
    resolver.set_feature_available(true);

    // Only one complete pair, but three passes asked for. Tail entries
    // must stay invalid rather than reading past the buffer.
    std::vector<uint64_t> ticks = {100'000, 1'100'000};  // pass 0 -> 1.0 ms
    auto timings = resolver.resolve_passes(ticks, 3);
    REQUIRE(timings.size() == 3);
    REQUIRE(timings[0].valid);
    REQUIRE_THAT(timings[0].gpu_time_ms, WithinAbs(1.0, 1e-9));
    REQUIRE_FALSE(timings[1].valid);
    REQUIRE_FALSE(timings[2].valid);

    // An odd-length buffer (dangling begin without an end) is also safe.
    std::vector<uint64_t> odd = {100'000, 1'100'000, 5'000'000};
    auto odd_timings = resolver.resolve_passes(odd, 2);
    REQUIRE(odd_timings[0].valid);
    REQUIRE_FALSE(odd_timings[1].valid);
}

// ── RenderPassManager GPU-time integration ──────────────────────────────

TEST_CASE("RenderPassManager carries resolved GPU time alongside CPU time",
          "[render][gpu][timestamp][issue-290]") {
    RenderPassManager rpm;

    // Default: GPU timestamps unavailable until a surface reports support.
    REQUIRE_FALSE(rpm.gpu_timestamps_available());
    rpm.set_gpu_timestamps_available(true);
    REQUIRE(rpm.gpu_timestamps_available());

    rpm.begin_frame();
    rpm.begin_pass(RenderPassType::background);
    rpm.end_pass(/*time_ms=*/3.0f, /*draw_calls=*/2);
    rpm.begin_pass(RenderPassType::content);
    rpm.end_pass(/*time_ms=*/8.0f, /*draw_calls=*/12);

    REQUIRE(rpm.passes().size() == 2);
    // CPU time recorded; GPU time not yet attached -> invalid.
    REQUIRE(rpm.passes()[0].time_ms == 3.0f);
    REQUIRE_FALSE(rpm.passes()[0].gpu_time_valid);

    // The render loop applies the previous frame's resolved GPU timings.
    GpuTimestampResolver resolver;
    resolver.set_feature_available(true);
    std::vector<uint64_t> ticks = {
        0,         2'400'000,    // background -> 2.4 ms GPU
        2'400'000, 9'000'000,    // content    -> 6.6 ms GPU
    };
    auto gpu = resolver.resolve_passes(ticks, 2);
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        rpm.set_pass_gpu_time(i, static_cast<float>(gpu[i].gpu_time_ms),
                              gpu[i].valid);
    }

    REQUIRE(rpm.passes()[0].gpu_time_valid);
    REQUIRE_THAT(rpm.passes()[0].gpu_time_ms, WithinAbs(2.4, 1e-4));
    REQUIRE(rpm.passes()[1].gpu_time_valid);
    REQUIRE_THAT(rpm.passes()[1].gpu_time_ms, WithinAbs(6.6, 1e-4));
    // CPU numbers are untouched — the perf view shows both side by side.
    REQUIRE(rpm.passes()[1].time_ms == 8.0f);

    // An out-of-range index is a safe no-op (no crash, no growth).
    rpm.set_pass_gpu_time(99, 1.0f, true);
    REQUIRE(rpm.passes().size() == 2);
}

TEST_CASE("RenderPassManager leaves GPU time invalid when feature absent",
          "[render][gpu][timestamp][issue-290]") {
    RenderPassManager rpm;
    rpm.begin_frame();
    rpm.begin_pass(RenderPassType::content);
    rpm.end_pass(/*time_ms=*/5.0f, /*draw_calls=*/4);

    // Feature absent — resolver returns an invalid timing; the manager
    // records it as invalid so the inspector reports "GPU unavailable"
    // rather than a misleading 0 ms.
    GpuTimestampResolver resolver;  // feature_available() == false
    GpuPassTiming t = resolver.resolve_pair(1'000, 9'000'000);
    rpm.set_pass_gpu_time(0, static_cast<float>(t.gpu_time_ms), t.valid);

    REQUIRE_FALSE(rpm.passes()[0].gpu_time_valid);
    REQUIRE(rpm.passes()[0].time_ms == 5.0f);  // CPU fallback intact
}
