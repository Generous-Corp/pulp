#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_runtime_state.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <variant>

namespace pulp::audio {

enum class SampleHeritageBusDspStatus : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidProfile,
    InvalidDimensions,
};

/// Stateful post-mix processor for typed heritage bus blocks. Spectral tilt law
/// v1 is a fixed cascade of octave-width shelves normalized at the profile's
/// reference frequency. Preparation computes all coefficients; process is
/// allocation-free and advances one deterministic RNG stream per noise block.
class SampleHeritageBusDsp {
public:
    SampleHeritageBusDspStatus prepare(
        const SampleHeritagePreparedProfile& profile,
        double sample_rate,
        std::size_t channel_count) noexcept {
        release();
        if (profile.schema_version != kSampleHeritageProfileSchemaVersion ||
            profile.bus_count > kSampleHeritageMaximumBusBlocks ||
            !(sample_rate >= 8000.0) || sample_rate > 384000.0 ||
            !std::isfinite(sample_rate) || channel_count == 0 ||
            channel_count > kSampleHeritageMaximumChannels)
            return SampleHeritageBusDspStatus::InvalidDimensions;

        profile_ = profile;
        sample_rate_ = sample_rate;
        channel_count_ = channel_count;
        std::array<bool, kSampleHeritageMaximumBusBlocks> seen{};
        std::size_t previous_type = 0;
        bool have_previous_type = false;
        for (std::size_t index = 0; index < profile_.bus_count; ++index) {
            const auto& spec = profile_.bus[index];
            if (spec.domain != SampleHeritageBlockDomain::Bus) {
                release();
                return SampleHeritageBusDspStatus::InvalidProfile;
            }
            const auto type = spec.parameters.index();
            if (type >= seen.size() || seen[type] ||
                (have_previous_type && type < previous_type)) {
                release();
                return SampleHeritageBusDspStatus::InvalidProfile;
            }
            seen[type] = true;
            previous_type = type;
            have_previous_type = true;
            const auto valid = std::visit(
                [&](const auto& block) noexcept {
                    using Block = std::decay_t<decltype(block)>;
                    if constexpr (std::is_same_v<
                                      Block, SampleHeritageBusNoiseIdleBlock>)
                        return prepare_noise(index, block);
                    else
                        return std::isfinite(block.drive) && block.drive >= 0.0f &&
                               block.drive <= 16.0f &&
                               std::isfinite(block.ceiling) &&
                               block.ceiling >= 0.001f && block.ceiling <= 4.0f;
                },
                spec.parameters);
            if (!valid) {
                release();
                return SampleHeritageBusDspStatus::InvalidProfile;
            }
            if (spec.bypass) noise_[index] = {};
        }
        prepared_ = true;
        return SampleHeritageBusDspStatus::Ok;
    }

    void release() noexcept {
        profile_ = {};
        noise_ = {};
        sample_rate_ = 0.0;
        channel_count_ = 0;
        prepared_ = false;
    }

    void reset() noexcept {
        if (!prepared_) return;
        for (std::size_t index = 0; index < profile_.bus_count; ++index) {
            auto& runtime = noise_[index];
            runtime.random_state = runtime.seed;
            runtime.filter_state = {};
        }
    }

    SampleHeritageBusDspStatus process(BufferView<float> buffer,
                                       bool any_voice_active) noexcept {
        return process_impl(buffer, {}, any_voice_active);
    }

    SampleHeritageBusDspStatus process(
        BufferView<float> buffer,
        std::span<const std::uint8_t> voice_activity) noexcept {
        if (voice_activity.size() != buffer.num_samples())
            return SampleHeritageBusDspStatus::InvalidDimensions;
        return process_impl(buffer, voice_activity, false);
    }

private:
    SampleHeritageBusDspStatus process_impl(
        BufferView<float> buffer,
        std::span<const std::uint8_t> voice_activity,
        bool constant_voice_activity) noexcept {
        if (!prepared_) return SampleHeritageBusDspStatus::NotPrepared;
        if (buffer.num_channels() != channel_count_)
            return SampleHeritageBusDspStatus::InvalidDimensions;

        for (std::size_t index = 0; index < profile_.bus_count; ++index) {
            const auto& spec = profile_.bus[index];
            if (spec.bypass) continue;
            std::visit(
                [&](const auto& block) noexcept {
                    using Block = std::decay_t<decltype(block)>;
                    if constexpr (std::is_same_v<
                                      Block, SampleHeritageBusNoiseIdleBlock>)
                        process_noise(index, block, voice_activity,
                                      constant_voice_activity, buffer);
                    else
                        process_output(block, buffer);
                },
                spec.parameters);
        }
        return SampleHeritageBusDspStatus::Ok;
    }

public:
    SampleHeritageRuntimeStateStatus capture_runtime_state(
        SampleHeritageRuntimeEngineState& state) const noexcept {
        state = {};
        if (!prepared_) return SampleHeritageRuntimeStateStatus::NotPrepared;
        for (std::size_t index = 0; index < profile_.bus_count; ++index) {
            const auto& spec = profile_.bus[index];
            if (spec.bypass) continue;
            const auto* block = std::get_if<SampleHeritageBusNoiseIdleBlock>(
                &spec.parameters);
            if (block == nullptr ||
                block->seed_policy !=
                    SampleHeritageSeedPolicy::ContinueSerializedState)
                continue;
            auto& saved = state.rng_states[state.rng_state_count++];
            saved.stage_index = static_cast<std::uint8_t>(index);
            saved.stage_type = SampleHeritageRuntimeRngStageType::Noise;
            saved.random_state = noise_[index].random_state;
        }
        return SampleHeritageRuntimeStateStatus::Ok;
    }

    SampleHeritageRuntimeStateStatus restore_runtime_state(
        const SampleHeritageRuntimeEngineState& state) noexcept {
        if (!prepared_) return SampleHeritageRuntimeStateStatus::NotPrepared;
        if (state.rng_state_count > state.rng_states.size())
            return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
        std::size_t expected = 0;
        for (std::size_t index = 0; index < profile_.bus_count; ++index) {
            const auto& spec = profile_.bus[index];
            if (spec.bypass) continue;
            const auto* block = std::get_if<SampleHeritageBusNoiseIdleBlock>(
                &spec.parameters);
            if (block == nullptr ||
                block->seed_policy !=
                    SampleHeritageSeedPolicy::ContinueSerializedState)
                continue;
            if (expected >= state.rng_state_count)
                return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
            const auto& saved = state.rng_states[expected++];
            if (saved.stage_index != index ||
                saved.stage_type != SampleHeritageRuntimeRngStageType::Noise)
                return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
            if (saved.random_state == 0)
                return SampleHeritageRuntimeStateStatus::InvalidRandomState;
        }
        if (expected != state.rng_state_count)
            return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
        reset();
        for (std::size_t index = 0; index < state.rng_state_count; ++index)
            noise_[state.rng_states[index].stage_index].random_state =
                state.rng_states[index].random_state;
        return SampleHeritageRuntimeStateStatus::Ok;
    }

    double tilt_response_db(std::size_t bus_index,
                            double frequency) const noexcept {
        if (!prepared_ || bus_index >= profile_.bus_count ||
            !(frequency > 0.0) || frequency > sample_rate_ * 0.5 ||
            !std::isfinite(frequency))
            return std::numeric_limits<double>::quiet_NaN();
        const auto& runtime = noise_[bus_index];
        double magnitude = runtime.reference_normalization;
        for (std::size_t shelf = 0; shelf < runtime.shelf_count; ++shelf)
            magnitude *= response_magnitude(runtime.shelves[shelf], sample_rate_,
                                            frequency);
        return 20.0 * std::log10(magnitude);
    }

private:
    static constexpr std::size_t kMaximumTiltShelves = 20;

    struct Biquad {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    struct BiquadState {
        double z1 = 0.0;
        double z2 = 0.0;
    };

    struct NoiseRuntime {
        std::uint64_t seed = 0;
        std::uint64_t random_state = 0;
        std::array<Biquad, kMaximumTiltShelves> shelves{};
        std::array<std::array<BiquadState, kMaximumTiltShelves>,
                   kSampleHeritageMaximumChannels> filter_state{};
        std::size_t shelf_count = 0;
        double reference_normalization = 1.0;
    };

    static std::uint64_t next_random(std::uint64_t& state) noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * UINT64_C(2685821657736338717);
    }

    static float bipolar_random(std::uint64_t& state) noexcept {
        const auto bits = static_cast<std::uint32_t>(next_random(state) >> 40);
        return static_cast<float>(bits) * (2.0f / 16777215.0f) - 1.0f;
    }

    static Biquad high_shelf(double sample_rate,
                             double frequency,
                             double gain_db) noexcept {
        constexpr double pi = 3.14159265358979323846;
        const auto a = std::pow(10.0, gain_db / 40.0);
        const auto omega = 2.0 * pi * frequency / sample_rate;
        const auto cosine = std::cos(omega);
        const auto sine = std::sin(omega);
        const auto alpha = sine * std::sqrt(2.0) * 0.5;
        const auto beta = 2.0 * std::sqrt(a) * alpha;
        const auto a0 = (a + 1.0) - (a - 1.0) * cosine + beta;
        return {
            a * ((a + 1.0) + (a - 1.0) * cosine + beta) / a0,
            -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine) / a0,
            a * ((a + 1.0) + (a - 1.0) * cosine - beta) / a0,
            2.0 * ((a - 1.0) - (a + 1.0) * cosine) / a0,
            ((a + 1.0) - (a - 1.0) * cosine - beta) / a0,
        };
    }

    static double response_magnitude(const Biquad& filter,
                                     double sample_rate,
                                     double frequency) noexcept {
        constexpr double pi = 3.14159265358979323846;
        const auto z = std::polar(1.0, -2.0 * pi * frequency / sample_rate);
        const auto numerator = filter.b0 + filter.b1 * z + filter.b2 * z * z;
        const auto denominator = 1.0 + filter.a1 * z + filter.a2 * z * z;
        return std::abs(numerator / denominator);
    }

    bool prepare_noise(std::size_t index,
                       const SampleHeritageBusNoiseIdleBlock& block) noexcept {
        if (!std::isfinite(block.noise_amplitude) ||
            !std::isfinite(block.idle_amplitude) ||
            !std::isfinite(block.tilt_db_per_octave) ||
            !std::isfinite(block.tilt_floor_hz) ||
            !std::isfinite(block.tilt_reference_hz) ||
            block.noise_amplitude < 0.0f || block.noise_amplitude > 1.0f ||
            block.idle_amplitude < 0.0f || block.idle_amplitude > 1.0f ||
            block.tilt_db_per_octave < -24.0f ||
            block.tilt_db_per_octave > 24.0f || block.tilt_floor_hz < 1.0 ||
            block.tilt_reference_hz < block.tilt_floor_hz ||
            block.tilt_reference_hz >= sample_rate_ * 0.5 ||
            (block.gate != SampleHeritageNoiseGate::AlwaysOn &&
             block.gate != SampleHeritageNoiseGate::VoiceActive) ||
            (block.seed_policy !=
                 SampleHeritageSeedPolicy::RestartFromProfileSeed &&
             block.seed_policy !=
                 SampleHeritageSeedPolicy::ContinueSerializedState) ||
            ((std::max(block.noise_amplitude, block.idle_amplitude) != 0.0f ||
              block.seed_policy ==
                  SampleHeritageSeedPolicy::ContinueSerializedState) &&
             block.seed == 0))
            return false;

        auto& runtime = noise_[index];
        runtime.seed = block.seed;
        runtime.random_state = block.seed;
        if (block.tilt_db_per_octave == 0.0f) return true;

        const auto upper = sample_rate_ * 0.5;
        auto lower = block.tilt_floor_hz;
        while (lower < upper && runtime.shelf_count < runtime.shelves.size()) {
            const auto next = std::min(lower * 2.0, upper);
            const auto octaves = std::log2(next / lower);
            const auto center = std::sqrt(lower * next);
            runtime.shelves[runtime.shelf_count++] = high_shelf(
                sample_rate_, center, block.tilt_db_per_octave * octaves);
            lower = next;
        }
        double reference_gain = 1.0;
        for (std::size_t shelf = 0; shelf < runtime.shelf_count; ++shelf)
            reference_gain *= response_magnitude(
                runtime.shelves[shelf], sample_rate_, block.tilt_reference_hz);
        runtime.reference_normalization = reference_gain > 0.0
            ? 1.0 / reference_gain
            : 1.0;
        return true;
    }

    static float process_biquad(const Biquad& filter,
                                BiquadState& state,
                                float input) noexcept {
        const auto output = filter.b0 * input + state.z1;
        state.z1 = filter.b1 * input - filter.a1 * output + state.z2;
        state.z2 = filter.b2 * input - filter.a2 * output;
        return static_cast<float>(output);
    }

    void process_noise(std::size_t index,
                       const SampleHeritageBusNoiseIdleBlock& block,
                       std::span<const std::uint8_t> voice_activity,
                       bool constant_voice_activity,
                       BufferView<float> buffer) noexcept {
        auto& runtime = noise_[index];
        for (std::size_t frame = 0; frame < buffer.num_samples(); ++frame) {
            const auto voice_active = voice_activity.empty()
                ? constant_voice_activity
                : voice_activity[frame] != 0;
            const auto noise_open =
                block.gate == SampleHeritageNoiseGate::AlwaysOn || voice_active;
            for (std::size_t channel = 0; channel < channel_count_; ++channel) {
                float generated = 0.0f;
                if (block.idle_amplitude != 0.0f)
                    generated += bipolar_random(runtime.random_state) *
                                 block.idle_amplitude;
                if (noise_open && block.noise_amplitude != 0.0f)
                    generated += bipolar_random(runtime.random_state) *
                                 block.noise_amplitude;
                for (std::size_t shelf = 0; shelf < runtime.shelf_count; ++shelf)
                    generated = process_biquad(
                        runtime.shelves[shelf],
                        runtime.filter_state[channel][shelf], generated);
                buffer.channel(channel)[frame] += static_cast<float>(
                    generated * runtime.reference_normalization);
            }
        }
    }

    static void process_output(const SampleHeritageBusOutputDriveBlock& block,
                               BufferView<float> buffer) noexcept {
        for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel)
            for (auto& sample : buffer.channel(channel)) {
                const auto driven = sample * block.drive;
                sample = std::isinf(driven)
                    ? std::copysign(block.ceiling, driven)
                    : block.ceiling * driven /
                          (block.ceiling + std::abs(driven));
            }
    }

    SampleHeritagePreparedProfile profile_{};
    std::array<NoiseRuntime, kSampleHeritageMaximumBusBlocks> noise_{};
    double sample_rate_ = 0.0;
    std::size_t channel_count_ = 0;
    bool prepared_ = false;
};

}  // namespace pulp::audio
