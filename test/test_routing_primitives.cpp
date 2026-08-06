#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/signal.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::AudioMatrixMixer;
using pulp::signal::ClickFreePathSwitcher;
using pulp::signal::MatrixHeadroomPolicy;
using pulp::signal::PathLatencyAligner;

template <typename T> struct ToggleFailAllocator {
    using value_type = T;

    bool* fail = nullptr;

    ToggleFailAllocator() = default;
    explicit ToggleFailAllocator(bool& should_fail) : fail(&should_fail) {}
    template <typename U>
    ToggleFailAllocator(const ToggleFailAllocator<U>& other) noexcept : fail(other.fail) {}

    T* allocate(std::size_t count) {
        if (fail != nullptr && *fail)
            throw std::bad_alloc{};
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
    }

    template <typename U> bool operator==(const ToggleFailAllocator<U>& other) const noexcept {
        return fail == other.fail;
    }
};

template <std::size_t Inputs, std::size_t Outputs>
concept AvailableAudioMatrix =
    requires { typename pulp::signal::AudioMatrixMixerT<float, Inputs, Outputs>; };

TEST_CASE("Orthonormal mid-side preserves identity, mono, and energy",
          "[signal][routing][mid-side]") {
    constexpr std::array<float, 8> left{0.0f, 1.0f, -1.0f, 0.25f, -0.7f, 0.9f, 0.3f, -0.2f};
    constexpr std::array<float, 8> right{0.0f, -1.0f, 1.0f, -0.5f, 0.2f, 0.1f, -0.8f, -0.2f};
    std::array<float, 8> mid{}, side{}, decoded_left{}, decoded_right{};
    REQUIRE(pulp::signal::mid_side_encode_block(left.data(), right.data(), mid.data(), side.data(),
                                                left.size()));
    REQUIRE(pulp::signal::mid_side_decode_block(mid.data(), side.data(), decoded_left.data(),
                                                decoded_right.data(), left.size()));
    for (std::size_t i = 0; i < left.size(); ++i) {
        CHECK_THAT(decoded_left[i], WithinAbs(left[i], 2.0e-7f));
        CHECK_THAT(decoded_right[i], WithinAbs(right[i], 2.0e-7f));
        const double stereo_energy =
            static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
        const double ms_energy =
            static_cast<double>(mid[i]) * mid[i] + static_cast<double>(side[i]) * side[i];
        CHECK_THAT(ms_energy, WithinAbs(stereo_energy, 3.0e-7));
    }

    float mono_mid{}, mono_side{};
    pulp::signal::mid_side_encode(0.375f, 0.375f, mono_mid, mono_side);
    CHECK(mono_side == 0.0f);
    CHECK_THAT(mono_mid, WithinAbs(0.375f * std::sqrt(2.0f), 1.0e-7f));

    float width_left{}, width_right{};
    CHECK(pulp::signal::stereo_width(0.8f, -0.2f, 0.0f, width_left, width_right) == 0.0f);
    CHECK_THAT(width_left, WithinAbs(width_right, 1.0e-7f));
    CHECK(pulp::signal::stereo_width(0.8f, -0.2f, std::numeric_limits<float>::quiet_NaN(),
                                     width_left, width_right) == 1.0f);
    CHECK_THAT(width_left, WithinAbs(0.8f, 2.0e-7f));
    CHECK_THAT(width_right, WithinAbs(-0.2f, 2.0e-7f));

    auto alias_left = left;
    auto alias_right = right;
    REQUIRE(pulp::signal::mid_side_encode_block(alias_left.data(), alias_right.data(),
                                                alias_left.data(), alias_right.data(),
                                                alias_left.size()));
    REQUIRE(pulp::signal::mid_side_decode_block(alias_left.data(), alias_right.data(),
                                                alias_left.data(), alias_right.data(),
                                                alias_left.size()));
    for (std::size_t i = 0; i < left.size(); ++i) {
        CHECK_THAT(alias_left[i], WithinAbs(left[i], 2.0e-7f));
        CHECK_THAT(alias_right[i], WithinAbs(right[i], 2.0e-7f));
    }
    std::array<float, 9> partial_alias{};
    CHECK_FALSE(pulp::signal::mid_side_encode_block(left.data(), right.data(), partial_alias.data(),
                                                    partial_alias.data() + 1, left.size()));

    // Negative control: the common 0.5 normalization is invertible but not
    // orthonormal; it halves energy for this anti-correlated sample.
    const double wrong_mid = (1.0 + -1.0) * 0.5;
    const double wrong_side = (1.0 - -1.0) * 0.5;
    CHECK(std::abs((wrong_mid * wrong_mid + wrong_side * wrong_side) - 2.0) > 0.9);
}

TEST_CASE("Audio matrix applies signed gains, headroom policy, aliasing, and faults",
          "[signal][routing][matrix]") {
    STATIC_REQUIRE((AvailableAudioMatrix<2, 2>));
    constexpr auto overflowing_inputs = std::numeric_limits<std::size_t>::max() / 2 + 2;
    STATIC_REQUIRE_FALSE((AvailableAudioMatrix<overflowing_inputs, 2>));

    AudioMatrixMixer mixer;
    REQUIRE(mixer.set_dimensions(2, 2));
    REQUIRE(mixer.prepare(8));
    REQUIRE(mixer.set_gain(0, 0, 1.0f));
    REQUIRE(mixer.set_gain(0, 1, -0.5f));
    REQUIRE(mixer.set_gain(1, 0, -1.0f));
    REQUIRE(mixer.set_gain(1, 1, 1.0f));

    std::array<float, 4> a{1, 2, 3, 4};
    std::array<float, 4> b{2, 2, 2, 2};
    std::array<const float*, 2> inputs{a.data(), b.data()};
    std::array<float, 4> out0{}, out1{};
    std::array<float*, 2> outputs{out0.data(), out1.data()};
    REQUIRE(mixer.process(inputs.data(), 2, outputs.data(), 2, 4));
    CHECK(out0 == std::array<float, 4>{0, 1, 2, 3});
    CHECK(out1 == std::array<float, 4>{1, 0, -1, -2});

    mixer.set_headroom_policy(MatrixHeadroomPolicy::NormalizePeak);
    REQUIRE(mixer.process(inputs.data(), 2, outputs.data(), 2, 4));
    CHECK_THAT(out0[0], WithinAbs(0.0f, 1.0e-7f));
    CHECK_THAT(out1[0], WithinAbs(0.5f, 1.0e-7f));

    mixer.set_headroom_policy(MatrixHeadroomPolicy::Raw);
    std::array<float*, 2> in_place{a.data(), b.data()};
    REQUIRE(mixer.process(inputs.data(), 2, in_place.data(), 2, 4));
    CHECK(a == std::array<float, 4>{0, 1, 2, 3});
    CHECK(b == std::array<float, 4>{1, 0, -1, -2});

    REQUIRE(mixer.set_gain(0, 0, std::numeric_limits<float>::infinity()));
    CHECK(mixer.target_gain(0, 0) == 0.0f);
    CHECK(mixer.nonfinite_gain_count() == 1);
    CHECK_FALSE(mixer.set_gain(0, 0, std::numeric_limits<float>::max()));
    CHECK(mixer.target_gain(0, 0) == 0.0f);
    CHECK(mixer.out_of_range_gain_count() == 1);

    std::array<float, 8> overlapping_outputs{};
    std::array<float*, 2> partial_outputs{overlapping_outputs.data(),
                                          overlapping_outputs.data() + 1};
    CHECK_FALSE(mixer.process(inputs.data(), 2, partial_outputs.data(), 2, 4));

    AudioMatrixMixer bounded;
    REQUIRE(bounded.set_dimensions(2, 1));
    REQUIRE(bounded.prepare(4));
    REQUIRE(bounded.set_gain(0, 0, AudioMatrixMixer::max_abs_gain));
    REQUIRE(bounded.set_gain(0, 1, AudioMatrixMixer::max_abs_gain));
    bounded.set_headroom_policy(MatrixHeadroomPolicy::NormalizePeak);
    std::array<float, 4> unity{1, 1, 1, 1}, normalized{};
    const float* unity_inputs[] = {unity.data(), unity.data()};
    float* normalized_output[] = {normalized.data()};
    REQUIRE(bounded.process(unity_inputs, 2, normalized_output, 1, 4));
    for (const auto sample : normalized) {
        CHECK(std::isfinite(sample));
        CHECK_THAT(sample, WithinAbs(1.0f, 1.0e-6f));
    }
}

TEST_CASE("Matrix gain automation is sample-continuous and block-split deterministic",
          "[signal][routing][matrix][automation]") {
    AudioMatrixMixer whole, split;
    for (auto* mixer : {&whole, &split}) {
        REQUIRE(mixer->set_dimensions(1, 1));
        REQUIRE(mixer->prepare(16));
        REQUIRE(mixer->set_gain(0, 0, 0.0f));
        REQUIRE(mixer->set_gain_ramped(0, 0, 1.0f, 8));
    }
    std::array<float, 12> input{};
    input.fill(1.0f);
    std::array<float, 12> a{}, b{};
    const float* whole_in[] = {input.data()};
    float* whole_out[] = {a.data()};
    REQUIRE(whole.process(whole_in, 1, whole_out, 1, input.size()));
    const float* split_in0[] = {input.data()};
    float* split_out0[] = {b.data()};
    REQUIRE(split.process(split_in0, 1, split_out0, 1, 5));
    const float* split_in1[] = {input.data() + 5};
    float* split_out1[] = {b.data() + 5};
    REQUIRE(split.process(split_in1, 1, split_out1, 1, 7));
    CHECK(a == b);
    CHECK(a[0] == 0.0f);
    CHECK_THAT(a[7], WithinAbs(0.875f, 1.0e-7f));
    CHECK(a[8] == 1.0f);
    for (std::size_t i = 1; i < a.size(); ++i)
        CHECK(std::abs(a[i] - a[i - 1]) <= 0.125f);

    AudioMatrixMixer retarget_whole, retarget_split;
    for (auto* mixer : {&retarget_whole, &retarget_split}) {
        REQUIRE(mixer->set_dimensions(1, 1));
        REQUIRE(mixer->prepare(16));
        REQUIRE(mixer->set_gain_ramped(0, 0, 1.0f, 8));
        std::array<float, 3> prefix{};
        const float* prefix_in[] = {input.data()};
        float* prefix_out[] = {prefix.data()};
        REQUIRE(mixer->process(prefix_in, 1, prefix_out, 1, 3));
        REQUIRE(mixer->set_gain_ramped(0, 0, -1.0f, 9));
    }
    std::array<float, 9> retarget_a{}, retarget_b{};
    float* retarget_whole_out[] = {retarget_a.data()};
    REQUIRE(retarget_whole.process(whole_in, 1, retarget_whole_out, 1, 9));
    float* retarget_split_head[] = {retarget_b.data()};
    REQUIRE(retarget_split.process(whole_in, 1, retarget_split_head, 1, 4));
    const float* retarget_split_input[] = {input.data() + 4};
    float* retarget_split_tail[] = {retarget_b.data() + 4};
    REQUIRE(retarget_split.process(retarget_split_input, 1, retarget_split_tail, 1, 5));
    CHECK(retarget_a == retarget_b);

    SECTION("float ramps retain progress beyond the exact-integer boundary") {
        constexpr std::size_t ramp_samples = 33'554'434;
        constexpr std::size_t checkpoint = 25'165'826;
        pulp::signal::AudioMatrixMixerT<float, 1, 2> long_ramp;
        REQUIRE(long_ramp.set_dimensions(1, 2));
        REQUIRE(long_ramp.prepare(4096));
        REQUIRE(long_ramp.set_gain_ramped(0, 0, 1.0f, ramp_samples));
        REQUIRE(long_ramp.set_gain_ramped(1, 0, -1.0f, ramp_samples));
        std::array<float, 4096> long_input{}, rising_output{}, falling_output{};
        long_input.fill(1.0f);
        const float* long_inputs[] = {long_input.data()};
        float* long_outputs[] = {rising_output.data(), falling_output.data()};
        auto advance = [&](std::size_t frames) {
            std::array<float, 2> last{};
            while (frames != 0) {
                const auto block = std::min(frames, long_input.size());
                REQUIRE(long_ramp.process(long_inputs, 1, long_outputs, 2, block));
                last = {rising_output[block - 1], falling_output[block - 1]};
                frames -= block;
            }
            return last;
        };
        advance(checkpoint);
        const auto expected =
            static_cast<float>(static_cast<double>(checkpoint) / static_cast<double>(ramp_samples));
        CHECK_THAT(long_ramp.gain(0, 0), WithinAbs(expected, 1.0e-7f));
        CHECK(long_ramp.gain(0, 0) > 0.5f);
        CHECK_THAT(long_ramp.gain(1, 0), WithinAbs(-expected, 1.0e-7f));
        CHECK(long_ramp.gain(1, 0) < -0.5f);
        const auto last_ramp_sample = advance(ramp_samples - checkpoint);
        CHECK(last_ramp_sample[0] == std::nextafter(1.0f, 0.0f));
        CHECK(last_ramp_sample[1] == std::nextafter(-1.0f, 0.0f));
        CHECK(long_ramp.gain(0, 0) == 1.0f);
        CHECK(long_ramp.gain(1, 0) == -1.0f);
        advance(1);
        CHECK(rising_output[0] == 1.0f);
        CHECK(falling_output[0] == -1.0f);
    }
}

TEST_CASE("N-way crossfade obeys endpoints and constant-power law",
          "[signal][routing][crossfade]") {
    std::array<double, 5> gains{};
    for (double position : {0.0, 0.25, 1.0, 1.5, 3.75, 4.0}) {
        REQUIRE(pulp::signal::nway_constant_power_gains(position, std::span<double>(gains)));
        double power = 0.0;
        for (const auto gain : gains)
            power += gain * gain;
        CHECK_THAT(power, WithinAbs(1.0, 1.0e-12));
    }
    REQUIRE(pulp::signal::nway_constant_power_gains(2.5, std::span<double>(gains)));
    CHECK_THAT(gains[2], WithinAbs(std::sqrt(0.5), 1.0e-12));
    CHECK_THAT(gains[3], WithinAbs(std::sqrt(0.5), 1.0e-12));
    CHECK(gains[0] == 0.0);
    REQUIRE(pulp::signal::nway_constant_power_gains(std::numeric_limits<double>::quiet_NaN(),
                                                    std::span<double>(gains)));
    CHECK(gains[0] == 1.0);
    CHECK_FALSE(pulp::signal::nway_constant_power_gains(0.0, std::span<double>{}));

    constexpr auto float_consecutive_integer_limit = std::size_t{1}
                                                     << std::numeric_limits<float>::digits;
    std::vector<float> large_gains(float_consecutive_integer_limit + 2);
    REQUIRE(pulp::signal::nway_constant_power_gains(
        static_cast<float>(float_consecutive_integer_limit), std::span<float>(large_gains)));
    CHECK(large_gains[float_consecutive_integer_limit] == 1.0f);
    CHECK(large_gains.back() == 0.0f);
    // Negative control: a linear midpoint has half the required power.
    CHECK(std::abs((0.5 * 0.5 + 0.5 * 0.5) - 1.0) > 0.49);
}

TEST_CASE("N-path switching is continuous under retargeting and block splits",
          "[signal][routing][switcher][automation]") {
    ClickFreePathSwitcher whole, split;
    REQUIRE(whole.configure(3, 0, 16));
    REQUIRE(split.configure(3, 0, 16));
    REQUIRE(whole.request_path(2));
    REQUIRE(split.request_path(2));
    std::array<float, 24> p0{}, p1{}, p2{};
    p0.fill(1.0f);
    p1.fill(-0.25f);
    p2.fill(0.5f);
    const float* paths[] = {p0.data(), p1.data(), p2.data()};
    std::array<float, 24> a{}, b{};
    REQUIRE(whole.process(paths, 3, a.data(), a.size()));
    REQUIRE(split.process(paths, 3, b.data(), 7));
    const float* tail_paths[] = {p0.data() + 7, p1.data() + 7, p2.data() + 7};
    REQUIRE(split.process(tail_paths, 3, b.data() + 7, b.size() - 7));
    CHECK(a == b);

    ClickFreePathSwitcher endpoint;
    REQUIRE(endpoint.configure(2, 0, 4));
    REQUIRE(endpoint.request_path(1));
    std::array<float, 2> endpoint_gains{};
    REQUIRE(endpoint.next_gains(endpoint_gains));
    CHECK(endpoint_gains == std::array<float, 2>{1, 0});
    REQUIRE(endpoint.next_gains(endpoint_gains));
    REQUIRE(endpoint.next_gains(endpoint_gains));
    REQUIRE(endpoint.next_gains(endpoint_gains));
    CHECK(endpoint_gains == std::array<float, 2>{0, 1});

    SECTION("float fades reach the target only on the requested final frame") {
        constexpr std::size_t fade_samples = 16'777'218;
        pulp::signal::ClickFreePathSwitcherT<float, 2> boundary;
        REQUIRE(boundary.configure(2, 0, fade_samples));
        REQUIRE(boundary.request_path(1));
        std::array<float, 2> boundary_gains{};
        for (std::size_t frame = 0; frame + 1 < fade_samples; ++frame)
            REQUIRE(boundary.next_gains(boundary_gains));
        CHECK(boundary.switching());
        CHECK(boundary_gains[0] > 0.0f);
        CHECK(boundary_gains != std::array<float, 2>{0, 1});
        REQUIRE(boundary.next_gains(boundary_gains));
        CHECK_FALSE(boundary.switching());
        CHECK(boundary_gains == std::array<float, 2>{0, 1});
    }

    ClickFreePathSwitcher retarget;
    REQUIRE(retarget.configure(3, 0, 32));
    REQUIRE(retarget.request_path(2));
    std::array<float, 3> before{}, after{};
    for (int i = 0; i < 11; ++i)
        REQUIRE(retarget.next_gains(before));
    const auto current = retarget.weights();
    std::copy(current.begin(), current.end(), before.begin());
    REQUIRE(retarget.request_path(1));
    REQUIRE(retarget.next_gains(after));
    CHECK(after == before);
    for (int i = 0; i < 32; ++i) {
        REQUIRE(retarget.next_gains(after));
        float power = 0.0f;
        for (float gain : after)
            power += gain * gain;
        CHECK_THAT(power, WithinAbs(1.0f, 2.0e-6f));
    }

    ClickFreePathSwitcher retarget_whole, retarget_split;
    for (auto* instance : {&retarget_whole, &retarget_split}) {
        REQUIRE(instance->configure(3, 0, 16));
        REQUIRE(instance->request_path(2));
        for (int i = 0; i < 7; ++i)
            REQUIRE(instance->next_gains(after));
        REQUIRE(instance->request_path(1));
    }
    std::array<float, 20> whole_retarget{}, split_retarget{};
    REQUIRE(retarget_whole.process(paths, 3, whole_retarget.data(), 20));
    REQUIRE(retarget_split.process(paths, 3, split_retarget.data(), 6));
    const float* retarget_tail_paths[] = {p0.data() + 6, p1.data() + 6, p2.data() + 6};
    REQUIRE(retarget_split.process(retarget_tail_paths, 3, split_retarget.data() + 6, 14));
    CHECK(whole_retarget == split_retarget);

    std::array<float, 25> partial_destination{};
    const float* overlapping_paths[] = {partial_destination.data() + 1, p1.data(), p2.data()};
    CHECK_FALSE(retarget.process(overlapping_paths, 3, partial_destination.data(), 16));
}

TEST_CASE("N-path latency alignment matches impulses and reported latency",
          "[signal][routing][latency][impulse]") {
    PathLatencyAligner aligner;
    REQUIRE(aligner.prepare(3, 1, 8, 16));
    PathLatencyAligner overflow_guard;
    CHECK_FALSE(overflow_guard.prepare(1, 1, std::numeric_limits<std::size_t>::max(), 16));
    const std::array<std::size_t, 3> latencies{2, 5, 0};
    REQUIRE(aligner.configure_latencies(latencies));
    REQUIRE(aligner.reported_latency_samples() == 5);
    CHECK(aligner.compensation_delay_samples(0) == 3);
    CHECK(aligner.compensation_delay_samples(1) == 0);
    CHECK(aligner.compensation_delay_samples(2) == 5);
    const std::array<std::size_t, 3> invalid_latencies{2, 9, 0};
    CHECK_FALSE(aligner.configure_latencies(invalid_latencies));
    CHECK(aligner.reported_latency_samples() == 5);
    CHECK(aligner.path_latency_samples(1) == 5);

    std::array<float, 12> path0{}, path1{}, path2{};
    path0[2] = 1.0f;
    path1[5] = 1.0f;
    path2[0] = 1.0f;
    const float* inputs[] = {path0.data(), path1.data(), path2.data()};
    std::array<float, 12> out0{}, out1{}, out2{};
    float* outputs[] = {out0.data(), out1.data(), out2.data()};
    REQUIRE(aligner.process(inputs, 3, outputs, 3, 12));
    for (const auto* output : {out0.data(), out1.data(), out2.data()}) {
        CHECK(output[5] == 1.0f);
        for (std::size_t frame = 0; frame < 12; ++frame)
            if (frame != 5)
                CHECK(output[frame] == 0.0f);
    }
    // Negative control: the intrinsic path outputs are visibly unaligned.
    CHECK(path0[2] == 1.0f);
    CHECK(path1[2] == 0.0f);
    CHECK(path2[2] == 0.0f);

    PathLatencyAligner split;
    REQUIRE(split.prepare(3, 1, 8, 16));
    REQUIRE(split.configure_latencies(latencies));
    std::array<float, 12> split0{}, split1{}, split2{};
    float* split_head[] = {split0.data(), split1.data(), split2.data()};
    REQUIRE(split.process(inputs, 3, split_head, 3, 4));
    const float* tail_inputs[] = {path0.data() + 4, path1.data() + 4, path2.data() + 4};
    float* split_tail[] = {split0.data() + 4, split1.data() + 4, split2.data() + 4};
    REQUIRE(split.process(tail_inputs, 3, split_tail, 3, 8));
    CHECK(split0 == out0);
    CHECK(split1 == out1);
    CHECK(split2 == out2);

    std::array<float, 16> partial_storage{};
    const float* partial_inputs[] = {partial_storage.data(), path1.data(), path2.data()};
    float* partial_latency_outputs[] = {partial_storage.data() + 1, out1.data(), out2.data()};
    CHECK_FALSE(split.process(partial_inputs, 3, partial_latency_outputs, 3, 8));
}

TEST_CASE("Routing prepare failures preserve the previous prepared state",
          "[signal][routing][prepare][failure]") {
    bool fail_allocations = false;
    using Allocator = ToggleFailAllocator<float>;

    pulp::signal::AudioMatrixMixerT<float, 1, 1, Allocator> matrix{Allocator{fail_allocations}};
    REQUIRE(matrix.set_dimensions(1, 1));
    REQUIRE(matrix.prepare(4));
    REQUIRE(matrix.set_gain(0, 0, 0.5f));
    fail_allocations = true;
    CHECK_FALSE(matrix.prepare(8));
    CHECK(matrix.max_block_size() == 4);
    CHECK(matrix.gain(0, 0) == 0.5f);
    std::array<float, 4> matrix_input{2, 2, 2, 2}, matrix_output{};
    const float* matrix_inputs[] = {matrix_input.data()};
    float* matrix_outputs[] = {matrix_output.data()};
    REQUIRE(matrix.process(matrix_inputs, 1, matrix_outputs, 1, 4));
    CHECK(matrix_output == std::array<float, 4>{1, 1, 1, 1});

    fail_allocations = false;
    pulp::signal::PathLatencyAlignerT<float, 2, 1, Allocator> aligner{Allocator{fail_allocations}};
    REQUIRE(aligner.prepare(2, 1, 4, 4));
    const std::array<std::size_t, 2> latencies{0, 4};
    REQUIRE(aligner.configure_latencies(latencies));
    std::array<float, 2> impulse{1, 0}, silence2{}, prefix0{}, prefix1{};
    const float* prefix_inputs[] = {impulse.data(), silence2.data()};
    float* prefix_outputs[] = {prefix0.data(), prefix1.data()};
    REQUIRE(aligner.process(prefix_inputs, 2, prefix_outputs, 2, 2));

    const auto retained = aligner.retained_samples();
    fail_allocations = true;
    CHECK_FALSE(aligner.prepare(1, 1, 2, 8));
    CHECK(aligner.path_count() == 2);
    CHECK(aligner.channel_count() == 1);
    CHECK(aligner.reported_latency_samples() == 4);
    CHECK(aligner.retained_samples() == retained);
    std::array<float, 5> oversized_input{}, oversized0{}, oversized1{};
    const float* oversized_inputs[] = {oversized_input.data(), oversized_input.data()};
    float* oversized_outputs[] = {oversized0.data(), oversized1.data()};
    CHECK_FALSE(aligner.process(oversized_inputs, 2, oversized_outputs, 2, 5));
    std::array<float, 3> silence3{}, continuation0{}, continuation1{};
    const float* continuation_inputs[] = {silence3.data(), silence3.data()};
    float* continuation_outputs[] = {continuation0.data(), continuation1.data()};
    REQUIRE(aligner.process(continuation_inputs, 2, continuation_outputs, 2, 3));
    CHECK(continuation0 == std::array<float, 3>{0, 0, 1});
}

TEST_CASE("Routing reset and reprepare semantics are deterministic", "[signal][routing][reset]") {
    AudioMatrixMixer matrix;
    REQUIRE(matrix.set_dimensions(1, 1));
    REQUIRE(matrix.prepare(4));
    REQUIRE(matrix.set_gain_ramped(0, 0, 1.0f, 8));
    std::array<float, 4> input{1, 1, 1, 1}, output{};
    const float* inputs[] = {input.data()};
    float* outputs[] = {output.data()};
    REQUIRE(matrix.process(inputs, 1, outputs, 1, 2));
    matrix.reset();
    REQUIRE(matrix.process(inputs, 1, outputs, 1, 4));
    CHECK(output == std::array<float, 4>{1, 1, 1, 1});
    REQUIRE(matrix.prepare(4));
    REQUIRE(matrix.process(inputs, 1, outputs, 1, 4));
    CHECK(output == std::array<float, 4>{1, 1, 1, 1});

    ClickFreePathSwitcher switcher;
    REQUIRE(switcher.configure(2, 0, 8));
    REQUIRE(switcher.request_path(1));
    std::array<float, 2> gains{};
    REQUIRE(switcher.next_gains(gains));
    switcher.reset();
    REQUIRE(switcher.next_gains(gains));
    CHECK(gains == std::array<float, 2>{0, 1});

    PathLatencyAligner aligner;
    REQUIRE(aligner.prepare(2, 1, 4, 4));
    const std::array<std::size_t, 2> latencies{0, 4};
    REQUIRE(aligner.configure_latencies(latencies));
    std::array<float, 4> impulse{1, 0, 0, 0}, zeros{}, delayed{}, direct{};
    const float* latency_inputs[] = {impulse.data(), zeros.data()};
    float* latency_outputs[] = {delayed.data(), direct.data()};
    REQUIRE(aligner.process(latency_inputs, 2, latency_outputs, 2, 4));
    aligner.reset();
    const float* zero_inputs[] = {zeros.data(), zeros.data()};
    REQUIRE(aligner.process(zero_inputs, 2, latency_outputs, 2, 4));
    CHECK(delayed == zeros);
    CHECK(direct == zeros);
}

TEST_CASE("Prepared routing process paths allocate nothing", "[signal][routing][rt-safety]") {
    AudioMatrixMixer matrix;
    REQUIRE(matrix.set_dimensions(2, 1));
    REQUIRE(matrix.prepare(16));
    REQUIRE(matrix.set_gain(0, 0, 0.5f));
    REQUIRE(matrix.set_gain(0, 1, -0.25f));
    ClickFreePathSwitcher switcher;
    REQUIRE(switcher.configure(2, 0, 8));
    REQUIRE(switcher.request_path(1));
    PathLatencyAligner aligner;
    REQUIRE(aligner.prepare(2, 1, 8, 16));
    const std::array<std::size_t, 2> latencies{2, 7};
    REQUIRE(aligner.configure_latencies(latencies));

    std::array<float, 16> a{}, b{}, out{}, aligned0{}, aligned1{};
    a.fill(0.5f);
    b.fill(-0.25f);
    const float* two_inputs[] = {a.data(), b.data()};
    float* one_output[] = {out.data()};
    float* two_outputs[] = {aligned0.data(), aligned1.data()};
    bool matrix_ok = false;
    bool switcher_ok = false;
    bool aligner_ok = false;
    std::size_t allocation_count = 0;
    std::size_t allocated_bytes = 0;
    {
        pulp::test::RtAllocationProbe probe;
        matrix_ok = matrix.process(two_inputs, 2, one_output, 1, 16);
        switcher_ok = switcher.process(two_inputs, 2, out.data(), 16);
        aligner_ok = aligner.process(two_inputs, 2, two_outputs, 2, 16);
        allocation_count = probe.allocation_count();
        allocated_bytes = probe.allocated_bytes();
    }
    CHECK(matrix_ok);
    CHECK(switcher_ok);
    CHECK(aligner_ok);
    CHECK(allocation_count == 0);
    CHECK(allocated_bytes == 0);
}
