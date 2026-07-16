#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_playback_cursor.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <vector>

using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::LoopInterpolationMode;
using pulp::audio::LoopPlaybackCursor;
using pulp::audio::LoopPlaybackMode;
using pulp::audio::LoopReader;
using pulp::audio::LoopRenderResult;
using pulp::audio::LoopRegion;
using pulp::audio::LoopRenderer;

namespace {

static_assert(std::is_trivially_copyable_v<LoopPlaybackCursor>);

BufferView<const float> const_view(const Buffer<float>& buffer,
                                   std::vector<const float*>& ptrs) {
    ptrs.resize(buffer.num_channels());
    for (std::size_t ch = 0; ch < buffer.num_channels(); ++ch) {
        ptrs[ch] = buffer.channel(ch).data();
    }
    return {ptrs.data(), buffer.num_channels(), buffer.num_samples()};
}

LoopRegion region(std::uint64_t start, std::uint64_t end) {
    LoopRegion loop;
    loop.start_frame = start;
    loop.end_frame = end;
    loop.source_sample_rate = 48000.0;
    loop.interpolation = LoopInterpolationMode::None;
    return loop;
}

}  // namespace

TEST_CASE("LoopReader interpolates and maps mono sources to extra outputs",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    source.channel(0)[0] = 0.0f;
    source.channel(0)[1] = 10.0f;
    source.channel(0)[2] = 20.0f;
    source.channel(0)[3] = 30.0f;
    std::vector<const float*> ptrs;

    auto loop = region(0, 4);
    loop.interpolation = LoopInterpolationMode::Linear;
    const auto value = LoopReader::read(const_view(source, ptrs), loop, 1, 1.5);
    REQUIRE(std::abs(value - 15.0f) < 1.0e-6f);
}

TEST_CASE("LoopRenderer renders forward and reverse loops",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = static_cast<float>(i);
    }
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    Buffer<float> output(1, 6);
    LoopRenderer renderer;
    auto loop = region(0, 4);
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.start();
    auto result = renderer.render(input, output.view(), output.num_samples());
    REQUIRE(result.rendered_frames == 6);
    REQUIRE(result.wrapped);
    REQUIRE(output.channel(0)[0] == 0.0f);
    REQUIRE(output.channel(0)[3] == 3.0f);
    REQUIRE(output.channel(0)[4] == 0.0f);
    REQUIRE(output.channel(0)[5] == 1.0f);

    // Immediate reverse loop = Reverse shape + reverse ENTRY (Direction=Reverse).
    loop.playback_mode = LoopPlaybackMode::Reverse;
    loop.reverse_entry = true;
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.start();
    result = renderer.render(input, output.view(), 5);
    REQUIRE(result.wrapped);
    REQUIRE(output.channel(0)[0] == 3.0f);
    REQUIRE(output.channel(0)[1] == 2.0f);
    REQUIRE(output.channel(0)[2] == 1.0f);
    REQUIRE(output.channel(0)[3] == 0.0f);
    REQUIRE(output.channel(0)[4] == 3.0f);
}

// Two-phase Direction × Loop: the ENTRY (reverse_entry) governs the first pass,
// the loop SHAPE governs the steady state and overrides the entry at the far edge.
TEST_CASE("LoopRenderer two-phase: forward entry then reverse loop",
          "[audio][loop][render][two-phase]") {
    Buffer<float> source(1, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i)
        source.channel(0)[i] = static_cast<float>(i);
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    Buffer<float> output(1, 8);
    LoopRenderer renderer;
    auto loop = region(0, 4);
    loop.playback_mode = LoopPlaybackMode::Reverse;
    loop.reverse_entry = false;  // Direction=Forward, Loop=Reverse
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.start();
    const auto result = renderer.render(input, output.view(), output.num_samples());
    REQUIRE(result.wrapped);
    // Forward first pass to the top, then loop backward: 0 1 2 3 2 1 0 3.
    const float expected[8] = {0, 1, 2, 3, 2, 1, 0, 3};
    for (std::size_t i = 0; i < 8; ++i) REQUIRE(output.channel(0)[i] == expected[i]);
}

TEST_CASE("LoopRenderer two-phase: reverse entry then forward loop",
          "[audio][loop][render][two-phase]") {
    Buffer<float> source(1, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i)
        source.channel(0)[i] = static_cast<float>(i);
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    Buffer<float> output(1, 8);
    LoopRenderer renderer;
    auto loop = region(0, 4);
    loop.playback_mode = LoopPlaybackMode::Forward;
    loop.reverse_entry = true;  // Direction=Reverse, Loop=Forward
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.start();
    const auto result = renderer.render(input, output.view(), output.num_samples());
    REQUIRE(result.wrapped);
    // Backward first pass to the bottom, then loop forward: 3 2 1 0 1 2 3 0.
    const float expected[8] = {3, 2, 1, 0, 1, 2, 3, 0};
    for (std::size_t i = 0; i < 8; ++i) REQUIRE(output.channel(0)[i] == expected[i]);
}

TEST_CASE("LoopRenderer ping-pong reflects at the loop boundaries",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i)
        source.channel(0)[i] = static_cast<float>(i);
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    Buffer<float> output(1, 8);
    LoopRenderer renderer;
    auto loop = region(0, 4);
    loop.playback_mode = LoopPlaybackMode::PingPong;
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.start();
    const auto result = renderer.render(input, output.view(), output.num_samples());
    REQUIRE(result.rendered_frames == 8);
    REQUIRE(result.wrapped);
    // Forward to the top, reflect, back to the bottom, reflect: 0 1 2 3 2 1 0 1.
    const float expected[8] = {0, 1, 2, 3, 2, 1, 0, 1};
    for (std::size_t i = 0; i < 8; ++i)
        REQUIRE(output.channel(0)[i] == expected[i]);
}

TEST_CASE("LoopRenderer ReverseOnce plays backward once then stops",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i)
        source.channel(0)[i] = static_cast<float>(i);
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    Buffer<float> output(1, 6);
    LoopRenderer renderer;
    auto loop = region(0, 4);
    loop.playback_mode = LoopPlaybackMode::ReverseOnce;
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.start();
    const auto result = renderer.render(input, output.view(), output.num_samples());
    REQUIRE(result.rendered_frames == 6);
    // End -> start (3,2,1,0), then silent (no wrap): 3 2 1 0 0 0.
    REQUIRE(output.channel(0)[0] == 3.0f);
    REQUIRE(output.channel(0)[1] == 2.0f);
    REQUIRE(output.channel(0)[2] == 1.0f);
    REQUIRE(output.channel(0)[3] == 0.0f);
    REQUIRE(output.channel(0)[4] == 0.0f);
    REQUIRE(output.channel(0)[5] == 0.0f);
    REQUIRE(result.silent_frames >= 2);
}

TEST_CASE("LoopRenderer set_playback_mode flips Forward<->OneShot in place",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i)
        source.channel(0)[i] = static_cast<float>(i + 1);  // 1,2,3,4
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    LoopRenderer renderer;
    auto loop = region(0, 4);
    loop.playback_mode = LoopPlaybackMode::Forward;
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.start();

    Buffer<float> out(1, 3);
    auto r1 = renderer.render(input, out.view(), 3);  // 1,2,3 ; position now 3
    REQUIRE_FALSE(r1.wrapped);
    REQUIRE(out.channel(0)[2] == 3.0f);

    // Flip to OneShot WITHOUT set_region: position must be preserved (no restart) and
    // it must stop at the end instead of looping back.
    renderer.set_playback_mode(LoopPlaybackMode::OneShot);
    REQUIRE(renderer.playback_mode() == LoopPlaybackMode::OneShot);
    auto r2 = renderer.render(input, out.view(), 3);
    REQUIRE_FALSE(r2.wrapped);
    REQUIRE(out.channel(0)[0] == 4.0f);  // continued from the preserved position, not restarted at 1
    REQUIRE(out.channel(0)[1] == 0.0f);  // one-shot ended -> silence (did not wrap to the start)
}

TEST_CASE("LoopRenderer renders one-shots and fades deterministically",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = 1.0f;
    }
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);
    Buffer<float> output(1, 6);

    auto loop = region(0, 4);
    loop.playback_mode = LoopPlaybackMode::OneShot;

    LoopRenderer renderer;
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.start();
    auto result = renderer.render(input, output.view(), output.num_samples());
    REQUIRE_FALSE(result.active);
    REQUIRE(result.silent_frames == 2);
    REQUIRE(output.channel(0)[0] == 1.0f);
    REQUIRE(output.channel(0)[3] == 1.0f);
    REQUIRE(output.channel(0)[4] == 0.0f);

    loop.playback_mode = LoopPlaybackMode::Forward;
    REQUIRE(renderer.set_region(loop, source.num_samples()));
    renderer.set_start_fade_frames(3);
    renderer.set_stop_fade_frames(3);
    renderer.start();
    result = renderer.render(input, output.view(), 3);
    REQUIRE(output.channel(0)[0] == 0.0f);
    REQUIRE(std::abs(output.channel(0)[1] - 0.5f) < 1.0e-6f);
    REQUIRE(output.channel(0)[2] == 1.0f);
    REQUIRE(std::abs(renderer.position() - 3.0) < 1.0e-6);

    renderer.stop();
    result = renderer.render(input, output.view(), 3);
    REQUIRE_FALSE(result.active);
    REQUIRE(output.channel(0)[0] == 1.0f);
    REQUIRE(std::abs(output.channel(0)[1] - 0.5f) < 1.0e-6f);
    REQUIRE(output.channel(0)[2] == 0.0f);
}

TEST_CASE("LoopRenderer overwrites destination scratch including silence",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    source.channel(0)[0] = 0.25f;
    source.channel(0)[1] = 0.5f;
    source.channel(0)[2] = 0.75f;
    source.channel(0)[3] = 1.0f;
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    Buffer<float> output(2, 5);
    for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
        std::fill(output.channel(ch).begin(), output.channel(ch).end(), 99.0f);
    }

    auto one_shot = region(0, 2);
    one_shot.playback_mode = LoopPlaybackMode::OneShot;

    LoopRenderer renderer;
    REQUIRE(renderer.set_region(one_shot, source.num_samples()));
    renderer.start();
    auto result = renderer.render(input, output.view(), output.num_samples());
    REQUIRE(result.rendered_frames == output.num_samples());
    REQUIRE_FALSE(result.active);
    REQUIRE(result.silent_frames == 3);
    for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
        REQUIRE(output.channel(ch)[0] == 0.25f);
        REQUIRE(output.channel(ch)[1] == 0.5f);
        REQUIRE(output.channel(ch)[2] == 0.0f);
        REQUIRE(output.channel(ch)[3] == 0.0f);
        REQUIRE(output.channel(ch)[4] == 0.0f);
    }

    for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
        std::fill(output.channel(ch).begin(), output.channel(ch).end(), -7.0f);
    }
    REQUIRE(renderer.set_region(region(0, 4), source.num_samples()));
    renderer.start();
    BufferView<const float> no_source(nullptr, 0, source.num_samples());
    result = renderer.render(no_source, output.view(), 3);
    REQUIRE(result.active);
    REQUIRE(result.rendered_frames == 3);
    REQUIRE(result.silent_frames == 3);
    for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
        REQUIRE(output.channel(ch)[0] == 0.0f);
        REQUIRE(output.channel(ch)[1] == 0.0f);
        REQUIRE(output.channel(ch)[2] == 0.0f);
        REQUIRE(output.channel(ch)[3] == -7.0f);
        REQUIRE(output.channel(ch)[4] == -7.0f);
    }
}

TEST_CASE("LoopRenderer crossfades near wrap boundaries",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    source.channel(0)[0] = 1.0f;
    source.channel(0)[1] = 1.0f;
    source.channel(0)[2] = 1.0f;
    source.channel(0)[3] = -1.0f;
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    Buffer<float> output(1, 4);
    auto loop = region(0, 4);

    LoopRenderer dry;
    REQUIRE(dry.set_region(loop, source.num_samples()));
    dry.start();
    const auto dry_result = dry.render(input, output.view(), 4);
    REQUIRE(dry_result.max_sample_delta >= 2.0f);

    loop.crossfade_frames = 2;
    LoopRenderer faded;
    REQUIRE(faded.set_region(loop, source.num_samples()));
    faded.start();
    const auto faded_result = faded.render(input, output.view(), 4);
    REQUIRE(faded_result.wrapped);
    REQUIRE(faded_result.max_sample_delta < dry_result.max_sample_delta);
}

TEST_CASE("LoopRenderer seam metric compares within each channel",
          "[audio][loop][render]") {
    Buffer<float> source(2, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = 0.25f;
        source.channel(1)[i] = -0.25f;
    }
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);
    Buffer<float> output(2, 4);

    LoopRenderer renderer;
    REQUIRE(renderer.set_region(region(0, 4), source.num_samples()));
    renderer.start();
    const auto result = renderer.render(input, output.view(), output.num_samples());

    REQUIRE(result.rendered_frames == output.num_samples());
    REQUIRE(result.max_sample_delta == 0.0f);
}

TEST_CASE("LoopRenderer handles high playback rates across wrap boundaries",
          "[audio][loop][render]") {
    Buffer<float> source(1, 8);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = static_cast<float>(i);
    }
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);
    Buffer<float> output(1, 5);

    LoopRenderer renderer;
    REQUIRE(renderer.set_region(region(0, 8), source.num_samples()));
    renderer.set_playback_rate(2.5);
    renderer.start();
    const auto result = renderer.render(input, output.view(), output.num_samples());

    REQUIRE(result.rendered_frames == output.num_samples());
    REQUIRE(result.wrapped);
    REQUIRE(output.channel(0)[0] == 0.0f);
    REQUIRE(output.channel(0)[1] == 2.0f);
    REQUIRE(output.channel(0)[2] == 5.0f);
    REQUIRE(output.channel(0)[3] == 7.0f);
}

TEST_CASE("LoopRenderer applies crossfade when high playback rates skip the fade zone",
          "[audio][loop][render]") {
    Buffer<float> source(1, 8);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = static_cast<float>(i);
    }
    source.channel(0)[7] = -8.0f;
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);
    Buffer<float> output(1, 4);

    auto dry_loop = region(0, 8);
    LoopRenderer dry;
    REQUIRE(dry.set_region(dry_loop, source.num_samples()));
    dry.set_playback_rate(4.0);
    dry.start();
    const auto dry_result = dry.render(input, output.view(), output.num_samples());
    REQUIRE(dry_result.wrapped);

    auto faded_loop = region(0, 8);
    faded_loop.crossfade_frames = 2;
    LoopRenderer faded;
    REQUIRE(faded.set_region(faded_loop, source.num_samples()));
    faded.set_playback_rate(4.0);
    faded.start();
    const auto faded_result = faded.render(input, output.view(), output.num_samples());

    REQUIRE(faded_result.wrapped);
    REQUIRE(faded_result.max_sample_delta < dry_result.max_sample_delta);
}

TEST_CASE("LoopRenderer zero-channel sources do not advance playback",
          "[audio][loop][render]") {
    Buffer<float> source(1, 4);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = static_cast<float>(i);
    }
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);

    Buffer<float> output(1, 4);
    LoopRenderer renderer;
    REQUIRE(renderer.set_region(region(0, 4), source.num_samples()));
    renderer.start();

    BufferView<const float> no_source(nullptr, 0, source.num_samples());
    auto result = renderer.render(no_source, output.view(), 3);
    REQUIRE(result.active);
    REQUIRE(result.rendered_frames == 3);
    REQUIRE(result.silent_frames == 3);
    REQUIRE_FALSE(result.wrapped);
    REQUIRE(renderer.position() == 0.0);
    REQUIRE(output.channel(0)[0] == 0.0f);
    REQUIRE(output.channel(0)[2] == 0.0f);

    result = renderer.render(input, output.view(), 2);
    REQUIRE(result.rendered_frames == 2);
    REQUIRE(output.channel(0)[0] == 0.0f);
    REQUIRE(output.channel(0)[1] == 1.0f);
    REQUIRE(renderer.position() == 2.0);
}

TEST_CASE("LoopRenderer hot path runs under no-allocation guard",
          "[audio][loop][render][rt]") {
    Buffer<float> source(1, 8);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = static_cast<float>(i);
    }
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);
    Buffer<float> output(1, 16);

    LoopRenderer renderer;
    REQUIRE(renderer.set_region(region(0, 8), source.num_samples()));
    renderer.start();
    LoopRenderResult result;
    {
        pulp::runtime::ScopedNoAlloc guard;
        result = renderer.render(input, output.view(), output.num_samples());
    }
    REQUIRE(result.rendered_frames == output.num_samples());
}

TEST_CASE("LoopPlaybackCursor matches LoopRenderer traversal coordinates",
          "[audio][loop][cursor]") {
    Buffer<float> source(1, 8);
    for (std::size_t i = 0; i < source.num_samples(); ++i) {
        source.channel(0)[i] = static_cast<float>(i + 1);
    }
    std::vector<const float*> ptrs;
    const auto input = const_view(source, ptrs);
    Buffer<float> output(1, 1);

    struct Scenario {
        LoopPlaybackMode mode;
        bool reverse_entry;
        double rate;
    };
    const Scenario scenarios[] = {
        {LoopPlaybackMode::OneShot, false, 1.0},
        {LoopPlaybackMode::ReverseOnce, false, 1.0},
        {LoopPlaybackMode::Forward, false, 2.5},
        {LoopPlaybackMode::Forward, true, 1.0},
        {LoopPlaybackMode::Reverse, false, 1.0},
        {LoopPlaybackMode::Reverse, true, 2.5},
        {LoopPlaybackMode::PingPong, false, 2.5},
        {LoopPlaybackMode::PingPong, true, 1.0},
    };

    for (const auto& scenario : scenarios) {
        auto loop = region(1, 7);
        loop.playback_mode = scenario.mode;
        loop.reverse_entry = scenario.reverse_entry;

        LoopPlaybackCursor cursor;
        LoopRenderer renderer;
        REQUIRE(cursor.set_region(loop, source.num_samples()));
        REQUIRE(renderer.set_region(loop, source.num_samples()));
        cursor.set_playback_rate(scenario.rate);
        renderer.set_playback_rate(scenario.rate);
        cursor.start();
        renderer.start();

        for (std::uint32_t frame = 0; frame < 12; ++frame) {
            REQUIRE(renderer.position() == cursor.position());
            REQUIRE(renderer.active() == cursor.active());

            const auto plan = cursor.frame_read_plan();
            const auto expected = cursor.active()
                ? LoopReader::read_validated(input, loop, 0, plan.read_position)
                : 0.0f;
            const auto advanced = cursor.active()
                ? cursor.advance()
                : pulp::audio::LoopPlaybackAdvanceResult{};
            const auto rendered = renderer.render(input, output.view(), 1);

            REQUIRE(output.channel(0)[0] == expected);
            REQUIRE(rendered.wrapped == (plan.wrapped || advanced.wrapped));
            REQUIRE(renderer.position() == cursor.position());
            REQUIRE(renderer.active() == cursor.active());
        }
    }
}

TEST_CASE("LoopPlaybackCursor exposes wrap crossfade read coordinates",
          "[audio][loop][cursor]") {
    auto forward = region(10, 20);
    forward.crossfade_frames = 4;

    LoopPlaybackCursor cursor;
    REQUIRE(cursor.set_region(forward, 32));
    cursor.set_playback_rate(4.0);
    cursor.start();
    for (int i = 0; i < 4; ++i) cursor.advance();

    const auto forward_plan = cursor.frame_read_plan();
    REQUIRE(forward_plan.blend);
    REQUIRE(forward_plan.wrapped);
    REQUIRE(forward_plan.read_position == 16.0);
    REQUIRE(forward_plan.blend_position == 10.0);
    REQUIRE(forward_plan.primary_gain == 1.0);
    REQUIRE(forward_plan.blend_gain == 0.0);

    auto reverse = forward;
    reverse.playback_mode = LoopPlaybackMode::Reverse;
    reverse.reverse_entry = true;
    REQUIRE(cursor.set_region(reverse, 32));
    cursor.set_playback_rate(4.0);
    cursor.start();
    cursor.advance();

    const auto reverse_plan = cursor.frame_read_plan();
    REQUIRE(reverse_plan.blend);
    REQUIRE(reverse_plan.wrapped);
    REQUIRE(reverse_plan.read_position == 15.0);
    REQUIRE(reverse_plan.blend_position == 17.0);
    REQUIRE(reverse_plan.primary_gain == 0.25);
    REQUIRE(reverse_plan.blend_gain == 0.75);
}

TEST_CASE("LoopPlaybackCursor folds very large ping-pong steps",
          "[audio][loop][cursor]") {
    auto loop = region(0, 4);
    loop.playback_mode = LoopPlaybackMode::PingPong;

    LoopPlaybackCursor forward;
    REQUIRE(forward.set_region(loop, 4));
    forward.set_playback_rate(10.0);
    forward.start();
    const double forward_positions[] = {0.0, 2.0, 2.0, 0.0, 2.0, 2.0};
    for (const auto expected : forward_positions) {
        REQUIRE(forward.position() == expected);
        REQUIRE(forward.advance().wrapped);
    }

    loop.reverse_entry = true;
    LoopPlaybackCursor reverse;
    REQUIRE(reverse.set_region(loop, 4));
    reverse.set_playback_rate(10.0);
    reverse.start();
    const double reverse_positions[] = {3.0, 1.0, 1.0, 3.0, 1.0, 1.0};
    for (const auto expected : reverse_positions) {
        REQUIRE(reverse.position() == expected);
        REQUIRE(reverse.advance().wrapped);
    }
}

TEST_CASE("LoopPlaybackCursor preserves direction at folded ping-pong endpoints",
          "[audio][loop][cursor]") {
    auto loop = region(0, 4);
    loop.playback_mode = LoopPlaybackMode::PingPong;

    LoopPlaybackCursor cursor;
    REQUIRE(cursor.set_region(loop, 4));
    cursor.set_playback_rate(6.0);
    cursor.start();
    REQUIRE(cursor.advance().wrapped);
    REQUIRE(cursor.position() == 0.0);
    REQUIRE(cursor.step() == 6.0);

    REQUIRE(cursor.set_region(loop, 4));
    cursor.set_playback_rate(9.0);
    cursor.start();
    REQUIRE(cursor.advance().wrapped);
    REQUIRE(cursor.position() == 3.0);
    REQUIRE(cursor.step() == 9.0);

    REQUIRE(cursor.set_region(loop, 4));
    cursor.set_playback_rate(3.0);
    cursor.start();
    REQUIRE_FALSE(cursor.advance().wrapped);
    REQUIRE(cursor.position() == 3.0);
    REQUIRE(cursor.step() == 3.0);
}

TEST_CASE("LoopPlaybackCursor carries residual distance after entry turns",
          "[audio][loop][cursor]") {
    auto reverse_loop = region(0, 4);
    reverse_loop.playback_mode = LoopPlaybackMode::Reverse;

    LoopPlaybackCursor cursor;
    REQUIRE(cursor.set_region(reverse_loop, 4));
    cursor.set_playback_rate(10.0);
    cursor.start();
    const double reverse_positions[] = {0.0, 0.0, 2.0, 0.0, 2.0};
    for (const auto expected : reverse_positions) {
        REQUIRE(cursor.position() == expected);
        REQUIRE(cursor.advance().wrapped);
    }

    auto forward_loop = region(0, 4);
    forward_loop.playback_mode = LoopPlaybackMode::Forward;
    forward_loop.reverse_entry = true;
    REQUIRE(cursor.set_region(forward_loop, 4));
    cursor.set_playback_rate(10.0);
    cursor.start();
    const double forward_positions[] = {3.0, 3.0, 1.0, 3.0, 1.0};
    for (const auto expected : forward_positions) {
        REQUIRE(cursor.position() == expected);
        REQUIRE(cursor.advance().wrapped);
    }
}

TEST_CASE("LoopPlaybackCursor normalizes very large steady loop steps",
          "[audio][loop][cursor]") {
    auto loop = region(0, 4);
    loop.crossfade_frames = 2;

    LoopPlaybackCursor cursor;
    REQUIRE(cursor.set_region(loop, 4));
    cursor.set_playback_rate(1001.0);
    cursor.start();
    const double positions[] = {0.0, 1.0, 2.0, 3.0, 0.0};
    for (const auto expected : positions) {
        REQUIRE(cursor.position() == expected);
        REQUIRE(cursor.frame_read_plan().blend);
        REQUIRE(cursor.advance().wrapped);
    }
}

TEST_CASE("LoopPlaybackCursor preserves signed rates and rejects invalid updates",
          "[audio][loop][cursor]") {
    LoopPlaybackCursor cursor;
    REQUIRE(cursor.set_region(region(0, 4), 4));
    cursor.set_playback_rate(2.5);
    cursor.start();
    REQUIRE(cursor.step() == 2.5);

    cursor.set_playback_rate(0.0);
    REQUIRE(cursor.step() == 2.5);
    cursor.set_playback_rate(-3.0);
    REQUIRE(cursor.step() == -3.0);
    cursor.set_playback_rate(std::numeric_limits<double>::infinity());
    REQUIRE(cursor.step() == -3.0);
    cursor.set_playback_rate(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(cursor.step() == -3.0);
}

TEST_CASE("LoopPlaybackCursor traversal is allocation-free",
          "[audio][loop][cursor][rt]") {
    auto loop = region(0, 8);
    loop.crossfade_frames = 2;
    loop.playback_mode = LoopPlaybackMode::PingPong;

    LoopPlaybackCursor cursor;
    REQUIRE(cursor.set_region(loop, 8));
    cursor.set_playback_rate(2.5);
    cursor.start();
    {
        pulp::runtime::ScopedNoAlloc guard;
        for (int frame = 0; frame < 64; ++frame) {
            (void) cursor.frame_read_plan();
            (void) cursor.advance();
        }
    }
    REQUIRE(cursor.active());
}
