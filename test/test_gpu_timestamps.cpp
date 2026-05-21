// Phase 6.5 — Dawn GPU timestamp queries.
//
// These tests cover the *pure* resolution layer of `gpu_timestamps.hpp`:
// the nanosecond-tick -> millisecond conversion, the `decode_resolved_
// ticks` mapped-buffer byte decode, the resolved-buffer -> per-pass walk,
// and the RenderPassManager integration. They run without a live GPU
// device by feeding synthetic resolved-buffer bytes/values, so they
// execute on every CI lane (including the no-GPU sanitizer matrix).
//
// `decode_resolved_ticks` is the seam `GpuTimestamps::resolve()` uses to
// turn a mapped Dawn readback buffer into ticks — covering it here is
// what proves `read_back()` surfaces populated, correctly-converted
// numbers rather than the empty vector the Phase 6.5 P1 review flagged.
//
// Live-device smoke (a real `wgpu::QuerySet` resolved against Metal/
// Vulkan via `resolve()`) is deferred to CI's GPU matrix; the math and
// decode asserted here are the parts that historically hide perf bugs.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/render/gpu_timestamps.hpp>
#include <pulp/render/render_pass.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace pulp::render;

namespace {

// Pack a vector of ticks into the little-endian uint64 byte layout that
// `wgpu::Buffer::GetConstMappedRange` hands back after a real
// `ResolveQuerySet` + copy. This is the synthetic stand-in for a mapped
// Dawn buffer — feeding it through `decode_resolved_ticks` exercises the
// exact decode path `GpuTimestamps::resolve()` uses on a live device.
std::vector<std::byte> pack_ticks(const std::vector<std::uint64_t>& ticks) {
    std::vector<std::byte> bytes(ticks.size() * sizeof(std::uint64_t));
    if (!ticks.empty()) {
        std::memcpy(bytes.data(), ticks.data(), bytes.size());
    }
    return bytes;
}

}  // namespace

// ── timestamp_pair_to_ms — nanosecond conversion ────────────────────────────

TEST_CASE("timestamp_pair_to_ms converts a tick delta to ms",
          "[render][gpu-timestamps]") {
    // 1,000,000 ns = exactly 1 ms.
    auto ms = timestamp_pair_to_ms(1'000, 1'000 + 1'000'000);
    REQUIRE(ms.has_value());
    REQUIRE(*ms == Catch::Approx(1.0));
}

TEST_CASE("timestamp_pair_to_ms handles sub-millisecond and large deltas",
          "[render][gpu-timestamps]") {
    // 250,000 ns = 0.25 ms — a cheap pass.
    auto quarter = timestamp_pair_to_ms(10, 10 + 250'000);
    REQUIRE(quarter.has_value());
    REQUIRE(*quarter == Catch::Approx(0.25));

    // 33,400,000 ns ~= 33.4 ms — a frame well over a 60fps budget.
    auto slow = timestamp_pair_to_ms(1, 1 + 33'400'000);
    REQUIRE(slow.has_value());
    REQUIRE(*slow == Catch::Approx(33.4));
}

TEST_CASE("timestamp_pair_to_ms rejects a zero tick (unwritten slot)",
          "[render][gpu-timestamps]") {
    // Dawn zero-fills query slots that were never written. A zero in
    // either position means the pass produced no real sample.
    REQUIRE_FALSE(timestamp_pair_to_ms(0, 1'000'000).has_value());
    REQUIRE_FALSE(timestamp_pair_to_ms(1'000'000, 0).has_value());
    REQUIRE_FALSE(timestamp_pair_to_ms(0, 0).has_value());
}

TEST_CASE("timestamp_pair_to_ms rejects a backwards pair (end < begin)",
          "[render][gpu-timestamps]") {
    // A counter wrap or a half-written pair can surface end < begin.
    // That is never a real duration — fall back to CPU time.
    REQUIRE_FALSE(timestamp_pair_to_ms(5'000'000, 4'000'000).has_value());
}

TEST_CASE("timestamp_pair_to_ms treats an equal pair as zero duration",
          "[render][gpu-timestamps]") {
    auto ms = timestamp_pair_to_ms(7'777, 7'777);
    REQUIRE(ms.has_value());
    REQUIRE(*ms == Catch::Approx(0.0));
}

// ── decode_resolved_ticks — mapped-buffer byte decode ───────────────────────
//
// This is the seam the Phase 6.5 P1 review flagged: before this layer
// existed, `GpuTimestamps::read_back()` returned an internal vector that
// nothing ever populated, so GPU timings never surfaced even when the
// device supported `timestamp-query`. `resolve()` now decodes the
// mapped readback buffer through `decode_resolved_ticks`; these cases
// pin that decode against the byte layout a real mapped buffer carries.

TEST_CASE("decode_resolved_ticks decodes a mapped timestamp buffer",
          "[render][gpu-timestamps]") {
    // The bytes a real resolved + map-read buffer would hold for a
    // two-pass frame: [begin0, end0, begin1, end1].
    const std::vector<std::uint64_t> ticks = {
        1'000, 1'000 + 2'000'000,
        9'000, 9'000 + 500'000,
    };
    const auto bytes = pack_ticks(ticks);

    const auto decoded = decode_resolved_ticks(bytes.data(), bytes.size());
    REQUIRE(decoded == ticks);

    // The decoded ticks feed straight into the existing per-pass walk —
    // proving the resolve path produces a populated, usable result.
    const auto timings = resolve_pass_timings(decoded, 2);
    REQUIRE(timings.size() == 2);
    REQUIRE(timings[0].valid);
    REQUIRE(timings[0].gpu_time_ms == Catch::Approx(2.0));
    REQUIRE(timings[1].valid);
    REQUIRE(timings[1].gpu_time_ms == Catch::Approx(0.5));
}

TEST_CASE("decode_resolved_ticks round-trips large 64-bit tick values",
          "[render][gpu-timestamps]") {
    // GPU clocks are wide — make sure no value is truncated to 32 bits.
    const std::vector<std::uint64_t> ticks = {
        0x0000'0001'0000'0000ULL,             // 2^32 — would vanish if truncated
        0xFFFF'FFFF'FFFF'FFFFULL,             // max uint64
        0x0123'4567'89AB'CDEFULL,
    };
    const auto bytes = pack_ticks(ticks);
    const auto decoded = decode_resolved_ticks(bytes.data(), bytes.size());
    REQUIRE(decoded == ticks);
}

TEST_CASE("decode_resolved_ticks returns empty for a null or short buffer",
          "[render][gpu-timestamps]") {
    // A device that never mapped the buffer hands back null / nothing —
    // the decode must yield an empty vector, never read out of bounds.
    REQUIRE(decode_resolved_ticks(nullptr, 64).empty());
    REQUIRE(decode_resolved_ticks(nullptr, 0).empty());

    const std::vector<std::uint64_t> one = {42};
    const auto bytes = pack_ticks(one);
    REQUIRE(decode_resolved_ticks(bytes.data(), 0).empty());
    // Fewer than 8 bytes — no whole uint64 to decode.
    REQUIRE(decode_resolved_ticks(bytes.data(), 7).empty());
}

TEST_CASE("decode_resolved_ticks truncates a partial trailing tick",
          "[render][gpu-timestamps]") {
    // A short or partial readback (e.g. only 1.5 uint64s landed) decodes
    // to the last *whole* tick; resolve_pass_timings then tolerates the
    // truncated tail by marking missing passes invalid.
    const std::vector<std::uint64_t> ticks = {111, 222};
    const auto bytes = pack_ticks(ticks);
    // 12 bytes = one whole uint64 + 4 trailing bytes.
    const auto decoded = decode_resolved_ticks(bytes.data(), 12);
    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0] == 111);
}

// ── resolve_pass_timings — resolved-buffer walk ─────────────────────────────

TEST_CASE("resolve_pass_timings walks a full resolved buffer",
          "[render][gpu-timestamps]") {
    // Layout is [begin0, end0, begin1, end1, ...]. Two passes:
    //   pass 0 -> 2.0 ms, pass 1 -> 0.5 ms.
    std::vector<std::uint64_t> resolved = {
        1'000, 1'000 + 2'000'000,           // pass 0: 2.0 ms
        9'000, 9'000 + 500'000,             // pass 1: 0.5 ms
    };
    auto timings = resolve_pass_timings(resolved, 2);
    REQUIRE(timings.size() == 2);

    REQUIRE(timings[0].pass_index == 0);
    REQUIRE(timings[0].valid);
    REQUIRE(timings[0].gpu_time_ms == Catch::Approx(2.0));

    REQUIRE(timings[1].pass_index == 1);
    REQUIRE(timings[1].valid);
    REQUIRE(timings[1].gpu_time_ms == Catch::Approx(0.5));
}

TEST_CASE("resolve_pass_timings marks a bad pair invalid but keeps the slot",
          "[render][gpu-timestamps]") {
    // Pass 0 is fine; pass 1 has an unwritten (zero) end slot.
    std::vector<std::uint64_t> resolved = {
        1'000, 1'000 + 1'000'000,           // pass 0: 1.0 ms
        9'000, 0,                           // pass 1: unwritten end
    };
    auto timings = resolve_pass_timings(resolved, 2);
    REQUIRE(timings.size() == 2);
    REQUIRE(timings[0].valid);
    REQUIRE(timings[0].gpu_time_ms == Catch::Approx(1.0));

    // The invalid pass is still present at its index — it just carries
    // no sample, so the inspector can show "unavailable" for that row.
    REQUIRE_FALSE(timings[1].valid);
    REQUIRE(timings[1].pass_index == 1);
    REQUIRE(timings[1].gpu_time_ms == Catch::Approx(0.0));
}

TEST_CASE("resolve_pass_timings tolerates a truncated readback",
          "[render][gpu-timestamps]") {
    // The buffer holds only pass 0; pass 1's timestamps have not landed.
    std::vector<std::uint64_t> resolved = {
        1'000, 1'000 + 4'000'000,           // pass 0: 4.0 ms
    };
    auto timings = resolve_pass_timings(resolved, 2);
    REQUIRE(timings.size() == 2);
    REQUIRE(timings[0].valid);
    REQUIRE(timings[0].gpu_time_ms == Catch::Approx(4.0));
    // Missing tail pass: returned, but invalid.
    REQUIRE_FALSE(timings[1].valid);
}

TEST_CASE("resolve_pass_timings on an empty buffer returns invalid passes",
          "[render][gpu-timestamps]") {
    auto timings = resolve_pass_timings({}, 3);
    REQUIRE(timings.size() == 3);
    for (const auto& t : timings) {
        REQUIRE_FALSE(t.valid);
    }
}

TEST_CASE("resolve_pass_timings with zero passes returns an empty vector",
          "[render][gpu-timestamps]") {
    auto timings = resolve_pass_timings({1, 2, 3, 4}, 0);
    REQUIRE(timings.empty());
}

// ── apply_pass_timings — RenderPassManager integration ──────────────────────

TEST_CASE("apply_pass_timings feeds GPU durations into RenderPassManager",
          "[render][gpu-timestamps]") {
    RenderPassManager rpm;
    rpm.begin_frame();
    rpm.begin_pass(RenderPassType::background);
    rpm.end_pass(/*cpu*/ 1.5f, /*draw_calls*/ 4);
    rpm.begin_pass(RenderPassType::content);
    rpm.end_pass(/*cpu*/ 3.0f, /*draw_calls*/ 12);
    rpm.end_frame();

    // Before resolution, no pass carries GPU timing.
    REQUIRE_FALSE(rpm.has_gpu_timing());
    for (const auto& p : rpm.passes()) {
        REQUIRE_FALSE(p.gpu_time_valid);
    }

    // Synthetic resolved buffer: pass 0 -> 0.8 ms GPU, pass 1 -> 5.2 ms GPU.
    std::vector<std::uint64_t> resolved = {
        100, 100 + 800'000,                 // pass 0: 0.8 ms
        700, 700 + 5'200'000,               // pass 1: 5.2 ms
    };
    apply_pass_timings(rpm, resolve_pass_timings(resolved, rpm.passes().size()));

    REQUIRE(rpm.has_gpu_timing());
    const auto& passes = rpm.passes();
    REQUIRE(passes.size() == 2);

    // CPU numbers are untouched; GPU numbers are now populated.
    REQUIRE(passes[0].cpu_time_ms() == Catch::Approx(1.5f));
    REQUIRE(passes[0].gpu_time_valid);
    REQUIRE(passes[0].gpu_time_ms == Catch::Approx(0.8f));

    REQUIRE(passes[1].cpu_time_ms() == Catch::Approx(3.0f));
    REQUIRE(passes[1].gpu_time_valid);
    REQUIRE(passes[1].gpu_time_ms == Catch::Approx(5.2f));
}

TEST_CASE("apply_pass_timings skips invalid timings (CPU-only fallback)",
          "[render][gpu-timestamps]") {
    RenderPassManager rpm;
    rpm.begin_frame();
    rpm.begin_pass(RenderPassType::content);
    rpm.end_pass(2.0f, 8);
    rpm.end_frame();

    // The resolved pair is garbage (end < begin) -> invalid timing.
    std::vector<std::uint64_t> resolved = {9'000'000, 1'000'000};
    apply_pass_timings(rpm, resolve_pass_timings(resolved, 1));

    // The pass keeps gpu_time_valid == false; the inspector shows CPU
    // time and an honest "GPU timestamps unavailable" for this pass.
    REQUIRE_FALSE(rpm.has_gpu_timing());
    REQUIRE_FALSE(rpm.passes()[0].gpu_time_valid);
    REQUIRE(rpm.passes()[0].cpu_time_ms() == Catch::Approx(2.0f));
}

TEST_CASE("set_pass_gpu_time ignores out-of-range pass indices",
          "[render][gpu-timestamps]") {
    // Timestamps lag one frame; the pass list may have shrunk since.
    RenderPassManager rpm;
    rpm.begin_frame();
    rpm.begin_pass(RenderPassType::content);
    rpm.end_pass(1.0f, 1);
    rpm.end_frame();

    rpm.set_pass_gpu_time(0, 0.5f);   // in range
    rpm.set_pass_gpu_time(99, 7.0f);  // stale index from a larger frame

    REQUIRE(rpm.passes().size() == 1);
    REQUIRE(rpm.passes()[0].gpu_time_valid);
    REQUIRE(rpm.passes()[0].gpu_time_ms == Catch::Approx(0.5f));
}

TEST_CASE("set_pass_gpu_time rejects a negative duration",
          "[render][gpu-timestamps]") {
    RenderPassManager rpm;
    rpm.begin_frame();
    rpm.begin_pass(RenderPassType::content);
    rpm.end_pass(1.0f, 1);
    rpm.end_frame();

    rpm.set_pass_gpu_time(0, -3.0f);
    REQUIRE_FALSE(rpm.passes()[0].gpu_time_valid);
}

// ── GpuTimestampSupport — feature-availability state machine ─────────────────

TEST_CASE("GpuTimestampSupport describe/is_usable cover every state",
          "[render][gpu-timestamps]") {
    REQUIRE_FALSE(is_usable(GpuTimestampSupport::unknown));
    REQUIRE_FALSE(is_usable(GpuTimestampSupport::unsupported));
    REQUIRE(is_usable(GpuTimestampSupport::supported));

    // The "unsupported" label is the exact string the inspector shows
    // when the adapter lacks the timestamp-query feature.
    REQUIRE(std::string(describe(GpuTimestampSupport::unsupported))
            == "GPU timestamps unavailable");
    REQUIRE(std::string(describe(GpuTimestampSupport::supported))
            == "GPU timestamps active");
    REQUIRE(std::string(describe(GpuTimestampSupport::unknown))
            == "GPU timestamps: not probed");
}

// ── GpuTimestamps — graceful degradation when the device is absent ──────────

TEST_CASE("GpuTimestamps degrades gracefully with no device",
          "[render][gpu-timestamps]") {
    // initialize(nullptr) must never crash — it reports unsupported and
    // every subsequent call is a safe no-op. This is also the exact
    // behavior of the CPU-only build (no Dawn linked at all).
    GpuTimestamps ts;
    REQUIRE(ts.support() == GpuTimestampSupport::unknown);

    const bool ok = ts.initialize(nullptr);
    REQUIRE_FALSE(ok);
    REQUIRE(ts.support() == GpuTimestampSupport::unsupported);
    REQUIRE_FALSE(ts.available());

    // No-ops, no crash.
    ts.begin_frame(4);
    REQUIRE(ts.read_back().empty());

    // resolve() with no device (and a null instance) must report false
    // and never crash — the same safe path the CPU-only build takes.
    REQUIRE_FALSE(ts.resolve(nullptr));
    REQUIRE(ts.read_back().empty());
}

TEST_CASE("kTimestampsPerPass is two (begin + end)",
          "[render][gpu-timestamps]") {
    REQUIRE(kTimestampsPerPass == 2);
}
