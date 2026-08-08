#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/signal/audio_matrix_mixer.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

using Catch::Matchers::WithinAbs;
using pulp::signal::AudioMatrixMixerT;
using pulp::signal::AudioMatrixPlanError;
using pulp::signal::AudioMatrixPlanT;
using pulp::signal::AudioMatrixProcessStatus;
using pulp::signal::AudioMatrixSummingLaw;

namespace {

template <typename T, std::size_t InputCount, std::size_t OutputCount,
          std::size_t Frames>
auto process(AudioMatrixMixerT<T, InputCount, OutputCount>& mixer,
             const std::array<std::array<T, Frames>, InputCount>& input_storage,
             std::array<std::array<T, Frames>, OutputCount>& output_storage,
             std::size_t frames = Frames) {
    std::array<std::span<const T>, InputCount> inputs{};
    std::array<std::span<T>, OutputCount> outputs{};
    for (std::size_t i = 0; i < InputCount; ++i)
        inputs[i] = input_storage[i];
    for (std::size_t i = 0; i < OutputCount; ++i)
        outputs[i] = output_storage[i];
    return mixer.process(inputs, outputs, frames);
}

float deterministic_value(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<std::int32_t>(state >> 8)) /
           static_cast<float>(0x7fffff);
}

}  // namespace

TEST_CASE("AudioMatrixPlan rejects malformed configurations transactionally",
          "[signal][audio-matrix][validation]") {
    using Plan = AudioMatrixPlanT<float, 2, 3>;
    const std::array gains{1.0f, 0.0f, 0.5f, -0.5f};

    auto result = Plan::prepare(0, 2, gains);
    REQUIRE(result.error() == AudioMatrixPlanError::NoInputs);
    result = Plan::prepare(2, 0, gains);
    REQUIRE(result.error() == AudioMatrixPlanError::NoOutputs);
    result = Plan::prepare(3, 1, gains);
    REQUIRE(result.error() == AudioMatrixPlanError::TooManyInputs);
    result = Plan::prepare(1, 4, gains);
    REQUIRE(result.error() == AudioMatrixPlanError::TooManyOutputs);
    result = Plan::prepare(2, 2, std::span<const float>{gains}.first(3));
    REQUIRE(result.error() == AudioMatrixPlanError::WrongGainCount);

    auto nonfinite = gains;
    nonfinite[2] = std::numeric_limits<float>::infinity();
    result = Plan::prepare(2, 2, nonfinite);
    REQUIRE(result.error() == AudioMatrixPlanError::NonfiniteGain);

    result = Plan::prepare(2, 2, gains,
                           static_cast<AudioMatrixSummingLaw>(255));
    REQUIRE(result.error() == AudioMatrixPlanError::InvalidSummingLaw);

    result = Plan::prepare(2, 2, gains);
    REQUIRE(result.has_value());
    REQUIRE(result->valid());
    REQUIRE(result->input_count() == 2);
    REQUIRE(result->output_count() == 2);
    REQUIRE(result->gain(1, 0) == 0.5f);
    REQUIRE(result->gain(9, 9) == 0.0f);
}

TEST_CASE("AudioMatrixMixer applies signed output-major direct gains",
          "[signal][audio-matrix][oracle]") {
    using Plan = AudioMatrixPlanT<float, 3, 2>;
    using Mixer = AudioMatrixMixerT<float, 3, 2>;
    const std::array gains{
        1.0f, -0.5f, 0.25f,
        -1.0f, 2.0f, 0.0f,
    };
    const auto prepared = Plan::prepare(3, 2, gains);
    REQUIRE(prepared.has_value());
    Mixer mixer(*prepared);

    const std::array<std::array<float, 4>, 3> inputs{{
        {1.0f, 2.0f, -1.0f, 0.5f},
        {2.0f, -2.0f, 4.0f, 0.0f},
        {4.0f, 8.0f, -4.0f, 2.0f},
    }};
    std::array<std::array<float, 4>, 2> outputs{};

    const auto result = process(mixer, inputs, outputs);
    REQUIRE(result.status == AudioMatrixProcessStatus::Ok);
    REQUIRE(result.frames_processed == 4);
    const std::array expected_0{1.0f, 5.0f, -4.0f, 1.0f};
    const std::array expected_1{3.0f, -6.0f, 9.0f, -0.5f};
    REQUIRE(outputs[0] == expected_0);
    REQUIRE(outputs[1] == expected_1);
}

TEST_CASE("AudioMatrixMixer matches an independent scalar matrix oracle",
          "[signal][audio-matrix][oracle]") {
    using Plan = AudioMatrixPlanT<float, 4, 3>;
    using Mixer = AudioMatrixMixerT<float, 4, 3>;
    constexpr std::size_t kFrames = 257;
    std::uint32_t state = 0x6d2b79f5u;
    std::array<float, 12> gains{};
    for (auto& gain : gains)
        gain = deterministic_value(state) * 1.75f;

    const auto prepared = Plan::prepare(4, 3, gains);
    REQUIRE(prepared.has_value());
    Mixer mixer(*prepared);

    std::array<std::array<float, kFrames>, 4> inputs{};
    for (auto& input : inputs)
        for (auto& sample : input)
            sample = deterministic_value(state) * 0.75f;
    std::array<std::array<float, kFrames>, 3> outputs{};
    REQUIRE(process(mixer, inputs, outputs));

    for (std::size_t output = 0; output < 3; ++output) {
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            long double expected = 0.0L;
            for (std::size_t input = 0; input < 4; ++input) {
                expected += static_cast<long double>(inputs[input][frame]) *
                            static_cast<long double>(gains[output * 4 + input]);
            }
            REQUIRE_THAT(outputs[output][frame],
                         WithinAbs(static_cast<float>(expected), 1.0e-5f));
        }
    }
}

TEST_CASE("NormalizeAbsoluteSum declares and enforces conservative headroom",
          "[signal][audio-matrix][headroom]") {
    using Plan = AudioMatrixPlanT<double, 3, 2>;
    using Mixer = AudioMatrixMixerT<double, 3, 2>;
    const std::array gains{
        2.0, -1.0, 1.0,
        0.25, 0.25, 0.25,
    };
    const auto prepared = Plan::prepare(
        3, 2, gains, AudioMatrixSummingLaw::NormalizeAbsoluteSum);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared->absolute_gain_sum(0) == 4.0);
    REQUIRE(prepared->gain(0, 0) == 0.5);
    REQUIRE(prepared->gain(0, 1) == -0.25);
    REQUIRE(prepared->absolute_gain_sum(1) == 0.75);
    REQUIRE(prepared->gain(1, 0) == 0.25);

    Mixer mixer(*prepared);
    const std::array<std::array<double, 3>, 3> inputs{{
        {1.0, -1.0, 0.5},
        {-1.0, 1.0, -0.5},
        {1.0, -1.0, 0.5},
    }};
    std::array<std::array<double, 3>, 2> outputs{};
    REQUIRE(process(mixer, inputs, outputs));

    REQUIRE(outputs[0][0] == 1.0);
    REQUIRE(outputs[0][1] == -1.0);
    for (const auto& output : outputs)
        for (double sample : output)
            REQUIRE(std::abs(sample) <= 1.0);
}

TEST_CASE("NormalizeAbsoluteSum prevents finite large-gain intermediate overflow",
          "[signal][audio-matrix][headroom][edge]") {
    const auto run = []<typename T>() {
        using Plan = AudioMatrixPlanT<T, 2, 1>;
        using Mixer = AudioMatrixMixerT<T, 2, 1>;
        const auto huge = std::numeric_limits<T>::max();
        const std::array gains{huge, huge};
        const auto prepared = Plan::prepare(
            2, 1, gains, AudioMatrixSummingLaw::NormalizeAbsoluteSum);
        REQUIRE(prepared.has_value());
        REQUIRE(std::isfinite(prepared->absolute_gain_sum(0)));
        REQUIRE(prepared->absolute_gain_sum(0) == huge);

        Mixer mixer(*prepared);
        const std::array<std::array<T, 2>, 2> inputs{{
            {T{1}, T{-1}},
            {T{1}, T{-1}},
        }};
        std::array<std::array<T, 2>, 1> outputs{};
        REQUIRE(process(mixer, inputs, outputs));
        REQUIRE(std::isfinite(outputs[0][0]));
        REQUIRE(std::isfinite(outputs[0][1]));
        REQUIRE(std::abs(outputs[0][0] - T{1}) <=
                std::numeric_limits<T>::epsilon() * T{4});
        REQUIRE(std::abs(outputs[0][1] + T{1}) <=
                std::numeric_limits<T>::epsilon() * T{4});
    };

    run.template operator()<float>();
    run.template operator()<double>();
    run.template operator()<long double>();
}

TEST_CASE("NormalizeAbsoluteSum stays bounded after non-power-of-two coefficient rounding",
          "[signal][audio-matrix][headroom][edge]") {
    using Plan = AudioMatrixPlanT<float, 10, 1>;
    using Mixer = AudioMatrixMixerT<float, 10, 1>;
    std::array<float, 10> gains{};
    gains.fill(1.0f);
    const auto prepared = Plan::prepare(
        10, 1, gains, AudioMatrixSummingLaw::NormalizeAbsoluteSum);
    REQUIRE(prepared.has_value());

    long double effective_sum = 0.0L;
    for (std::size_t input = 0; input < 10; ++input)
        effective_sum += std::abs(static_cast<long double>(prepared->gain(0, input)));
    REQUIRE(effective_sum <= 1.0L);

    Mixer mixer(*prepared);
    std::array<std::array<float, 1>, 10> inputs{};
    for (auto& input : inputs)
        input[0] = 1.0f;
    std::array<std::array<float, 1>, 1> outputs{};
    REQUIRE(process(mixer, inputs, outputs));
    REQUIRE(outputs[0][0] <= 1.0f);
    REQUIRE(outputs[0][0] > 0.99999f);
}

TEST_CASE("AudioMatrixMixer rejects invalid buffers before writing",
          "[signal][audio-matrix][validation]") {
    using Plan = AudioMatrixPlanT<float, 2, 1>;
    using Mixer = AudioMatrixMixerT<float, 2, 1>;
    const std::array gains{1.0f, 1.0f};
    const auto prepared = Plan::prepare(2, 1, gains);
    REQUIRE(prepared.has_value());
    Mixer mixer(*prepared);

    std::array<float, 4> source_a{1, 2, 3, 4};
    std::array<float, 4> source_b{5, 6, 7, 8};
    std::array<float, 4> destination{9, 9, 9, 9};
    const std::array<std::span<const float>, 2> inputs{source_a, source_b};
    std::array<std::span<float>, 1> outputs{destination};

    auto result = mixer.process(std::span{inputs}.first(1), outputs, 4);
    REQUIRE(result.status == AudioMatrixProcessStatus::InsufficientInputs);
    REQUIRE(destination == std::array{9.0f, 9.0f, 9.0f, 9.0f});

    result = mixer.process(inputs, std::span<std::span<float>>{}, 4);
    REQUIRE(result.status == AudioMatrixProcessStatus::InsufficientOutputs);
    REQUIRE(destination == std::array{9.0f, 9.0f, 9.0f, 9.0f});

    auto short_inputs = inputs;
    short_inputs[1] = short_inputs[1].first(3);
    result = mixer.process(short_inputs, outputs, 4);
    REQUIRE(result.status == AudioMatrixProcessStatus::ShortInput);
    REQUIRE(destination == std::array{9.0f, 9.0f, 9.0f, 9.0f});

    outputs[0] = std::span{destination}.first(3);
    result = mixer.process(inputs, outputs, 4);
    REQUIRE(result.status == AudioMatrixProcessStatus::ShortOutput);
    REQUIRE(destination == std::array{9.0f, 9.0f, 9.0f, 9.0f});

    outputs[0] = source_a;
    result = mixer.process(inputs, outputs, 4);
    REQUIRE(result.status == AudioMatrixProcessStatus::OverlappingBuffers);
    REQUIRE(source_a == std::array{1.0f, 2.0f, 3.0f, 4.0f});
}

TEST_CASE("AudioMatrixMixer plan publication is whole and boundary-latched",
          "[signal][audio-matrix][transaction]") {
    using Plan = AudioMatrixPlanT<float, 1, 1>;
    using Mixer = AudioMatrixMixerT<float, 1, 1>;
    const std::array unity{1.0f};
    const std::array inverse{-1.0f};
    const auto plan_a = Plan::prepare(1, 1, unity);
    const auto plan_b = Plan::prepare(1, 1, inverse);
    REQUIRE(plan_a.has_value());
    REQUIRE(plan_b.has_value());
    Mixer mixer(*plan_a);

    const std::array<std::array<float, 4>, 1> inputs{{{1, 2, 3, 4}}};
    std::array<std::array<float, 4>, 1> outputs{};
    REQUIRE(process(mixer, inputs, outputs));
    REQUIRE(outputs[0] == inputs[0]);

    REQUIRE_FALSE(mixer.publish(Plan{}));
    REQUIRE(process(mixer, inputs, outputs));
    REQUIRE(outputs[0] == inputs[0]);

    REQUIRE(mixer.publish(*plan_b));
    REQUIRE(process(mixer, inputs, outputs));
    REQUIRE(outputs[0] == std::array{-1.0f, -2.0f, -3.0f, -4.0f});
}

TEST_CASE("AudioMatrixMixer is bit-exact across block partitions",
          "[signal][audio-matrix][determinism]") {
    using Plan = AudioMatrixPlanT<float, 3, 2>;
    using Mixer = AudioMatrixMixerT<float, 3, 2>;
    constexpr std::size_t kFrames = 509;
    const std::array gains{0.25f, -0.5f, 1.25f, 0.75f, 0.125f, -0.375f};
    const auto prepared = Plan::prepare(3, 2, gains);
    REQUIRE(prepared.has_value());
    Mixer whole(*prepared);
    Mixer partitioned(*prepared);

    std::uint32_t state = 0x12345678u;
    std::array<std::array<float, kFrames>, 3> inputs{};
    for (auto& input : inputs)
        for (auto& sample : input)
            sample = deterministic_value(state);
    std::array<std::array<float, kFrames>, 2> expected{};
    std::array<std::array<float, kFrames>, 2> actual{};
    REQUIRE(process(whole, inputs, expected));

    constexpr std::array partitions{1u, 17u, 3u, 128u, 5u, 64u, 2u, 97u};
    std::size_t offset = 0;
    std::size_t partition_index = 0;
    while (offset < kFrames) {
        const auto frames = std::min<std::size_t>(
            partitions[partition_index++ % partitions.size()], kFrames - offset);
        std::array<std::span<const float>, 3> input_views{};
        std::array<std::span<float>, 2> output_views{};
        for (std::size_t i = 0; i < input_views.size(); ++i)
            input_views[i] = std::span{inputs[i]}.subspan(offset, frames);
        for (std::size_t i = 0; i < output_views.size(); ++i)
            output_views[i] = std::span{actual[i]}.subspan(offset, frames);
        REQUIRE(partitioned.process(input_views, output_views, frames));
        offset += frames;
    }

    for (std::size_t output = 0; output < expected.size(); ++output) {
        for (std::size_t frame = 0; frame < kFrames; ++frame) {
            REQUIRE(std::bit_cast<std::uint32_t>(actual[output][frame]) ==
                    std::bit_cast<std::uint32_t>(expected[output][frame]));
        }
    }
}

TEST_CASE("AudioMatrixMixer Release process path does not allocate",
          "[signal][audio-matrix][rt]") {
    using Plan = AudioMatrixPlanT<float, 4, 4>;
    using Mixer = AudioMatrixMixerT<float, 4, 4>;
    std::array<float, 16> gains{};
    for (std::size_t i = 0; i < 4; ++i)
        gains[i * 4 + i] = 1.0f;
    const auto prepared = Plan::prepare(4, 4, gains);
    REQUIRE(prepared.has_value());
    Mixer mixer(*prepared);

    std::array<std::array<float, 128>, 4> inputs{};
    std::array<std::array<float, 128>, 4> outputs{};
    for (std::size_t input = 0; input < inputs.size(); ++input)
        inputs[input].fill(static_cast<float>(input + 1));
    std::array<std::span<const float>, 4> input_views{
        inputs[0], inputs[1], inputs[2], inputs[3]};
    std::array<std::span<float>, 4> output_views{
        outputs[0], outputs[1], outputs[2], outputs[3]};

    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (int iteration = 0; iteration < 100; ++iteration) {
            const auto result = mixer.process(input_views, output_views, 128);
            if (!result)
                std::terminate();
        }
    }

    for (std::size_t output = 0; output < outputs.size(); ++output)
        REQUIRE(outputs[output][127] == static_cast<float>(output + 1));
}

TEST_CASE("An unprepared AudioMatrixMixer fails closed",
          "[signal][audio-matrix][validation]") {
    AudioMatrixMixerT<float, 1, 1> mixer;
    std::array<float, 1> input{1.0f};
    std::array<float, 1> output{7.0f};
    const std::array<std::span<const float>, 1> inputs{input};
    std::array<std::span<float>, 1> outputs{output};
    const auto result = mixer.process(inputs, outputs, 1);
    REQUIRE(result.status == AudioMatrixProcessStatus::Unprepared);
    REQUIRE(output[0] == 7.0f);
}
