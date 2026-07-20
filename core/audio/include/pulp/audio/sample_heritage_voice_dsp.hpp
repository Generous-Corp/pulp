#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_schema.hpp>
#include <pulp/signal/iir_design.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>
#include <variant>

namespace pulp::audio {

namespace detail {

inline std::uint64_t heritage_next_random(std::uint64_t& state) noexcept {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717);
}

inline float heritage_bipolar_random(std::uint64_t& state) noexcept {
    const auto bits = static_cast<std::uint32_t>(heritage_next_random(state) >> 40);
    return static_cast<float>(bits) * (2.0f / 16777215.0f) - 1.0f;
}

}  // namespace detail

/// Prepared, fixed-memory processor for the typed per-voice heritage blocks.
class SampleHeritageVoiceDsp {
public:
    static constexpr float kTailSilenceThreshold = 1.0e-6f;
    static constexpr double kMaximumTailSeconds = 10.0;

    bool prepare(const SampleHeritagePreparedProfile& profile,
                 double machine_sample_rate) noexcept {
        release();
        if (profile.voice_count > runtimes_.size() ||
            !(machine_sample_rate > 0.0) || !std::isfinite(machine_sample_rate))
            return false;
        profile_ = profile;
        machine_sample_rate_ = machine_sample_rate;
        for (std::size_t index = 0; index < profile_.voice_count; ++index) {
            auto& runtime = runtimes_[index];
            runtime.spec = profile_.voice[index];
            if (!initialize(runtime)) {
                release();
                return false;
            }
        }
        prepared_ = true;
        reset();
        return true;
    }

    void release() noexcept {
        profile_ = {};
        runtimes_ = {};
        machine_sample_rate_ = 0.0;
        prepared_ = false;
    }

    void reset() noexcept {
        if (!prepared_) return;
        for (std::size_t index = 0; index < profile_.voice_count; ++index) {
            auto& runtime = runtimes_[index];
            runtime.hold_value.fill(0.0f);
            runtime.hold_phase.fill(0);
            runtime.one_pole_state.fill(0.0f);
            runtime.sos_z1 = {};
            runtime.sos_z2 = {};
            if (const auto* converter =
                    std::get_if<SampleHeritageVoiceConverterBlock>(
                        &runtime.spec.parameters))
                runtime.random_state = converter->seed;
        }
    }

    bool prepared() const noexcept { return prepared_; }

    bool converter_continues_state(std::size_t index) const noexcept {
        if (!prepared_ || index >= profile_.voice_count) return false;
        const auto* converter = std::get_if<SampleHeritageVoiceConverterBlock>(
            &runtimes_[index].spec.parameters);
        return converter != nullptr && !runtimes_[index].spec.bypass &&
               converter->seed_policy ==
                   SampleHeritageSeedPolicy::ContinueSerializedState;
    }

    std::uint64_t converter_random_state(std::size_t index) const noexcept {
        return converter_continues_state(index) ? runtimes_[index].random_state : 0;
    }

    bool restore_converter_random_state(std::size_t index,
                                        std::uint64_t state) noexcept {
        if (!converter_continues_state(index) || state == 0) return false;
        runtimes_[index].random_state = state;
        return true;
    }

    void process(BufferView<float> buffer) noexcept {
        process_range(buffer, 0, profile_.voice_count, false);
    }

    void process_before_live(BufferView<float> buffer,
                             bool draining_tail = false) noexcept {
        process_range(buffer, 0, live_stage_index(), draining_tail);
    }

    void process_after_live(BufferView<float> buffer,
                            bool draining_tail = false) noexcept {
        const auto live = live_stage_index();
        process_range(buffer, live < profile_.voice_count ? live + 1 : live,
                      profile_.voice_count, draining_tail);
    }

    void process_tail(BufferView<float> buffer) noexcept {
        process_range(buffer, 0, profile_.voice_count, true);
    }

    std::size_t live_stage_index() const noexcept {
        if (!prepared_) return profile_.voice_count;
        for (std::size_t index = 0; index < profile_.voice_count; ++index) {
            if (!runtimes_[index].spec.bypass &&
                std::holds_alternative<SampleHeritageVoiceLiveCyclicStretchBlock>(
                    runtimes_[index].spec.parameters))
                return index;
        }
        return profile_.voice_count;
    }

    bool live_stage_active() const noexcept {
        return live_stage_index() < profile_.voice_count;
    }

private:
    void process_range(BufferView<float> buffer, std::size_t begin,
                       std::size_t end, bool draining_tail) noexcept {
        if (!prepared_) return;
        end = std::min(end, profile_.voice_count);
        for (std::size_t index = begin; index < end; ++index) {
            auto& runtime = runtimes_[index];
            if (runtime.spec.bypass ||
                (draining_tail &&
                 std::holds_alternative<SampleHeritageVoiceConverterBlock>(
                    runtime.spec.parameters)))
                continue;
            std::visit(
                [&](const auto& block) noexcept { process(runtime, block, buffer); },
                runtime.spec.parameters);
        }
    }

public:

    std::uint64_t tail_machine_frames() const noexcept {
        if (!prepared_) return 0;
        long double frames = 0.0L;
        for (std::size_t index = 0; index < profile_.voice_count; ++index) {
            const auto& runtime = runtimes_[index];
            if (runtime.spec.bypass) continue;
            std::visit(
                [&](const auto& block) noexcept {
                    using Block = std::decay_t<decltype(block)>;
                    if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceHoldDroopBlock>) {
                        frames += block.hold_samples;
                    } else if constexpr (
                        std::is_same_v<Block,
                                       SampleHeritageVoiceReconstructionBlock>) {
                        auto radius = static_cast<long double>(runtime.maximum_pole_radius);
                        if (radius > 0.0L && radius < 1.0L) {
                            constexpr long double reference_peak = 16.0L;
                            const auto decay = std::ceil(
                                std::log(static_cast<long double>(
                                    kTailSilenceThreshold) / reference_peak) /
                                std::log(radius));
                            frames += decay * static_cast<long double>(
                                std::max<std::size_t>(1, runtime.sos_count + 1));
                        }
                    }
                },
                runtime.spec.parameters);
        }
        const auto cap = std::ceil(static_cast<long double>(machine_sample_rate_) *
                                   kMaximumTailSeconds);
        return static_cast<std::uint64_t>(std::clamp(frames, 0.0L, cap));
    }

private:
    static constexpr std::size_t kMaximumSos = 8;

    struct SosCoefficients {
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
    };

    struct Runtime {
        SampleHeritageVoiceBlockSpec spec{};
        std::uint64_t random_state = 0;
        std::array<float, kSampleHeritageMaximumChannels> hold_value{};
        std::array<std::uint32_t, kSampleHeritageMaximumChannels> hold_phase{};
        float one_pole_pole = 0.0f;
        std::array<float, kSampleHeritageMaximumChannels> one_pole_state{};
        std::array<SosCoefficients, kMaximumSos> sos{};
        std::size_t sos_count = 0;
        std::array<std::array<float, kMaximumSos>,
                   kSampleHeritageMaximumChannels> sos_z1{};
        std::array<std::array<float, kMaximumSos>,
                   kSampleHeritageMaximumChannels> sos_z2{};
        float maximum_pole_radius = 0.0f;
    };

    bool initialize(Runtime& runtime) noexcept {
        if (runtime.spec.bypass) return true;
        return std::visit(
            [&](const auto& block) noexcept {
                using Block = std::decay_t<decltype(block)>;
                if constexpr (std::is_same_v<Block,
                                             SampleHeritageVoiceConverterBlock>) {
                    runtime.random_state = block.seed;
                } else if constexpr (
                    std::is_same_v<Block,
                                   SampleHeritageVoiceReconstructionBlock>) {
                    return initialize_filter(runtime, block);
                } else if constexpr (
                    std::is_same_v<Block,
                                   SampleHeritageVoiceLiveCyclicStretchBlock>) {
                    return true;
                }
                return true;
            },
            runtime.spec.parameters);
    }

    double resolve_cutoff(
        const SampleHeritageVoiceReconstructionBlock& block) const noexcept {
        return block.cutoff_law == SampleHeritageCutoffLaw::MachineRateRatio
            ? block.cutoff_value * machine_sample_rate_
            : block.cutoff_value;
    }

    bool initialize_filter(
        Runtime& runtime,
        const SampleHeritageVoiceReconstructionBlock& block) noexcept {
        const auto cutoff = resolve_cutoff(block);
        if (!(cutoff > 0.0 && cutoff < machine_sample_rate_ * 0.5) ||
            !std::isfinite(cutoff))
            return false;
        if (block.family == SampleHeritageReconstructionFamily::OnePole) {
            runtime.one_pole_pole = static_cast<float>(std::exp(
                -2.0 * std::numbers::pi * cutoff / machine_sample_rate_));
            runtime.maximum_pole_radius = runtime.one_pole_pole;
            return true;
        }

        runtime.sos_count = block.order / 2;
        if (runtime.sos_count == 0 || runtime.sos_count > runtime.sos.size())
            return false;
        if (block.family == SampleHeritageReconstructionFamily::Elliptic) {
            try {
                const auto designed = pulp::signal::IirDesign::elliptic_lowpass(
                    block.order, static_cast<float>(cutoff), block.ripple_db,
                    block.stopband_attenuation_db,
                    static_cast<float>(machine_sample_rate_));
                if (designed.size() != runtime.sos_count) return false;
                runtime.maximum_pole_radius = 0.0f;
                for (std::size_t section = 0; section < runtime.sos_count;
                     ++section) {
                    const auto& source = designed[section];
                    auto& destination = runtime.sos[section];
                    destination = {source.b0, source.b1, source.b2,
                                   source.a1, source.a2};
                    const auto roots = std::sqrt(std::complex<double>(
                        source.a1 * source.a1 - 4.0 * source.a2, 0.0));
                    const auto first = (-static_cast<double>(source.a1) + roots) /
                                       2.0;
                    const auto second = (-static_cast<double>(source.a1) - roots) /
                                        2.0;
                    runtime.maximum_pole_radius = std::max(
                        runtime.maximum_pole_radius,
                        static_cast<float>(
                            std::max(std::abs(first), std::abs(second))));
                }
                return runtime.maximum_pole_radius < 1.0f &&
                       std::isfinite(runtime.maximum_pole_radius);
            } catch (...) {
                return false;
            }
        }
        const auto warped = std::tan(std::numbers::pi * cutoff /
                                     machine_sample_rate_);
        const auto epsilon = block.family ==
                                     SampleHeritageReconstructionFamily::Butterworth
            ? 0.0
            : std::sqrt(std::pow(10.0, block.ripple_db / 10.0) - 1.0);
        const auto mu = epsilon > 0.0
            ? std::asinh(1.0 / epsilon) / static_cast<double>(block.order)
            : 0.0;
        runtime.maximum_pole_radius = 0.0f;
        for (std::size_t section = 0; section < runtime.sos_count; ++section) {
            const auto theta = std::numbers::pi *
                (2.0 * static_cast<double>(section) + 1.0) /
                (2.0 * static_cast<double>(block.order));
            const auto real = epsilon == 0.0
                ? -std::sin(theta)
                : -std::sinh(mu) * std::sin(theta);
            const auto imaginary = epsilon == 0.0
                ? std::cos(theta)
                : std::cosh(mu) * std::cos(theta);
            const auto analog_a1 = -2.0 * real;
            const auto analog_a0 = real * real + imaginary * imaginary;
            const auto denominator = 1.0 + analog_a1 * warped +
                                     analog_a0 * warped * warped;
            auto& coefficients = runtime.sos[section];
            coefficients.a1 = static_cast<float>(
                2.0 * (analog_a0 * warped * warped - 1.0) / denominator);
            coefficients.a2 = static_cast<float>(
                (1.0 - analog_a1 * warped + analog_a0 * warped * warped) /
                denominator);

            const auto gain = analog_a0 * warped * warped / denominator;
            coefficients.b0 = static_cast<float>(gain);
            coefficients.b1 = static_cast<float>(2.0 * gain);
            coefficients.b2 = static_cast<float>(gain);

            const auto roots = std::sqrt(std::complex<double>(
                coefficients.a1 * coefficients.a1 -
                    4.0 * coefficients.a2,
                0.0));
            const auto first = (-static_cast<double>(coefficients.a1) + roots) /
                               2.0;
            const auto second = (-static_cast<double>(coefficients.a1) - roots) /
                                2.0;
            runtime.maximum_pole_radius = std::max(
                runtime.maximum_pole_radius,
                static_cast<float>(std::max(std::abs(first), std::abs(second))));
            if (!(runtime.maximum_pole_radius < 1.0f) ||
                !std::isfinite(runtime.maximum_pole_radius))
                return false;
        }
        return true;
    }

    static float compress(float sample,
                          SampleHeritageConverterFamily family) noexcept {
        sample = std::clamp(sample, -1.0f, 1.0f);
        const auto magnitude = std::abs(sample);
        if (family == SampleHeritageConverterFamily::LinearPcm) return sample;
        if (family == SampleHeritageConverterFamily::MuLaw) {
            constexpr float mu = 255.0f;
            return std::copysign(std::log1p(mu * magnitude) / std::log1p(mu),
                                 sample);
        }
        constexpr float a = 87.6f;
        const float denominator = 1.0f + std::log(a);
        const auto compressed = magnitude < 1.0f / a
            ? a * magnitude / denominator
            : (1.0f + std::log(a * magnitude)) / denominator;
        return std::copysign(compressed, sample);
    }

    static float expand(float sample,
                        SampleHeritageConverterFamily family) noexcept {
        sample = std::clamp(sample, -1.0f, 1.0f);
        const auto magnitude = std::abs(sample);
        if (family == SampleHeritageConverterFamily::LinearPcm) return sample;
        if (family == SampleHeritageConverterFamily::MuLaw) {
            constexpr float mu = 255.0f;
            return std::copysign(std::expm1(magnitude * std::log1p(mu)) / mu,
                                 sample);
        }
        constexpr float a = 87.6f;
        const float denominator = 1.0f + std::log(a);
        const auto threshold = 1.0f / denominator;
        const auto expanded = magnitude < threshold
            ? magnitude * denominator / a
            : std::exp(magnitude * denominator - 1.0f) / a;
        return std::copysign(expanded, sample);
    }

    static float apply_dac_nonlinearity(float sample, float amount) noexcept {
        const auto bounded = std::clamp(sample, -1.0f, 1.0f);
        return bounded + amount * bounded * (1.0f - std::abs(bounded));
    }

    static void process(Runtime&,
                        const SampleHeritageVoiceMachineDomainBlock&,
                        BufferView<float>) noexcept {}
    static void process(Runtime&,
                        const SampleHeritageVoiceClockBlock&,
                        BufferView<float>) noexcept {}
    static void process(Runtime&,
                        const SampleHeritageVoicePitchBlock&,
                        BufferView<float>) noexcept {}
    static void process(Runtime&,
                        const SampleHeritageVoiceLiveCyclicStretchBlock&,
                        BufferView<float>) noexcept {}

    static void process(Runtime& runtime,
                        const SampleHeritageVoiceConverterBlock& block,
                        BufferView<float> buffer) noexcept {
        const auto levels = std::exp2(block.bit_depth - 1.0f);
        const auto minimum_code = -std::floor(levels);
        const auto maximum_code = std::floor(levels - 1.0f);
        for (std::size_t frame = 0; frame < buffer.num_samples(); ++frame) {
            for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
                auto& sample = buffer.channel(channel)[frame];
                const auto encoded = compress(sample, block.family);
                // Dither is expressed in quantizer LSBs and is applied in the
                // compressed code domain for every converter family.
                const auto dither = block.dither_lsb == 0.0f
                    ? 0.0f
                    : detail::heritage_bipolar_random(runtime.random_state) *
                          block.dither_lsb;
                const auto code = std::clamp(std::round(encoded * levels + dither),
                                             minimum_code, maximum_code);
                sample = apply_dac_nonlinearity(
                    expand(code / levels, block.family), block.dac_nonlinearity);
            }
        }
    }

    static void process(Runtime& runtime,
                        const SampleHeritageVoiceHoldDroopBlock& block,
                        BufferView<float> buffer) noexcept {
        const auto retention = 1.0f - block.droop;
        for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
            auto& held = runtime.hold_value[channel];
            auto& phase = runtime.hold_phase[channel];
            for (auto& sample : buffer.channel(channel)) {
                if (phase == 0) held = sample;
                sample = held;
                held *= retention;
                phase = (phase + 1) % block.hold_samples;
            }
        }
    }

    static void process(Runtime& runtime,
                        const SampleHeritageVoiceReconstructionBlock& block,
                        BufferView<float> buffer) noexcept {
        if (block.family == SampleHeritageReconstructionFamily::OnePole) {
            const auto pole = runtime.one_pole_pole;
            for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
                auto state = runtime.one_pole_state[channel];
                for (auto& sample : buffer.channel(channel)) {
                    state = (1.0f - pole) * sample + pole * state;
                    sample = state;
                }
                runtime.one_pole_state[channel] = state;
            }
            return;
        }
        for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
            for (auto& sample : buffer.channel(channel)) {
                auto value = sample;
                for (std::size_t section = 0; section < runtime.sos_count; ++section) {
                    const auto& coefficients = runtime.sos[section];
                    const auto output = coefficients.b0 * value +
                                        runtime.sos_z1[channel][section];
                    runtime.sos_z1[channel][section] =
                        coefficients.b1 * value - coefficients.a1 * output +
                        runtime.sos_z2[channel][section];
                    runtime.sos_z2[channel][section] =
                        coefficients.b2 * value - coefficients.a2 * output;
                    value = output;
                }
                sample = value;
            }
        }
    }

    static void process(Runtime&,
                        const SampleHeritageVoiceAnalogColorBlock& block,
                        BufferView<float> buffer) noexcept {
        if (block.mix == 0.0f) return;
        const auto bias = 1.5 * static_cast<double>(block.asymmetry);
        const auto center = std::tanh(bias);
        const auto drive = static_cast<double>(block.drive);
        const auto positive_scale = std::tanh(drive + bias) - center;
        const auto negative_scale = center - std::tanh(bias - drive);
        constexpr auto minimum_scale =
            16.0 * std::numeric_limits<double>::epsilon();
        for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
            for (auto& sample : buffer.channel(channel)) {
                const auto dry = sample;
                const auto scale = dry >= 0.0f ? positive_scale : negative_scale;
                const auto shifted = std::tanh(drive * dry + bias) - center;
                const auto wet = std::abs(scale) > minimum_scale
                    ? static_cast<float>(shifted / scale)
                    : dry;
                sample = dry + block.mix * (wet - dry);
            }
        }
    }

    SampleHeritagePreparedProfile profile_{};
    std::array<Runtime, kSampleHeritageMaximumVoiceBlocks> runtimes_{};
    double machine_sample_rate_ = 0.0;
    bool prepared_ = false;
};

}  // namespace pulp::audio
