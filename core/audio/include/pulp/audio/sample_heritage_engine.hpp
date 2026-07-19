#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_runtime_state.hpp>
#include <pulp/audio/sample_heritage_src.hpp>
#include <pulp/audio/sample_heritage_voice_dsp.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::audio {

namespace detail {

inline std::uint64_t next_random(std::uint64_t& state) noexcept {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * UINT64_C(2685821657736338717);
}

inline float bipolar_random(std::uint64_t& state) noexcept {
    const auto bits = static_cast<std::uint32_t>(next_random(state) >> 40);
    return static_cast<float>(bits) * (2.0f / 16777215.0f) - 1.0f;
}

}  // namespace detail

enum class SampleHeritagePrepareStatus : std::uint8_t {
    Ok,
    InvalidProfile,
    InvalidDimensions,
    UnsupportedRateConversion,
    SizeOverflow,
    AllocationFailed,
    KernelBuildFailed,
};

struct SampleHeritagePrepareConfig {
    SampleHeritagePreparedProfile profile{};
    std::size_t channel_count = 0;
    std::size_t maximum_output_frames = 0;
    // The view is copied during prepare. Its issuing bank must retain the
    // coefficient storage until the prepared engine is released.
    const SampleSincKernelBankView* external_sinc_bank = nullptr;
    double maximum_runtime_clock_factor = 1.0;
};

enum class SampleHeritagePlanStatus : std::uint8_t {
    Ok,
    NotPrepared,
    OutputTooLarge,
    SizeOverflow,
};

struct SampleHeritageProcessPlan {
    SampleHeritagePlanStatus status = SampleHeritagePlanStatus::NotPrepared;
    std::uint64_t prepare_epoch = 0;
    std::uint64_t sequence = 0;
    std::size_t output_frames = 0;
    std::size_t machine_frames = 0;
    std::size_t input_frames = 0;
    double runtime_clock_multiplier = 1.0;

    bool valid() const noexcept { return status == SampleHeritagePlanStatus::Ok; }
};

enum class SampleHeritageProcessStatus : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidPlan,
    InvalidDimensions,
    InputFrameMismatch,
    TailInputNotSilent,
    InternalContractFailure,
};

class SampleHeritageEngine {
public:
    /// Compatibility preparation for fixed-rate profiles. Rate-changing
    /// profiles require the checked exact-output configuration overload.
    bool prepare(const SampleHeritagePreparedProfile& profile) noexcept {
        return prepare_impl(profile, 0, 0, false, nullptr) ==
               SampleHeritagePrepareStatus::Ok;
    }

    SampleHeritagePrepareStatus prepare(
        const SampleHeritagePrepareConfig& config) noexcept {
        return prepare_impl(config.profile, config.channel_count,
                            config.maximum_output_frames, true,
                            config.external_sinc_bank,
                            config.maximum_runtime_clock_factor);
    }

    void release() noexcept {
        profile_ = {};
        stages_ = {};
        input_src_.release();
        return_src_.release();
        voice_dsp_.release();
        sinc_bank_.release();
        std::vector<float>().swap(machine_scratch_);
        std::vector<float>().swap(dynamic_identity_delay_);
        machine_ptrs_.fill(nullptr);
        machine_const_ptrs_.fill(nullptr);
        prepared_ = false;
        exact_prepared_ = false;
        all_bypassed_ = false;
        typed_voice_ = false;
        runtime_dynamic_clock_ = false;
        dynamic_identity_exact_ = false;
        dynamic_identity_epoch_ = false;
        dynamic_identity_delay_frames_ = 0;
        dynamic_identity_delay_head_ = 0;
        dynamic_return_latency_output_frames_ = 0.0;
        input_identity_ = true;
        return_identity_ = true;
        channel_count_ = 0;
        maximum_output_frames_ = 0;
        maximum_input_frames_ = 0;
        maximum_machine_frames_ = 0;
        maximum_runtime_clock_factor_ = 1.0;
        machine_sample_rate_ = 0.0;
        clock_ratio_ = 1.0;
        clocked_sample_rate_ = 0.0;
        input_ratio_ = 1.0;
        return_ratio_ = 1.0;
        process_sequence_ = 0;
        ++prepare_epoch_;
        if (prepare_epoch_ == 0) ++prepare_epoch_;
    }

    void reset() noexcept {
        if (!prepared_) return;
        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            auto& runtime = stages_[index];
            runtime.filter_state.fill(0.0f);
            runtime.hold_value.fill(0.0f);
            runtime.hold_phase.fill(0);
            std::visit([&](const auto& stage) noexcept {
                using Stage = std::decay_t<decltype(stage)>;
                if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage> ||
                              std::is_same_v<Stage, SampleHeritageQuantizationStage>) {
                    if (stage.seed_policy ==
                        SampleHeritageSeedPolicy::RestartFromProfileSeed)
                        runtime.random_state = stage.seed;
                }
            }, runtime.spec.parameters);
        }
        input_src_.reset();
        return_src_.reset();
        voice_dsp_.reset();
        std::fill(dynamic_identity_delay_.begin(),
                  dynamic_identity_delay_.end(), 0.0f);
        dynamic_identity_delay_head_ = 0;
        dynamic_identity_epoch_ = dynamic_identity_exact_;
        process_sequence_ = 0;
        ++prepare_epoch_;
        if (prepare_epoch_ == 0) ++prepare_epoch_;
    }

    bool prepared() const noexcept { return prepared_; }
    bool exact_processing_prepared() const noexcept { return exact_prepared_; }
    std::size_t maximum_input_frames() const noexcept {
        return maximum_input_frames_;
    }
    std::size_t maximum_machine_frames() const noexcept {
        return maximum_machine_frames_;
    }
    double machine_sample_rate() const noexcept { return machine_sample_rate_; }
    double clock_ratio() const noexcept { return clock_ratio_; }
    double clocked_sample_rate() const noexcept { return clocked_sample_rate_; }

    double latency_output_frames() const noexcept {
        return latency_output_frames(1.0);
    }

    double latency_output_frames(double runtime_clock_multiplier) const noexcept {
        if (!exact_prepared_) return 0.0;
        if (!valid_runtime_clock_multiplier(runtime_clock_multiplier)) return 0.0;
        if (all_bypassed_ && !runtime_dynamic_clock_) return 0.0;
        constexpr double half =
            static_cast<double>(kHighQualitySampleSincHalfWidth);
        const auto input_latency = input_identity_ ? 0.0 : half / clock_ratio_;
        const auto effective_return_ratio =
            return_ratio_ * runtime_clock_multiplier;
        const auto return_latency = runtime_dynamic_clock_
            ? dynamic_return_latency_output_frames_
            : (return_identity_ ? 0.0 : half / effective_return_ratio);
        return input_latency + return_latency;
    }

    /// Host-rate process_tail_exact() frames sufficient to flush every finite
    /// SRC, hold, and filter state for the prepared profile.
    std::uint64_t tail_output_frames() const noexcept {
        if (!exact_prepared_ || (all_bypassed_ && !runtime_dynamic_clock_)) return 0;

        long double machine_frames = typed_voice_
            ? static_cast<long double>(voice_dsp_.tail_machine_frames())
            : 0.0L;
        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            const auto& runtime = stages_[index];
            if (runtime.spec.bypass) continue;
            std::visit(
                [&](const auto& stage) noexcept {
                    using Stage = std::decay_t<decltype(stage)>;
                    if constexpr (std::is_same_v<Stage,
                                                 SampleHeritageDacHoldStage>) {
                        machine_frames += stage.hold_samples;
                    } else if constexpr (
                        std::is_same_v<Stage,
                                       SampleHeritageReconstructionFilterStage>) {
                        const auto pole = static_cast<long double>(runtime.filter_pole);
                        if (pole > 0.0L && pole < 1.0L) {
                            constexpr long double reference_peak = 16.0L;
                            machine_frames += std::ceil(
                                std::log(static_cast<long double>(
                                    SampleHeritageVoiceDsp::kTailSilenceThreshold) /
                                         reference_peak) /
                                std::log(pole));
                        }
                    }
                },
                runtime.spec.parameters);
        }
        machine_frames = std::min(
            machine_frames,
            std::ceil(static_cast<long double>(machine_sample_rate_) *
                      SampleHeritageVoiceDsp::kMaximumTailSeconds));

        const auto required_frames =
            static_cast<long double>(latency_output_frames()) +
            machine_frames / static_cast<long double>(
                return_ratio_ / maximum_runtime_clock_factor_);
        const auto output_frames = std::ceil(required_frames);
        if (!std::isfinite(output_frames) || output_frames <= 0.0L)
            return output_frames <= 0.0L
                ? 0
                : std::numeric_limits<std::uint64_t>::max();
        if (output_frames >=
            static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            return std::numeric_limits<std::uint64_t>::max();
        return static_cast<std::uint64_t>(output_frames);
    }

    /// Flat fixed-rate in-place processing for the internal engine
    /// representation when active machine and clock rates are host identity.
    bool process(BufferView<float> buffer) noexcept {
        if (!prepared_ || exact_prepared_ || !input_identity_ || !return_identity_ ||
            buffer.num_channels() == 0 ||
            buffer.num_channels() > kSampleHeritageMaximumChannels) {
            return false;
        }
        process_machine_stages(buffer);
        return true;
    }

    SampleHeritageProcessPlan plan_exact(std::size_t output_frames) const noexcept {
        return plan_exact(output_frames, 1.0);
    }

    SampleHeritageProcessPlan plan_exact(
        std::size_t output_frames,
        double runtime_clock_multiplier) const noexcept {
        SampleHeritageProcessPlan result;
        result.prepare_epoch = prepare_epoch_;
        result.sequence = process_sequence_;
        result.output_frames = output_frames;
        result.runtime_clock_multiplier = runtime_clock_multiplier;
        if (!exact_prepared_) return result;
        if (!valid_runtime_clock_multiplier(runtime_clock_multiplier)) {
            result.status = SampleHeritagePlanStatus::SizeOverflow;
            return result;
        }
        if (output_frames > maximum_output_frames_) {
            result.status = SampleHeritagePlanStatus::OutputTooLarge;
            return result;
        }
        if (all_bypassed_ && !runtime_dynamic_clock_) {
            result.status = SampleHeritagePlanStatus::Ok;
            result.input_frames = output_frames;
            return result;
        }
        const auto return_plan = return_src_.plan(
            output_frames, return_ratio_ * runtime_clock_multiplier);
        if (!return_plan.valid()) {
            result.status = SampleHeritagePlanStatus::SizeOverflow;
            return result;
        }
        result.machine_frames = return_plan.input_frames;
        const auto input_plan = input_src_.plan(result.machine_frames);
        if (!input_plan.valid() || result.machine_frames > maximum_machine_frames_ ||
            input_plan.input_frames > maximum_input_frames_) {
            result.status = SampleHeritagePlanStatus::SizeOverflow;
            return result;
        }
        result.input_frames = input_plan.input_frames;
        result.status = SampleHeritagePlanStatus::Ok;
        return result;
    }

    SampleHeritageProcessStatus process_exact(
        const SampleHeritageProcessPlan& plan,
        const BufferView<const float>& input,
        BufferView<float> output) noexcept {
        return process_exact_impl(plan, input, output, false);
    }

    /// Advances finite SRC, hold, and filter memory without exciting noise or
    /// dither stages. The exact planned input must contain only zero samples.
    SampleHeritageProcessStatus process_tail_exact(
        const SampleHeritageProcessPlan& plan,
        const BufferView<const float>& input,
        BufferView<float> output) noexcept {
        for (std::size_t channel = 0; channel < input.num_channels(); ++channel)
            for (const auto sample : input.channel(channel))
                if (sample != 0.0f)
                    return SampleHeritageProcessStatus::TailInputNotSilent;
        return process_exact_impl(plan, input, output, true);
    }

private:
    SampleHeritageProcessStatus process_exact_impl(
        const SampleHeritageProcessPlan& plan,
        const BufferView<const float>& input,
        BufferView<float> output,
        bool draining_tail) noexcept {
        if (!exact_prepared_) return SampleHeritageProcessStatus::NotPrepared;
        const auto expected = plan_exact(output.num_samples(),
                                         plan.runtime_clock_multiplier);
        if (!plan.valid() || !expected.valid() ||
            plan.prepare_epoch != expected.prepare_epoch ||
            plan.sequence != expected.sequence ||
            plan.output_frames != expected.output_frames ||
            plan.machine_frames != expected.machine_frames ||
            plan.input_frames != expected.input_frames ||
            plan.runtime_clock_multiplier != expected.runtime_clock_multiplier) {
            return SampleHeritageProcessStatus::InvalidPlan;
        }
        if (input.num_channels() != channel_count_ ||
            output.num_channels() != channel_count_) {
            return SampleHeritageProcessStatus::InvalidDimensions;
        }
        if (input.num_samples() != plan.input_frames)
            return SampleHeritageProcessStatus::InputFrameMismatch;

        if (all_bypassed_ && !runtime_dynamic_clock_) {
            for (std::size_t channel = 0; channel < channel_count_; ++channel)
                std::copy(input.channel(channel).begin(), input.channel(channel).end(),
                          output.channel(channel).begin());
            ++process_sequence_;
            return SampleHeritageProcessStatus::Ok;
        }

        BufferView<float> machine(machine_ptrs_.data(), channel_count_,
                                  plan.machine_frames);
        if (input_src_.process(input, machine) != SampleHeritageSrcStatus::Ok)
            return SampleHeritageProcessStatus::InternalContractFailure;
        process_machine_stages(machine, draining_tail);
        BufferView<const float> machine_input(machine_const_ptrs_.data(),
                                              channel_count_, plan.machine_frames);
        if (return_src_.process(machine_input, output,
                                return_ratio_ * plan.runtime_clock_multiplier) !=
            SampleHeritageSrcStatus::Ok)
            return SampleHeritageProcessStatus::InternalContractFailure;
        if (dynamic_identity_exact_) {
            if (dynamic_identity_epoch_ &&
                plan.runtime_clock_multiplier == 1.0 &&
                input.num_samples() == output.num_samples()) {
                process_dynamic_identity_delay(input, output);
            } else if (plan.runtime_clock_multiplier != 1.0) {
                dynamic_identity_epoch_ = false;
            }
        }
        ++process_sequence_;
        return SampleHeritageProcessStatus::Ok;
    }

public:

    /// Captures only ContinueSerializedState RNG streams. SRC, hold, and filter
    /// transients are deliberately outside the serialized contract.
    SampleHeritageRuntimeStateCapture capture_runtime_state() const noexcept {
        SampleHeritageRuntimeStateCapture result;
        if (!prepared_) return result;
        result.state.profile_schema_version = profile_.schema_version;
        std::copy(profile_.profile_id.begin(), profile_.profile_id.end(),
                  result.state.profile_id.begin());
        result.state.profile_digest = profile_.profile_digest;
        if (typed_voice_) {
            for (std::size_t index = 0; index < profile_.voice_count; ++index) {
                if (!voice_dsp_.converter_continues_state(index)) continue;
                auto& saved = result.state.rng_states[
                    result.state.rng_state_count++];
                saved.stage_index = static_cast<std::uint8_t>(index);
                saved.stage_type =
                    SampleHeritageRuntimeRngStageType::Quantization;
                saved.random_state = voice_dsp_.converter_random_state(index);
            }
            result.status = SampleHeritageRuntimeStateStatus::Ok;
            return result;
        }
        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            const auto& runtime = stages_[index];
            std::visit(
                [&](const auto& stage) noexcept {
                    using Stage = std::decay_t<decltype(stage)>;
                    if constexpr (std::is_same_v<Stage,
                                                 SampleHeritageQuantizationStage> ||
                                  std::is_same_v<Stage,
                                                 SampleHeritageNoiseStage>) {
                        if (stage.seed_policy !=
                            SampleHeritageSeedPolicy::ContinueSerializedState)
                            return;
                        auto& saved = result.state.rng_states[
                            result.state.rng_state_count++];
                        saved.stage_index = static_cast<std::uint8_t>(index);
                        saved.stage_type =
                            std::is_same_v<Stage, SampleHeritageQuantizationStage>
                                ? SampleHeritageRuntimeRngStageType::Quantization
                                : SampleHeritageRuntimeRngStageType::Noise;
                        saved.random_state = runtime.random_state;
                    }
                },
                runtime.spec.parameters);
        }
        result.status = SampleHeritageRuntimeStateStatus::Ok;
        return result;
    }

    /// Restores RNG-stream continuation after resetting every DSP transient;
    /// this is not sample-exact whole-engine continuation.
    SampleHeritageRuntimeStateStatus restore_runtime_state(
        const SampleHeritageRuntimeState& state) noexcept {
        if (!prepared_) return SampleHeritageRuntimeStateStatus::NotPrepared;
        if (state.schema_version != kSampleHeritageRuntimeStateSchemaVersion)
            return SampleHeritageRuntimeStateStatus::UnsupportedSchemaVersion;
        if (state.profile_schema_version != profile_.schema_version ||
            state.bound_profile_id() != profile_.id() ||
            state.profile_digest_version != kSampleHeritageProfileDigestVersion ||
            state.profile_digest != profile_.profile_digest)
            return SampleHeritageRuntimeStateStatus::ProfileMismatch;

        if (typed_voice_) {
            std::size_t expected_count = 0;
            for (std::size_t index = 0; index < profile_.voice_count; ++index) {
                if (!voice_dsp_.converter_continues_state(index)) continue;
                if (expected_count >= state.rng_state_count)
                    return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
                const auto& saved = state.rng_states[expected_count];
                if (saved.stage_index != index ||
                    saved.stage_type !=
                        SampleHeritageRuntimeRngStageType::Quantization ||
                    saved.random_state == 0)
                    return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
                ++expected_count;
            }
            if (expected_count != state.rng_state_count ||
                state.rng_state_count > state.rng_states.size())
                return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
            reset();
            for (std::size_t index = 0; index < state.rng_state_count; ++index) {
                const auto& saved = state.rng_states[index];
                if (!voice_dsp_.restore_converter_random_state(
                        saved.stage_index, saved.random_state))
                    return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
            }
            return SampleHeritageRuntimeStateStatus::Ok;
        }

        std::size_t expected_count = 0;
        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            const auto expected = std::visit(
                [](const auto& stage) noexcept
                    -> std::pair<bool, SampleHeritageRuntimeRngStageType> {
                    using Stage = std::decay_t<decltype(stage)>;
                    if constexpr (std::is_same_v<Stage,
                                                 SampleHeritageQuantizationStage>)
                        return {stage.seed_policy ==
                                    SampleHeritageSeedPolicy::ContinueSerializedState,
                                SampleHeritageRuntimeRngStageType::Quantization};
                    if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage>)
                        return {stage.seed_policy ==
                                    SampleHeritageSeedPolicy::ContinueSerializedState,
                                SampleHeritageRuntimeRngStageType::Noise};
                    return {false, SampleHeritageRuntimeRngStageType::Quantization};
                },
                stages_[index].spec.parameters);
            if (!expected.first) continue;
            if (expected_count >= state.rng_state_count)
                return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
            const auto& saved = state.rng_states[expected_count];
            if (saved.stage_index != index || saved.stage_type != expected.second)
                return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
            if (saved.random_state == 0)
                return SampleHeritageRuntimeStateStatus::InvalidRandomState;
            ++expected_count;
        }
        if (expected_count != state.rng_state_count ||
            state.rng_state_count > state.rng_states.size())
            return SampleHeritageRuntimeStateStatus::InvalidStageLayout;

        reset();
        for (std::size_t index = 0; index < state.rng_state_count; ++index) {
            const auto& saved = state.rng_states[index];
            stages_[saved.stage_index].random_state = saved.random_state;
        }
        return SampleHeritageRuntimeStateStatus::Ok;
    }

private:
    struct RuntimeStage {
        SampleHeritageStageSpec spec{};
        std::uint64_t random_state = 0;
        float filter_pole = 0.0f;
        std::array<float, kSampleHeritageMaximumChannels> filter_state{};
        std::array<float, kSampleHeritageMaximumChannels> hold_value{};
        std::array<std::uint32_t, kSampleHeritageMaximumChannels> hold_phase{};
    };

    SampleHeritagePrepareStatus prepare_impl(
        const SampleHeritagePreparedProfile& profile,
        std::size_t channel_count,
        std::size_t maximum_output_frames,
        bool exact,
        const SampleSincKernelBankView* external_sinc_bank,
        double maximum_runtime_clock_factor = 1.0) noexcept {
        release();
        if (profile.schema_version != kSampleHeritageProfileSchemaVersion ||
            profile.stage_count > stages_.size() ||
            !(profile.host_sample_rate > 0.0) ||
            !std::isfinite(profile.host_sample_rate)) {
            return SampleHeritagePrepareStatus::InvalidProfile;
        }
        if (exact && (channel_count == 0 ||
                      channel_count > kSampleHeritageMaximumChannels ||
                      maximum_output_frames == 0 ||
                      !(maximum_runtime_clock_factor >= 1.0) ||
                      maximum_runtime_clock_factor > 64.0 ||
                      !std::isfinite(maximum_runtime_clock_factor))) {
            return SampleHeritagePrepareStatus::InvalidDimensions;
        }
        try {
            SampleHeritageProfile authoring;
            authoring.schema_version = profile.schema_version;
            authoring.profile_id = std::string(profile.id());
            authoring.host_sample_rate = profile.host_sample_rate;
            if (profile.voice_count != 0 || profile.bus_count != 0 ||
                profile.record_commit_count != 0) {
                authoring.voice.assign(
                    profile.voice.begin(),
                    profile.voice.begin() +
                        static_cast<std::ptrdiff_t>(profile.voice_count));
                authoring.bus.assign(
                    profile.bus.begin(),
                    profile.bus.begin() +
                        static_cast<std::ptrdiff_t>(profile.bus_count));
                authoring.record_commit.assign(
                    profile.record_commit.begin(),
                    profile.record_commit.begin() +
                        static_cast<std::ptrdiff_t>(profile.record_commit_count));
            } else {
                authoring.stages.assign(
                    profile.stages.begin(),
                    profile.stages.begin() +
                        static_cast<std::ptrdiff_t>(profile.stage_count));
            }
            const auto validated = validate_sample_heritage_profile(authoring);
            if (!validated.valid() ||
                validated.profile.profile_digest != profile.profile_digest)
                return SampleHeritagePrepareStatus::InvalidProfile;
            profile_ = validated.profile;
        } catch (...) {
            return SampleHeritagePrepareStatus::AllocationFailed;
        }

        machine_sample_rate_ = profile_.host_sample_rate;
        clock_ratio_ = 1.0;
        all_bypassed_ = true;
        typed_voice_ = profile_.voice_count != 0;
        if (typed_voice_) {
            for (std::size_t index = 0; index < profile_.voice_count; ++index) {
                const auto& spec = profile_.voice[index];
                all_bypassed_ = all_bypassed_ && spec.bypass;
                if (spec.bypass) continue;
                if (const auto* machine =
                        std::get_if<SampleHeritageVoiceMachineDomainBlock>(
                            &spec.parameters))
                    machine_sample_rate_ = machine->sample_rate;
                else if (const auto* clock =
                             std::get_if<SampleHeritageVoiceClockBlock>(
                                 &spec.parameters))
                    clock_ratio_ = clock->ratio;
            }
        } else {
            for (std::size_t index = 0; index < profile_.stage_count; ++index) {
                const auto& spec = profile_.stages[index];
                all_bypassed_ = all_bypassed_ && spec.bypass;
                if (spec.bypass) continue;
                if (const auto* machine =
                        std::get_if<SampleHeritageMachineDomainStage>(
                            &spec.parameters))
                    machine_sample_rate_ = machine->sample_rate;
                else if (const auto* clock =
                             std::get_if<SampleHeritageClockPitchStage>(
                                 &spec.parameters))
                    clock_ratio_ = clock->ratio;
            }
        }
        clocked_sample_rate_ = machine_sample_rate_ * clock_ratio_;
        input_ratio_ = profile_.host_sample_rate / machine_sample_rate_;
        return_ratio_ = clocked_sample_rate_ / profile_.host_sample_rate;
        input_identity_ = machine_sample_rate_ == profile_.host_sample_rate;
        return_identity_ = clocked_sample_rate_ == profile_.host_sample_rate;
        if ((!input_identity_ && input_ratio_ > 128.0) ||
            (!return_identity_ && return_ratio_ > 128.0) ||
            !std::isfinite(input_ratio_) || !std::isfinite(return_ratio_)) {
            release();
            return SampleHeritagePrepareStatus::UnsupportedRateConversion;
        }
        if (!exact && (!input_identity_ || !return_identity_)) {
            release();
            return SampleHeritagePrepareStatus::UnsupportedRateConversion;
        }

        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            stages_[index].spec = profile_.stages[index];
            initialize_seed(stages_[index]);
            if (const auto* filter =
                    std::get_if<SampleHeritageReconstructionFilterStage>(
                        &stages_[index].spec.parameters)) {
                stages_[index].filter_pole = static_cast<float>(
                    std::exp(-2.0 * 3.14159265358979323846 * filter->cutoff_hz /
                             clocked_sample_rate_));
            }
        }
        if (typed_voice_ && !voice_dsp_.prepare(profile_, machine_sample_rate_)) {
            release();
            return SampleHeritagePrepareStatus::InvalidProfile;
        }

        if (exact) {
            runtime_dynamic_clock_ = maximum_runtime_clock_factor > 1.0;
            const auto maximum_return_ratio =
                return_ratio_ * maximum_runtime_clock_factor;
            const auto maximum_ratio = std::max(input_ratio_, maximum_return_ratio);
            SampleSincKernelBankView bank;
            if (external_sinc_bank != nullptr) {
                bank = *external_sinc_bank;
                if (!bank.valid()) {
                    release();
                    return SampleHeritagePrepareStatus::KernelBuildFailed;
                }
            } else if (!input_identity_ || !return_identity_ ||
                       runtime_dynamic_clock_) {
                if (!sinc_bank_.build_dense_for_maximum_consumption(
                        maximum_ratio)) {
                    release();
                    return SampleHeritagePrepareStatus::KernelBuildFailed;
                }
                bank = sinc_bank_.view();
            }
            const auto input_selection = input_identity_
                ? SampleSincKernelSelection{}
                : bank.select(input_ratio_);
            const auto return_selection = return_identity_
                ? SampleSincKernelSelection{}
                : bank.select(return_ratio_);
            if ((!input_identity_ && !input_selection.valid()) ||
                (!return_identity_ && !return_selection.valid())) {
                release();
                return SampleHeritagePrepareStatus::KernelBuildFailed;
            }
            const auto input_status = input_src_.prepare(
                input_ratio_, channel_count, input_selection, input_identity_);
            const auto return_status = maximum_runtime_clock_factor == 1.0
                ? return_src_.prepare(return_ratio_, channel_count,
                                      return_selection, return_identity_)
                : return_src_.prepare_variable(return_ratio_,
                                               return_ratio_ /
                                                   maximum_runtime_clock_factor,
                                               maximum_return_ratio,
                                               channel_count, bank);
            if (input_status != SampleHeritageSrcStatus::Ok ||
                return_status != SampleHeritageSrcStatus::Ok) {
                const auto allocation_failed =
                    input_status == SampleHeritageSrcStatus::AllocationFailed ||
                    return_status == SampleHeritageSrcStatus::AllocationFailed;
                const auto overflow =
                    input_status == SampleHeritageSrcStatus::SizeOverflow ||
                    return_status == SampleHeritageSrcStatus::SizeOverflow;
                release();
                return allocation_failed
                    ? SampleHeritagePrepareStatus::AllocationFailed
                    : (overflow ? SampleHeritagePrepareStatus::SizeOverflow
                                : SampleHeritagePrepareStatus::UnsupportedRateConversion);
            }

            if (!checked_frame_bound(maximum_output_frames,
                                     maximum_return_ratio,
                                     return_identity_ &&
                                         maximum_runtime_clock_factor == 1.0,
                                     maximum_machine_frames_) ||
                !checked_frame_bound(maximum_machine_frames_, input_ratio_,
                                     input_identity_, maximum_input_frames_) ||
                (channel_count != 0 &&
                 maximum_machine_frames_ >
                     std::numeric_limits<std::size_t>::max() / channel_count)) {
                release();
                return SampleHeritagePrepareStatus::SizeOverflow;
            }
            try {
                machine_scratch_.assign(channel_count * maximum_machine_frames_, 0.0f);
                dynamic_identity_exact_ = runtime_dynamic_clock_ &&
                                          runtime_clock_identity_only() &&
                                          input_identity_ && return_ratio_ == 1.0;
                dynamic_return_latency_output_frames_ = runtime_dynamic_clock_
                    ? std::ceil(static_cast<double>(
                          kHighQualitySampleSincHalfWidth) /
                                (return_ratio_ /
                                 maximum_runtime_clock_factor))
                    : 0.0;
                dynamic_identity_delay_frames_ = dynamic_identity_exact_
                    ? static_cast<std::size_t>(
                          dynamic_return_latency_output_frames_)
                    : 0;
                dynamic_identity_delay_.assign(
                    channel_count * dynamic_identity_delay_frames_, 0.0f);
            } catch (...) {
                release();
                return SampleHeritagePrepareStatus::AllocationFailed;
            }
            for (std::size_t channel = 0; channel < channel_count; ++channel) {
                auto* pointer = machine_scratch_.data() +
                    channel * maximum_machine_frames_;
                machine_ptrs_[channel] = pointer;
                machine_const_ptrs_[channel] = pointer;
            }
            channel_count_ = channel_count;
            maximum_output_frames_ = maximum_output_frames;
            maximum_runtime_clock_factor_ = maximum_runtime_clock_factor;
            exact_prepared_ = true;
            dynamic_identity_epoch_ = dynamic_identity_exact_;
        }

        prepared_ = true;
        process_sequence_ = 0;
        ++prepare_epoch_;
        if (prepare_epoch_ == 0) ++prepare_epoch_;
        return SampleHeritagePrepareStatus::Ok;
    }

    static bool checked_frame_bound(std::size_t output_frames,
                                    double ratio,
                                    bool identity,
                                    std::size_t& result) noexcept {
        if (identity) {
            result = output_frames;
            return true;
        }
        const auto frames = std::ceil(static_cast<double>(output_frames) * ratio);
        if (!std::isfinite(frames) || frames < 0.0 ||
            frames >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
            return false;
        result = static_cast<std::size_t>(frames) + 1;
        return result != 0;
    }

    bool valid_runtime_clock_multiplier(double multiplier) const noexcept {
        return std::isfinite(multiplier) &&
               multiplier >= 1.0 / maximum_runtime_clock_factor_ &&
               multiplier <= maximum_runtime_clock_factor_;
    }

    bool runtime_clock_identity_only() const noexcept {
        if (!typed_voice_) {
            for (std::size_t index = 0; index < profile_.stage_count; ++index) {
                const auto& spec = profile_.stages[index];
                if (spec.bypass) continue;
                const auto supported = std::visit(
                    [&](const auto& stage) noexcept {
                        using Stage = std::decay_t<decltype(stage)>;
                        if constexpr (std::is_same_v<
                                          Stage,
                                          SampleHeritageMachineDomainStage>)
                            return stage.sample_rate == profile_.host_sample_rate;
                        if constexpr (std::is_same_v<
                                          Stage,
                                          SampleHeritageClockPitchStage>)
                            return stage.ratio == 1.0;
                        return false;
                    },
                    spec.parameters);
                if (!supported) return false;
            }
            return true;
        }
        bool has_pitch_marker = false;
        for (std::size_t index = 0; index < profile_.voice_count; ++index) {
            const auto& spec = profile_.voice[index];
            if (spec.bypass) continue;
            const auto supported = std::visit(
                [&](const auto& block) noexcept {
                    using Block = std::decay_t<decltype(block)>;
                    if constexpr (std::is_same_v<
                                      Block,
                                      SampleHeritageVoiceMachineDomainBlock>)
                        return block.sample_rate == profile_.host_sample_rate;
                    if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceClockBlock>)
                        return block.ratio == 1.0;
                    if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoicePitchBlock>) {
                        has_pitch_marker =
                            block.family == SampleHeritagePitchFamily::VariableClock;
                        return has_pitch_marker;
                    }
                    return false;
                },
                spec.parameters);
            if (!supported) return false;
        }
        return has_pitch_marker;
    }

    void process_dynamic_identity_delay(
        const BufferView<const float>& input,
        BufferView<float> output) noexcept {
        if (dynamic_identity_delay_frames_ == 0) return;
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            for (std::size_t channel = 0; channel < channel_count_; ++channel) {
                auto& delayed = dynamic_identity_delay_[
                    channel * dynamic_identity_delay_frames_ +
                    dynamic_identity_delay_head_];
                output.channel(channel)[frame] = delayed;
                delayed = input.channel(channel)[frame];
            }
            dynamic_identity_delay_head_ =
                (dynamic_identity_delay_head_ + 1) %
                dynamic_identity_delay_frames_;
        }
    }

    static void initialize_seed(RuntimeStage& runtime) noexcept {
        std::visit([&](const auto& stage) noexcept {
            using Stage = std::decay_t<decltype(stage)>;
            if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage> ||
                          std::is_same_v<Stage, SampleHeritageQuantizationStage>)
                runtime.random_state = stage.seed;
        }, runtime.spec.parameters);
    }

    void process_machine_stages(BufferView<float> buffer,
                                bool draining_tail = false) noexcept {
        if (typed_voice_) {
            if (draining_tail)
                voice_dsp_.process_tail(buffer);
            else
                voice_dsp_.process(buffer);
            return;
        }
        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            auto& runtime = stages_[index];
            if (runtime.spec.bypass) continue;
            if (draining_tail &&
                (std::holds_alternative<SampleHeritageQuantizationStage>(
                     runtime.spec.parameters) ||
                 std::holds_alternative<SampleHeritageNoiseStage>(
                     runtime.spec.parameters)))
                continue;
            std::visit([&](const auto& stage) noexcept {
                process_stage(runtime, stage, buffer);
            }, runtime.spec.parameters);
        }
    }

    static void process_stage(RuntimeStage&,
                              const SampleHeritageMachineDomainStage&,
                              BufferView<float>) noexcept {}
    static void process_stage(RuntimeStage&,
                              const SampleHeritageClockPitchStage&,
                              BufferView<float>) noexcept {}

    static void process_stage(RuntimeStage& runtime,
                              const SampleHeritageQuantizationStage& stage,
                              BufferView<float> buffer) noexcept {
        const auto levels = static_cast<float>(std::uint32_t{1} << (stage.bit_depth - 1));
        const auto upper = 1.0f - 1.0f / levels;
        for (std::size_t frame = 0; frame < buffer.num_samples(); ++frame) {
            for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
                auto& sample = buffer.channel(channel)[frame];
                const auto dither = stage.dither_lsb == 0.0f
                    ? 0.0f
                    : detail::bipolar_random(runtime.random_state) * stage.dither_lsb;
                sample = std::round(std::clamp(sample, -1.0f, upper) * levels + dither) /
                         levels;
            }
        }
    }

    static void process_stage(RuntimeStage& runtime,
                              const SampleHeritageDacHoldStage& stage,
                              BufferView<float> buffer) noexcept {
        for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
            auto& held = runtime.hold_value[channel];
            auto& phase = runtime.hold_phase[channel];
            for (auto& sample : buffer.channel(channel)) {
                if (phase == 0) held = sample;
                sample = held;
                phase = (phase + 1) % stage.hold_samples;
            }
        }
    }

    static void process_stage(RuntimeStage& runtime,
                              const SampleHeritageReconstructionFilterStage&,
                              BufferView<float> buffer) noexcept {
        const auto pole = runtime.filter_pole;
        for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
            auto previous = runtime.filter_state[channel];
            for (auto& sample : buffer.channel(channel)) {
                previous = (1.0f - pole) * sample + pole * previous;
                sample = previous;
            }
            runtime.filter_state[channel] = previous;
        }
    }

    static void process_stage(RuntimeStage& runtime,
                              const SampleHeritageNoiseStage& stage,
                              BufferView<float> buffer) noexcept {
        for (std::size_t frame = 0; frame < buffer.num_samples(); ++frame) {
            for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
                buffer.channel(channel)[frame] +=
                    detail::bipolar_random(runtime.random_state) * stage.amplitude;
            }
        }
    }

    static void process_stage(RuntimeStage&,
                              const SampleHeritageOutputStage& stage,
                              BufferView<float> buffer) noexcept {
        for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel)
            for (auto& sample : buffer.channel(channel)) sample *= stage.gain;
    }

    SampleHeritagePreparedProfile profile_{};
    std::array<RuntimeStage, kSampleHeritageMaximumStages> stages_{};
    SampleSincKernelBank sinc_bank_;
    SampleHeritageCausalSrc input_src_;
    SampleHeritageCausalSrc return_src_;
    SampleHeritageVoiceDsp voice_dsp_;
    std::vector<float> machine_scratch_;
    std::vector<float> dynamic_identity_delay_;
    std::array<float*, kSampleHeritageMaximumChannels> machine_ptrs_{};
    std::array<const float*, kSampleHeritageMaximumChannels> machine_const_ptrs_{};
    bool prepared_ = false;
    bool exact_prepared_ = false;
    bool all_bypassed_ = false;
    bool typed_voice_ = false;
    bool input_identity_ = true;
    bool return_identity_ = true;
    bool runtime_dynamic_clock_ = false;
    bool dynamic_identity_exact_ = false;
    bool dynamic_identity_epoch_ = false;
    std::size_t channel_count_ = 0;
    std::size_t maximum_output_frames_ = 0;
    std::size_t maximum_input_frames_ = 0;
    std::size_t maximum_machine_frames_ = 0;
    double machine_sample_rate_ = 0.0;
    double clock_ratio_ = 1.0;
    double clocked_sample_rate_ = 0.0;
    double input_ratio_ = 1.0;
    double return_ratio_ = 1.0;
    double maximum_runtime_clock_factor_ = 1.0;
    double dynamic_return_latency_output_frames_ = 0.0;
    std::size_t dynamic_identity_delay_frames_ = 0;
    std::size_t dynamic_identity_delay_head_ = 0;
    std::uint64_t prepare_epoch_ = 1;
    std::uint64_t process_sequence_ = 0;
};

}  // namespace pulp::audio
