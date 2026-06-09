#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/voice_sum_mixer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using Catch::Matchers::WithinAbs;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::VoiceSumInput;
using pulp::audio::VoiceSumMixer;
using pulp::audio::VoiceSumOptions;

namespace {

template<std::size_t ChannelCount>
BufferView<const float> const_view(std::array<const float*, ChannelCount>& ptrs,
                                   std::size_t frames) {
    return {ptrs.data(), ChannelCount, frames};
}

void fill(Buffer<float>& buffer, float value) {
    for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
        std::fill(buffer.channel(channel).begin(), buffer.channel(channel).end(), value);
    }
}

}  // namespace

TEST_CASE("VoiceSumMixer clears and sums multiple mono voices",
          "[audio][sampler][voice-sum]") {
    std::array<float, 4> voice_a{1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> voice_b{10.0f, 10.0f, 10.0f, 10.0f};
    std::array<const float*, 1> ptrs_a{voice_a.data()};
    std::array<const float*, 1> ptrs_b{voice_b.data()};

    Buffer<float> destination(2, 4);
    fill(destination, 99.0f);

    std::array<VoiceSumInput, 2> inputs{
        VoiceSumInput{.source = const_view(ptrs_a, voice_a.size())},
        VoiceSumInput{.source = const_view(ptrs_b, voice_b.size()), .gain = 0.5f},
    };

    const auto result = VoiceSumMixer::mix(
        inputs,
        destination.view(),
        4,
        VoiceSumOptions{.accumulate = false});

    REQUIRE(result.frames_mixed == 4);
    REQUIRE(result.inputs_mixed == 2);
    REQUIRE(result.inputs_skipped == 0);
    for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
        REQUIRE_THAT(destination.channel(channel)[0], WithinAbs(6.0f, 1.0e-6f));
        REQUIRE_THAT(destination.channel(channel)[1], WithinAbs(7.0f, 1.0e-6f));
        REQUIRE_THAT(destination.channel(channel)[2], WithinAbs(8.0f, 1.0e-6f));
        REQUIRE_THAT(destination.channel(channel)[3], WithinAbs(9.0f, 1.0e-6f));
    }
}

TEST_CASE("VoiceSumMixer accumulates and clamps requested frames",
          "[audio][sampler][voice-sum]") {
    std::array<float, 4> voice{2.0f, 2.0f, 2.0f, 2.0f};
    std::array<const float*, 1> ptrs{voice.data()};

    Buffer<float> destination(1, 4);
    fill(destination, 1.0f);

    std::array<VoiceSumInput, 1> inputs{
        VoiceSumInput{.source = const_view(ptrs, voice.size())},
    };

    const auto result = VoiceSumMixer::mix(inputs, destination.view(), 2);

    REQUIRE(result.frames_mixed == 2);
    REQUIRE(result.inputs_mixed == 1);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(3.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[1], WithinAbs(3.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[2], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[3], WithinAbs(1.0f, 1.0e-6f));
}

TEST_CASE("VoiceSumMixer maps multichannel sources without duplicating extras",
          "[audio][sampler][voice-sum]") {
    std::array<float, 2> left{1.0f, 2.0f};
    std::array<float, 2> right{10.0f, 20.0f};
    std::array<const float*, 2> ptrs{left.data(), right.data()};

    Buffer<float> destination(4, 2);
    fill(destination, 7.0f);

    std::array<VoiceSumInput, 1> inputs{
        VoiceSumInput{.source = const_view(ptrs, left.size())},
    };

    auto result = VoiceSumMixer::mix(
        inputs,
        destination.view(),
        2,
        VoiceSumOptions{.accumulate = false});

    REQUIRE(result.inputs_mixed == 1);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[1], WithinAbs(2.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(1)[0], WithinAbs(10.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(1)[1], WithinAbs(20.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(2)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(2)[1], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(3)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(3)[1], WithinAbs(0.0f, 1.0e-6f));

    fill(destination, 7.0f);
    result = VoiceSumMixer::mix(inputs, destination.view(), 2);
    REQUIRE(result.inputs_mixed == 1);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(8.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(1)[0], WithinAbs(17.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(2)[0], WithinAbs(7.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(3)[0], WithinAbs(7.0f, 1.0e-6f));
}

TEST_CASE("VoiceSumMixer counts skipped inputs and nonfinite gains",
          "[audio][sampler][voice-sum][edge]") {
    std::array<float, 2> voice{1.0f, 1.0f};
    std::array<const float*, 1> ptrs{voice.data()};
    const auto view = const_view(ptrs, voice.size());

    Buffer<float> destination(1, 2);
    std::array<VoiceSumInput, 5> inputs{
        VoiceSumInput{.source = view},
        VoiceSumInput{.source = view, .active = false},
        VoiceSumInput{.source = view, .gain = 0.0f},
        VoiceSumInput{.source = view,
                      .gain = std::numeric_limits<float>::quiet_NaN()},
        VoiceSumInput{},
    };

    const auto result = VoiceSumMixer::mix(
        inputs,
        destination.view(),
        2,
        VoiceSumOptions{.accumulate = false});

    REQUIRE(result.inputs_mixed == 1);
    REQUIRE(result.inputs_skipped == 4);
    REQUIRE(result.nonfinite_gains == 1);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[1], WithinAbs(1.0f, 1.0e-6f));
}

TEST_CASE("VoiceSumMixer clears only the requested frame range",
          "[audio][sampler][voice-sum]") {
    Buffer<float> destination(1, 4);
    fill(destination, 5.0f);

    const auto result = VoiceSumMixer::mix(
        std::span<const VoiceSumInput>{},
        destination.view(),
        2,
        VoiceSumOptions{.accumulate = false});

    REQUIRE(result.frames_mixed == 2);
    REQUIRE(result.inputs_mixed == 0);
    REQUIRE(result.inputs_skipped == 0);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[1], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[2], WithinAbs(5.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[3], WithinAbs(5.0f, 1.0e-6f));
}

TEST_CASE("VoiceSumMixer handles zero-frame and zero-destination cases",
          "[audio][sampler][voice-sum][edge]") {
    std::array<float, 2> voice{1.0f, 1.0f};
    std::array<const float*, 1> ptrs{voice.data()};
    std::array<VoiceSumInput, 1> inputs{
        VoiceSumInput{.source = const_view(ptrs, voice.size())},
    };

    Buffer<float> destination(1, 2);
    fill(destination, 5.0f);

    auto result = VoiceSumMixer::mix(
        inputs,
        destination.view(),
        0,
        VoiceSumOptions{.accumulate = false});
    REQUIRE(result.frames_mixed == 0);
    REQUIRE(result.inputs_mixed == 0);
    REQUIRE(result.inputs_skipped == 1);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(5.0f, 1.0e-6f));

    Buffer<float> empty_destination;
    result = VoiceSumMixer::mix(inputs, empty_destination.view(), 2);
    REQUIRE(result.frames_mixed == 0);
    REQUIRE(result.inputs_mixed == 0);
    REQUIRE(result.inputs_skipped == 1);
}

TEST_CASE("VoiceSumMixer clears full request when source is shorter",
          "[audio][sampler][voice-sum][edge]") {
    std::array<float, 2> voice{1.0f, 2.0f};
    std::array<const float*, 1> ptrs{voice.data()};
    std::array<VoiceSumInput, 1> inputs{
        VoiceSumInput{.source = const_view(ptrs, voice.size())},
    };

    Buffer<float> destination(1, 4);
    fill(destination, 5.0f);

    const auto result = VoiceSumMixer::mix(
        inputs,
        destination.view(),
        4,
        VoiceSumOptions{.accumulate = false});

    REQUIRE(result.frames_mixed == 4);
    REQUIRE(result.inputs_mixed == 1);
    REQUIRE(result.inputs_skipped == 0);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[1], WithinAbs(2.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[2], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[3], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("VoiceSumMixer hot path does not allocate",
          "[audio][sampler][voice-sum][rt]") {
    std::array<float, 8> voice_a{1.0f, 1.0f, 1.0f, 1.0f,
                                 1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 8> voice_b{2.0f, 2.0f, 2.0f, 2.0f,
                                 2.0f, 2.0f, 2.0f, 2.0f};
    std::array<const float*, 1> ptrs_a{voice_a.data()};
    std::array<const float*, 1> ptrs_b{voice_b.data()};
    std::array<VoiceSumInput, 2> inputs{
        VoiceSumInput{.source = const_view(ptrs_a, voice_a.size())},
        VoiceSumInput{.source = const_view(ptrs_b, voice_b.size()), .gain = 0.25f},
    };

    Buffer<float> destination(2, 8);

    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        VoiceSumMixer::mix(
            inputs,
            destination.view(),
            8,
            VoiceSumOptions{.accumulate = false});
    }

    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(1.5f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(1)[7], WithinAbs(1.5f, 1.0e-6f));
}
