#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_schema.hpp>
#include <pulp/signal/iir_design.hpp>
#include <pulp/signal/ladder_filter.hpp>

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
                 double processing_sample_rate) noexcept {
        return prepare(profile, processing_sample_rate, processing_sample_rate);
    }

    bool prepare(const SampleHeritagePreparedProfile& profile,
                 double machine_sample_rate,
                 double host_sample_rate) noexcept {
        release();
        if (profile.voice_count > runtimes_.size() ||
            !(machine_sample_rate > 0.0) || !std::isfinite(machine_sample_rate) ||
            !(host_sample_rate > 0.0) || !std::isfinite(host_sample_rate))
            return false;
        profile_ = profile;
        machine_sample_rate_ = machine_sample_rate;
        host_sample_rate_ = host_sample_rate;
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
        has_companded_8bit_decode_ = false;
        machine_sample_rate_ = 0.0;
        host_sample_rate_ = 0.0;
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
            for (auto& filter : runtime.analog_filters) filter.reset();
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
        process_range(buffer, 0, profile_.voice_count, false, Frame::Machine);
        process_range(buffer, 0, profile_.voice_count, false, Frame::Host);
    }

    void process_before_live(BufferView<float> buffer,
                             bool draining_tail = false) noexcept {
        process_range(buffer, 0, live_stage_index(), draining_tail,
                      Frame::Machine);
    }

    void process_after_live(BufferView<float> buffer,
                            bool draining_tail = false) noexcept {
        const auto live = live_stage_index();
        process_range(buffer, live < profile_.voice_count ? live + 1 : live,
                      profile_.voice_count, draining_tail, Frame::Machine);
    }

    void process_host_frame(BufferView<float> buffer,
                            bool draining_tail = false) noexcept {
        process_range(buffer, 0, profile_.voice_count, draining_tail,
                      Frame::Host);
    }

    void process_machine_tail(BufferView<float> buffer) noexcept {
        process_range(buffer, 0, profile_.voice_count, true, Frame::Machine);
    }

    void process_tail(BufferView<float> buffer) noexcept {
        process_machine_tail(buffer);
        process_host_frame(buffer, true);
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
    enum class Frame : std::uint8_t { Machine, Host };

    static bool belongs_to_frame(const SampleHeritageVoiceBlockSpec& block,
                                 Frame frame) noexcept {
        const auto host =
            std::holds_alternative<SampleHeritageVoiceReconstructionBlock>(
                block.parameters) ||
            std::holds_alternative<SampleHeritageVoiceAnalogColorBlock>(
                block.parameters);
        return host == (frame == Frame::Host);
    }

    void process_range(BufferView<float> buffer, std::size_t begin,
                       std::size_t end, bool draining_tail, Frame frame) noexcept {
        if (!prepared_) return;
        end = std::min(end, profile_.voice_count);
        for (std::size_t index = begin; index < end; ++index) {
            auto& runtime = runtimes_[index];
            if (runtime.spec.bypass || !belongs_to_frame(runtime.spec, frame) ||
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
        const auto machine = tail_machine_domain_frames();
        const auto host = tail_host_frames();
        return machine > std::numeric_limits<std::uint64_t>::max() - host
            ? std::numeric_limits<std::uint64_t>::max()
            : machine + host;
    }

    std::uint64_t tail_machine_domain_frames() const noexcept {
        return tail_frames(Frame::Machine, machine_sample_rate_);
    }

    std::uint64_t tail_host_frames() const noexcept {
        return tail_frames(Frame::Host, host_sample_rate_);
    }

private:
    std::uint64_t tail_frames(Frame frame, double sample_rate) const noexcept {
        if (!prepared_) return 0;
        long double frames = 0.0L;
        for (std::size_t index = 0; index < profile_.voice_count; ++index) {
            const auto& runtime = runtimes_[index];
            if (runtime.spec.bypass || !belongs_to_frame(runtime.spec, frame))
                continue;
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
                    } else if constexpr (
                        std::is_same_v<Block,
                                       SampleHeritageVoiceAnalogColorBlock>) {
                        auto radius =
                            static_cast<long double>(runtime.analog_decay_pole);
                        if (block.filter_family !=
                                SampleHeritageAnalogFilterFamily::None &&
                            block.mix > 0.0f && radius > 0.0L && radius < 1.0L) {
                            constexpr long double reference_peak = 16.0L;
                            frames += 4.0L * std::ceil(
                                std::log(static_cast<long double>(
                                    kTailSilenceThreshold) / reference_peak) /
                                std::log(radius));
                        }
                    }
                },
                runtime.spec.parameters);
        }
        const auto cap = std::ceil(static_cast<long double>(sample_rate) *
                                   kMaximumTailSeconds);
        return static_cast<std::uint64_t>(std::clamp(frames, 0.0L, cap));
    }

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
        std::array<pulp::signal::LadderFilter,
                   kSampleHeritageMaximumChannels> analog_filters{};
        float analog_decay_pole = 0.0f;
    };

    bool initialize(Runtime& runtime) noexcept {
        if (runtime.spec.bypass) return true;
        return std::visit(
            [&](const auto& block) noexcept {
                using Block = std::decay_t<decltype(block)>;
                if constexpr (std::is_same_v<Block,
                                             SampleHeritageVoiceConverterBlock>) {
                    runtime.random_state = block.seed;
                    has_companded_8bit_decode_ =
                        block.bit_depth == 8.0f &&
                        block.family != SampleHeritageConverterFamily::LinearPcm;
                    if (has_companded_8bit_decode_) {
                        for (std::size_t index = 0;
                             index < companded_8bit_decode_.size(); ++index) {
                            const auto code = static_cast<int>(index) - 128;
                            companded_8bit_decode_[index] = expand(
                                static_cast<float>(code) / 128.0f,
                                block.family);
                        }
                    }
                } else if constexpr (
                    std::is_same_v<Block,
                                   SampleHeritageVoiceReconstructionBlock>) {
                    return initialize_filter(runtime, block);
                } else if constexpr (
                    std::is_same_v<Block,
                                   SampleHeritageVoiceLiveCyclicStretchBlock>) {
                    return true;
                } else if constexpr (
                    std::is_same_v<Block,
                                   SampleHeritageVoiceAnalogColorBlock>) {
                    return initialize_analog_filter(runtime, block);
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

    double resolve_cutoff(
        const SampleHeritageVoiceAnalogColorBlock& block) const noexcept {
        return block.cutoff_law == SampleHeritageCutoffLaw::MachineRateRatio
            ? block.cutoff_value * machine_sample_rate_
            : block.cutoff_value;
    }

    bool initialize_analog_filter(
        Runtime& runtime,
        const SampleHeritageVoiceAnalogColorBlock& block) noexcept {
        if (block.filter_family == SampleHeritageAnalogFilterFamily::None) {
            runtime.analog_decay_pole = 0.0f;
            return true;
        }
        const auto cutoff = resolve_cutoff(block);
        if (!(cutoff >= 1.0 &&
              cutoff <= host_sample_rate_ *
                            kSampleHeritageMaximumLadderCutoffRatio) ||
            !std::isfinite(cutoff) || block.resonance < 0.0f ||
            block.resonance > kSampleHeritageMaximumLadderResonance ||
            !std::isfinite(block.resonance))
            return false;
        for (auto& filter : runtime.analog_filters) {
            filter.set_sample_rate(static_cast<float>(host_sample_rate_));
            filter.set_frequency(static_cast<float>(cutoff));
            filter.set_resonance(block.resonance);
        }
        const auto one_pole_decay = static_cast<float>(std::exp(
            -2.0 * std::numbers::pi * cutoff / host_sample_rate_));
        runtime.analog_decay_pole =
            std::max(one_pole_decay, std::sqrt(block.resonance));
        return runtime.analog_decay_pole >= 0.0f &&
               runtime.analog_decay_pole < 1.0f &&
               std::isfinite(runtime.analog_decay_pole);
    }

    bool initialize_filter(
        Runtime& runtime,
        const SampleHeritageVoiceReconstructionBlock& block) noexcept {
        const auto cutoff = resolve_cutoff(block);
        if (!(cutoff > 0.0 && cutoff < host_sample_rate_ * 0.5) ||
            !std::isfinite(cutoff))
            return false;
        if (block.family == SampleHeritageReconstructionFamily::OnePole) {
            runtime.one_pole_pole = static_cast<float>(std::exp(
                -2.0 * std::numbers::pi * cutoff / host_sample_rate_));
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
                    static_cast<float>(host_sample_rate_));
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
                                     host_sample_rate_);
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

    void process(Runtime& runtime,
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
                const auto use_cached_decode = has_companded_8bit_decode_ &&
                    std::isfinite(code) && code >= -128.0f && code <= 127.0f &&
                    !(code == 0.0f && std::signbit(code));
                const auto decoded = use_cached_decode
                    ? companded_8bit_decode_[
                          static_cast<std::size_t>(static_cast<int>(code) + 128)]
                    : expand(code / levels, block.family);
                sample = apply_dac_nonlinearity(
                    decoded, block.dac_nonlinearity);
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
            auto samples = buffer.channel(channel);
            for (std::size_t section = 0; section < runtime.sos_count; ++section) {
                const auto coefficients = runtime.sos[section];
                auto z1 = runtime.sos_z1[channel][section];
                auto z2 = runtime.sos_z2[channel][section];
                for (auto& sample : samples) {
                    const auto input = sample;
                    const auto output = coefficients.b0 * input + z1;
                    z1 = coefficients.b1 * input - coefficients.a1 * output + z2;
                    z2 = coefficients.b2 * input - coefficients.a2 * output;
                    sample = output;
                }
                runtime.sos_z1[channel][section] = z1;
                runtime.sos_z2[channel][section] = z2;
            }
        }
    }

    static void process(Runtime& runtime,
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
                const auto filtered = block.filter_family ==
                        SampleHeritageAnalogFilterFamily::None
                    ? dry
                    : runtime.analog_filters[channel].process(dry);
                const auto scale =
                    filtered >= 0.0f ? positive_scale : negative_scale;
                const auto shifted = std::tanh(drive * filtered + bias) - center;
                const auto wet = std::abs(scale) > minimum_scale
                    ? static_cast<float>(shifted / scale)
                    : filtered;
                sample = dry + block.mix * (wet - dry);
            }
        }
    }

    SampleHeritagePreparedProfile profile_{};
    std::array<Runtime, kSampleHeritageMaximumVoiceBlocks> runtimes_{};
    std::array<float, 256> companded_8bit_decode_{};
    double machine_sample_rate_ = 0.0;
    double host_sample_rate_ = 0.0;
    bool has_companded_8bit_decode_ = false;
    bool prepared_ = false;
};

}  // namespace pulp::audio
