// PF-2 null test for the LoopRenderer wrap-crossfade refactor.
//
// The refactor hoists the equal-power cos/sin out of the per-channel loop:
// instead of recomputing the blend gains for every channel, render() now builds
// one per-frame CrossfadePlan and every channel reuses it. This test proves the
// change is bit-exact by driving the REAL renderer frame-by-frame and comparing
// every output sample against an oracle that reproduces the ORIGINAL
// per-channel sample_with_crossfade()/blend() math verbatim.
//
// A plain Forward loop (reverse_entry=false, playback_rate=1) keeps step_dir_
// pinned to the loop direction and step==1 for the whole render, so the oracle's
// branch selection matches the renderer's exactly.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::LoopCrossfadeCurve;
using pulp::audio::LoopInterpolationMode;
using pulp::audio::LoopPlaybackMode;
using pulp::audio::LoopReader;
using pulp::audio::LoopRegion;
using pulp::audio::LoopRenderer;

namespace {

constexpr double kPi = 3.14159265358979323846;

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch)
        ptrs[ch] = buffer.channel(ch).data();
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

// --- Verbatim copy of the ORIGINAL (pre-refactor) blend + sample_with_crossfade
// math, specialized to a Forward loop (step_dir == loop_dir == 1). This is the
// reference the refactored renderer must reproduce bit-for-bit.
float ref_blend(float a, float b, double t, LoopCrossfadeCurve curve) {
    t = std::clamp(t, 0.0, 1.0);
    if (curve == LoopCrossfadeCurve::EqualPower) {
        const auto dry = std::cos(t * 0.5 * kPi);
        const auto wet = std::sin(t * 0.5 * kPi);
        return static_cast<float>(static_cast<double>(a) * dry +
                                  static_cast<double>(b) * wet);
    }
    return static_cast<float>(static_cast<double>(a) * (1.0 - t) +
                              static_cast<double>(b) * t);
}

float ref_sample(BufferView<const float> source, const LoopRegion& region,
                 std::uint32_t ch, double position, double step) {
    const auto crossfade = static_cast<double>(region.crossfade_frames);
    const auto start = static_cast<double>(region.start_frame);
    const auto end = static_cast<double>(region.end_frame);
    const auto normalized = LoopReader::normalize_position(region, position);
    auto read = [&](double p) {
        return LoopReader::read_validated(source, region, ch, p);
    };
    if (step >= 0.0 && normalized >= end - crossfade) {
        const auto t = (normalized - (end - crossfade)) / crossfade;
        return ref_blend(read(normalized), read(start + (normalized - (end - crossfade))),
                         t, region.crossfade_curve);
    }
    if (step > 0.0 && normalized < end - crossfade &&
        normalized + step >= end - crossfade) {
        const auto probe = std::min(normalized + step, end);
        const auto t = (probe - (end - crossfade)) / crossfade;
        return ref_blend(read(normalized), read(start + (probe - (end - crossfade))),
                         t, region.crossfade_curve);
    }
    return read(normalized);
}

}  // namespace

TEST_CASE("PF-2: LoopRenderer wrap-crossfade is bit-exact after the plan hoist",
          "[audio][loop][render][pf2][null]") {
    constexpr std::size_t kChannels = 2;
    constexpr std::size_t kSourceFrames = 1100;
    constexpr std::uint64_t kStart = 100;
    constexpr std::uint64_t kEnd = 1000;
    constexpr int kFrames = 1000;

    // Distinct, deterministic content per channel so the blend result differs
    // between channels (the per-channel apply path is genuinely exercised).
    Buffer<float> source(kChannels, kSourceFrames);
    for (std::size_t i = 0; i < kSourceFrames; ++i) {
        source.channel(0)[i] =
            std::sin(0.017f * static_cast<float>(i)) * 0.9f;
        source.channel(1)[i] =
            std::cos(0.023f * static_cast<float>(i) + 0.5f) * 0.7f;
    }
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    for (LoopCrossfadeCurve curve :
         {LoopCrossfadeCurve::EqualPower, LoopCrossfadeCurve::Linear}) {
        LoopRegion region;
        region.start_frame = kStart;
        region.end_frame = kEnd;
        region.crossfade_frames = 64;
        region.crossfade_curve = curve;
        region.playback_mode = LoopPlaybackMode::Forward;
        region.reverse_entry = false;
        region.interpolation = LoopInterpolationMode::None;
        region.source_sample_rate = 48000.0;

        LoopRenderer renderer;
        REQUIRE(renderer.set_region(region, kSourceFrames));
        renderer.set_playback_rate(1.0);
        renderer.start();

        Buffer<float> frame_out(kChannels, 1);
        bool crossfade_fired = false;
        for (int f = 0; f < kFrames; ++f) {
            const double pos_before = renderer.position();
            const auto result = renderer.render(input, frame_out.view(), 1);
            REQUIRE(result.rendered_frames == 1);

            for (std::uint32_t ch = 0; ch < kChannels; ++ch) {
                const float expected = ref_sample(input, region, ch, pos_before, 1.0);
                REQUIRE(frame_out.channel(ch)[0] == expected);  // bit-exact
            }
            const double norm = LoopReader::normalize_position(region, pos_before);
            if (norm >= static_cast<double>(kEnd) -
                            static_cast<double>(region.crossfade_frames))
                crossfade_fired = true;
        }
        // Guard against a vacuous pass: the crossfade region must actually be hit.
        REQUIRE(crossfade_fired);
    }
}
