#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_live_cyclic.hpp>
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
    double maximum_note_pitch_factor = 1.0;
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
    std::size_t pre_live_machine_frames = 0;
    std::size_t input_frames = 0;
    double runtime_clock_multiplier = 1.0;
    double note_pitch_multiplier = 1.0;

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
                            config.maximum_runtime_clock_factor,
                            config.maximum_note_pitch_factor);
    }

    void release() noexcept {
        profile_ = {};
        stages_ = {};
        input_src_.release();
        return_src_.release();
        live_cyclic_.release();
        voice_dsp_.release();
        sinc_bank_.release();
        std::vector<float>().swap(machine_scratch_);
        std::vector<float>().swap(pre_live_scratch_);
        std::vector<float>().swap(dynamic_identity_delay_);
        machine_ptrs_.fill(nullptr);
        machine_const_ptrs_.fill(nullptr);
        pre_live_ptrs_.fill(nullptr);
        pre_live_const_ptrs_.fill(nullptr);
        prepared_ = false;
        exact_prepared_ = false;
        all_bypassed_ = false;
        typed_voice_ = false;
        live_cyclic_active_ = false;
        runtime_dynamic_clock_ = false;
        dynamic_identity_exact_ = false;
        dynamic_identity_epoch_ = false;
        dynamic_identity_delay_frames_ = 0;
        dynamic_identity_delay_head_ = 0;
        dynamic_return_latency_output_frames_ = 0.0;
        dynamic_input_latency_machine_frames_ = 0.0;
        dynamic_return_src_latency_output_frames_ = 0.0;
        input_identity_ = true;
        return_identity_ = true;
        channel_count_ = 0;
        maximum_output_frames_ = 0;
        maximum_input_frames_ = 0;
        maximum_machine_frames_ = 0;
        maximum_pre_live_machine_frames_ = 0;
        maximum_runtime_clock_factor_ = 1.0;
        maximum_artifact_clock_factor_ = 1.0;
        maximum_note_pitch_factor_ = 1.0;
        machine_sample_rate_ = 0.0;
        clock_ratio_ = 1.0;
        clocked_sample_rate_ = 0.0;
        input_ratio_ = 1.0;
        return_ratio_ = 1.0;
        process_sequence_ = 0;
        last_valid_output_frames_ = 0;
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
        live_cyclic_.reset();
        voice_dsp_.reset();
        std::fill(dynamic_identity_delay_.begin(),
                  dynamic_identity_delay_.end(), 0.0f);
        dynamic_identity_delay_head_ = 0;
        dynamic_identity_epoch_ = dynamic_identity_exact_;
        process_sequence_ = 0;
        last_valid_output_frames_ = 0;
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
    std::size_t maximum_pre_live_machine_frames() const noexcept {
        return maximum_pre_live_machine_frames_;
    }
    const SampleHeritageLiveCyclicResources& live_cyclic_resources() const noexcept {
        return live_cyclic_.resources();
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
        if (runtime_dynamic_clock_)
            return dynamic_return_latency_output_frames_;
        constexpr double half =
            static_cast<double>(kHighQualitySampleSincHalfWidth);
        const auto input_latency = input_identity_ ? 0.0 : half;
        const auto effective_return_ratio = return_ratio_ *
            runtime_clock_multiplier;
        const auto return_latency =
            return_identity_ ? 0.0 : half / effective_return_ratio;
        return input_latency + return_latency;
    }

    /// Host-rate process_tail_exact() frames sufficient to flush every finite
    /// SRC, hold, and filter state for the prepared profile.
    std::uint64_t tail_output_frames() const noexcept {
        if (!exact_prepared_ || (all_bypassed_ && !runtime_dynamic_clock_))
            return 0;

        long double machine_frames = typed_voice_
            ? static_cast<long double>(voice_dsp_.tail_machine_domain_frames())
            : 0.0L;
        const auto host_frames = typed_voice_
            ? static_cast<long double>(voice_dsp_.tail_host_frames())
            : 0.0L;
        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            const auto& runtime = stages_[index];
            if (runtime.spec.bypass)
                continue;
            std::visit(
                [&](const auto& stage) noexcept {
                    using Stage = std::decay_t<decltype(stage)>;
                    if constexpr (std::is_same_v<Stage, SampleHeritageDacHoldStage>) {
                        machine_frames += stage.hold_samples;
                    } else if constexpr (std::is_same_v<Stage,
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
        machine_frames =
            std::min(machine_frames, std::ceil(static_cast<long double>(machine_sample_rate_) *
                                               SampleHeritageVoiceDsp::kMaximumTailSeconds));
        if (live_cyclic_active_)
            machine_frames += static_cast<long double>(live_cyclic_.remaining_output_frames());
        if (live_cyclic_active_) {
            machine_frames +=
                static_cast<long double>(input_src_.remaining_valid_output_frames()) /
                static_cast<long double>(live_cyclic_.source_frames_per_output_frame());
        }

        const auto required_frames =
            static_cast<long double>(latency_output_frames()) +
            machine_frames /
                static_cast<long double>(return_ratio_ /
                                         maximum_runtime_clock_factor_) +
            host_frames;
        const auto output_frames = std::ceil(required_frames);
        if (!std::isfinite(output_frames) || output_frames <= 0.0L)
            return output_frames <= 0.0L ? 0 : std::numeric_limits<std::uint64_t>::max();
        if (output_frames >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
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
        if (typed_voice_)
            voice_dsp_.process_host_frame(buffer);
        return true;
    }

    SampleHeritageProcessPlan plan_exact(std::size_t output_frames) const noexcept {
        return plan_exact(output_frames, 1.0);
    }

    SampleHeritageProcessPlan plan_exact(
        std::size_t output_frames,
        double runtime_clock_multiplier) const noexcept {
        return plan_exact(output_frames, runtime_clock_multiplier, 1.0);
    }

    SampleHeritageProcessPlan plan_exact(
        std::size_t output_frames,
        double runtime_clock_multiplier,
        double note_pitch_multiplier) const noexcept {
        SampleHeritageProcessPlan result;
        result.prepare_epoch = prepare_epoch_;
        result.sequence = process_sequence_;
        result.output_frames = output_frames;
        result.runtime_clock_multiplier = runtime_clock_multiplier;
        result.note_pitch_multiplier = note_pitch_multiplier;
        if (!exact_prepared_) return result;
        if (!valid_runtime_rate_multipliers(runtime_clock_multiplier,
                                            note_pitch_multiplier)) {
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
        if (dynamic_identity_epoch_ && runtime_clock_multiplier == 1.0 &&
            note_pitch_multiplier == 1.0) {
            result.status = SampleHeritagePlanStatus::Ok;
            result.input_frames = output_frames;
            return result;
        }
        const auto return_plan = return_src_.plan(
            output_frames, return_ratio_ * runtime_clock_multiplier *
                               note_pitch_multiplier);
        if (!return_plan.valid()) {
            result.status = SampleHeritagePlanStatus::SizeOverflow;
            return result;
        }
        result.machine_frames = return_plan.input_frames;
        result.pre_live_machine_frames = result.machine_frames;
        if (live_cyclic_active_) {
            const auto live_plan = live_cyclic_.plan(result.machine_frames);
            if (!live_plan.valid()) {
                result.status = SampleHeritagePlanStatus::SizeOverflow;
                return result;
            }
            result.pre_live_machine_frames = live_plan.input_frames;
        }
        const auto input_plan = input_src_.plan(
            result.pre_live_machine_frames,
            input_ratio_ / runtime_clock_multiplier);
        if (!input_plan.valid() || result.machine_frames > maximum_machine_frames_ ||
            result.pre_live_machine_frames > maximum_pre_live_machine_frames_ ||
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
        return process_exact_impl(plan, input, output, false,
                                  input.num_samples(), false);
    }

    SampleHeritageProcessStatus process_source_exact(
        const SampleHeritageProcessPlan& plan,
        const BufferView<const float>& input,
        BufferView<float> output,
        std::size_t valid_input_frames,
        bool end_of_source) noexcept {
        return process_exact_impl(plan, input, output, false,
                                  valid_input_frames, end_of_source);
    }

    std::size_t last_valid_output_frames() const noexcept {
        return last_valid_output_frames_;
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
        return process_exact_impl(plan, input, output, true, 0, true);
    }

private:
    SampleHeritageProcessStatus process_exact_impl(
        const SampleHeritageProcessPlan& plan,
        const BufferView<const float>& input,
        BufferView<float> output,
        bool draining_tail,
        std::size_t valid_input_frames,
        bool end_of_source) noexcept {
        if (!exact_prepared_) return SampleHeritageProcessStatus::NotPrepared;
        const auto expected = plan_exact(output.num_samples(),
                                         plan.runtime_clock_multiplier,
                                         plan.note_pitch_multiplier);
        if (!plan.valid() || !expected.valid() ||
            plan.prepare_epoch != expected.prepare_epoch ||
            plan.sequence != expected.sequence ||
            plan.output_frames != expected.output_frames ||
            plan.machine_frames != expected.machine_frames ||
            plan.pre_live_machine_frames != expected.pre_live_machine_frames ||
            plan.input_frames != expected.input_frames ||
            plan.runtime_clock_multiplier != expected.runtime_clock_multiplier ||
            plan.note_pitch_multiplier != expected.note_pitch_multiplier) {
            return SampleHeritageProcessStatus::InvalidPlan;
        }
        if (input.num_channels() != channel_count_ ||
            output.num_channels() != channel_count_) {
            return SampleHeritageProcessStatus::InvalidDimensions;
        }
        if (input.num_samples() != plan.input_frames)
            return SampleHeritageProcessStatus::InputFrameMismatch;
        if (valid_input_frames > input.num_samples())
            return SampleHeritageProcessStatus::InputFrameMismatch;
        last_valid_output_frames_ = output.num_samples();

        if (all_bypassed_ && !runtime_dynamic_clock_) {
            for (std::size_t channel = 0; channel < channel_count_; ++channel)
                std::copy(input.channel(channel).begin(), input.channel(channel).end(),
                          output.channel(channel).begin());
            ++process_sequence_;
            if (end_of_source)
                last_valid_output_frames_ =
                    std::min(output.num_samples(), valid_input_frames);
            return SampleHeritageProcessStatus::Ok;
        }
        if (dynamic_identity_epoch_ &&
            plan.runtime_clock_multiplier == 1.0 &&
            plan.note_pitch_multiplier == 1.0) {
            for (std::size_t channel = 0; channel < channel_count_; ++channel)
                std::copy(input.channel(channel).begin(),
                          input.channel(channel).end(),
                          output.channel(channel).begin());
            process_dynamic_latency_delay(output,
                                           dynamic_identity_delay_frames_);
            if (end_of_source) {
                const auto valid =
                    std::min(output.num_samples(), valid_input_frames);
                last_valid_output_frames_ = valid + std::min(
                    output.num_samples() - valid,
                    dynamic_identity_delay_frames_);
            }
            ++process_sequence_;
            return SampleHeritageProcessStatus::Ok;
        }

        auto* prepared_ptrs = live_cyclic_active_ ? pre_live_ptrs_.data() : machine_ptrs_.data();
        BufferView<float> prepared_machine(prepared_ptrs, channel_count_,
                                           plan.pre_live_machine_frames);
        const auto effective_input_ratio =
            input_ratio_ / plan.runtime_clock_multiplier;
        const auto input_status = live_cyclic_active_
            ? input_src_.process_source(input, prepared_machine,
                                        valid_input_frames, end_of_source,
                                        effective_input_ratio)
            : input_src_.process(input, prepared_machine,
                                 effective_input_ratio);
        if (input_status != SampleHeritageSrcStatus::Ok)
            return SampleHeritageProcessStatus::InternalContractFailure;
        BufferView<float> machine(machine_ptrs_.data(), channel_count_, plan.machine_frames);
        if (live_cyclic_active_) {
            const auto valid_pre_live_frames = input_src_.last_valid_output_frames();
            BufferView<float> valid_pre_live(pre_live_ptrs_.data(), channel_count_,
                                             valid_pre_live_frames);
            voice_dsp_.process_before_live(valid_pre_live, draining_tail);
            if (valid_pre_live_frames < plan.pre_live_machine_frames) {
                for (std::size_t channel = 0; channel < channel_count_; ++channel)
                    std::fill(pre_live_ptrs_[channel] + valid_pre_live_frames,
                              pre_live_ptrs_[channel] + plan.pre_live_machine_frames, 0.0f);
            }
            BufferView<const float> live_input(pre_live_const_ptrs_.data(), channel_count_,
                                               plan.pre_live_machine_frames);
            const auto live_end_of_source = end_of_source && input_src_.source_drained();
            if (live_cyclic_.process(live_input, machine, valid_pre_live_frames,
                                     live_end_of_source) != SampleHeritageLiveCyclicStatus::Ok)
                return SampleHeritageProcessStatus::InternalContractFailure;
            if (live_end_of_source) {
                const auto valid_machine = live_cyclic_.last_valid_output_frames();
                const auto converted = std::ceil(static_cast<double>(valid_machine) /
                    (return_ratio_ * plan.runtime_clock_multiplier *
                     plan.note_pitch_multiplier));
                last_valid_output_frames_ =
                    static_cast<std::size_t>(std::min<double>(output.num_samples(), converted));
            }
            voice_dsp_.process_after_live(machine, draining_tail);
        } else {
            process_machine_stages(machine, draining_tail);
        }
        BufferView<const float> machine_input(machine_const_ptrs_.data(), channel_count_,
                                              plan.machine_frames);
        if (return_src_.process(machine_input, output,
                                return_ratio_ * plan.runtime_clock_multiplier *
                                    plan.note_pitch_multiplier) !=
            SampleHeritageSrcStatus::Ok)
            return SampleHeritageProcessStatus::InternalContractFailure;
        if (typed_voice_)
            voice_dsp_.process_host_frame(output, draining_tail);
        if (runtime_dynamic_clock_) {
            const auto effective_return_ratio = return_ratio_ *
                plan.runtime_clock_multiplier * plan.note_pitch_multiplier;
            const auto active_input_latency =
                input_identity_ && maximum_artifact_clock_factor_ == 1.0
                ? 0.0
                : dynamic_input_latency_machine_frames_;
            const auto raw_latency = static_cast<std::size_t>(std::ceil(
                active_input_latency /
                effective_return_ratio)) +
                static_cast<std::size_t>(
                    dynamic_return_src_latency_output_frames_);
            const auto correction = raw_latency < dynamic_identity_delay_frames_
                ? dynamic_identity_delay_frames_ - raw_latency
                : 0;
            process_dynamic_latency_delay(output, correction);
            if (end_of_source &&
                last_valid_output_frames_ < output.num_samples())
                last_valid_output_frames_ += std::min(
                    output.num_samples() - last_valid_output_frames_,
                    correction);
            if (plan.runtime_clock_multiplier != 1.0 ||
                plan.note_pitch_multiplier != 1.0)
                dynamic_identity_epoch_ = false;
        }
        ++process_sequence_;
        return SampleHeritageProcessStatus::Ok;
    }

public:

    /// Captures ContinueSerializedState RNG streams and the live shuffler's
    /// next-cycle sequence. SRC, ring, hold, and filter transients remain
    /// outside the serialized contract.
    SampleHeritageRuntimeStateCapture capture_runtime_state() const noexcept {
        SampleHeritageRuntimeStateCapture result;
        if (!prepared_) return result;
        result.state.profile_schema_version = profile_.schema_version;
        std::copy(profile_.profile_id.begin(), profile_.profile_id.end(),
                  result.state.profile_id.begin());
        result.state.profile_digest = profile_.profile_digest;
        if (typed_voice_) {
            for (std::size_t index = 0; index < profile_.voice_count; ++index) {
                if (voice_dsp_.converter_continues_state(index)) {
                    auto& saved = result.state.rng_states[
                        result.state.rng_state_count++];
                    saved.stage_index = static_cast<std::uint8_t>(index);
                    saved.stage_type =
                        SampleHeritageRuntimeRngStageType::Quantization;
                    saved.random_state = voice_dsp_.converter_random_state(index);
                    continue;
                }
                const auto* live =
                    std::get_if<SampleHeritageVoiceLiveCyclicStretchBlock>(
                        &profile_.voice[index].parameters);
                if (live == nullptr || profile_.voice[index].bypass ||
                    live->seed_policy !=
                        SampleHeritageSeedPolicy::ContinueSerializedState)
                    continue;
                const auto continuation =
                    live_cyclic_.capture_next_cycle_rng_continuation();
                if (continuation.next_cycle_index ==
                    std::numeric_limits<std::uint64_t>::max())
                    return {};
                auto& saved = result.state.rng_states[
                    result.state.rng_state_count++];
                saved.stage_index = static_cast<std::uint8_t>(index);
                saved.stage_type = SampleHeritageRuntimeRngStageType::LiveCyclic;
                saved.random_state = continuation.next_cycle_index + 1;
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
                const auto* live =
                    std::get_if<SampleHeritageVoiceLiveCyclicStretchBlock>(
                        &profile_.voice[index].parameters);
                const auto converter =
                    voice_dsp_.converter_continues_state(index);
                const auto cyclic = live != nullptr &&
                    !profile_.voice[index].bypass &&
                    live->seed_policy ==
                        SampleHeritageSeedPolicy::ContinueSerializedState;
                if (!converter && !cyclic) continue;
                if (expected_count >= state.rng_state_count)
                    return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
                const auto& saved = state.rng_states[expected_count];
                if (saved.stage_index != index ||
                    saved.stage_type != (converter
                        ? SampleHeritageRuntimeRngStageType::Quantization
                        : SampleHeritageRuntimeRngStageType::LiveCyclic) ||
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
                if (saved.stage_type ==
                    SampleHeritageRuntimeRngStageType::Quantization) {
                    if (!voice_dsp_.restore_converter_random_state(
                            saved.stage_index, saved.random_state))
                        return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
                } else {
                    const auto* live =
                        std::get_if<SampleHeritageVoiceLiveCyclicStretchBlock>(
                            &profile_.voice[saved.stage_index].parameters);
                    if (live == nullptr || live->seed == 0)
                        return SampleHeritageRuntimeStateStatus::InvalidStageLayout;
                    live_cyclic_.reset_with_rng_continuation(
                        {live->seed, saved.random_state - 1});
                }
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
        return prepare_impl(profile, channel_count, maximum_output_frames,
                            exact, external_sinc_bank,
                            maximum_runtime_clock_factor, 1.0);
    }

    SampleHeritagePrepareStatus prepare_impl(
        const SampleHeritagePreparedProfile& profile,
        std::size_t channel_count,
        std::size_t maximum_output_frames,
        bool exact,
        const SampleSincKernelBankView* external_sinc_bank,
        double maximum_runtime_clock_factor,
        double maximum_note_pitch_factor) noexcept {
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
                      !std::isfinite(maximum_runtime_clock_factor) ||
                      !(maximum_note_pitch_factor >= 1.0) ||
                      maximum_note_pitch_factor > 64.0 ||
                      !std::isfinite(maximum_note_pitch_factor) ||
                      maximum_runtime_clock_factor *
                              maximum_note_pitch_factor >
                          64.0)) {
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
        input_ratio_ = profile_.host_sample_rate / clocked_sample_rate_;
        return_ratio_ = clocked_sample_rate_ / profile_.host_sample_rate;
        input_identity_ = clocked_sample_rate_ == profile_.host_sample_rate;
        return_identity_ = clocked_sample_rate_ == profile_.host_sample_rate;
        if ((!input_identity_ && input_ratio_ >
                                  kMaximumDenseSampleSincConsumption) ||
            (!return_identity_ && return_ratio_ >
                                   kMaximumDenseSampleSincConsumption) ||
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
        if (typed_voice_ &&
            !voice_dsp_.prepare(profile_, machine_sample_rate_,
                                profile_.host_sample_rate)) {
            release();
            return SampleHeritagePrepareStatus::InvalidProfile;
        }
        live_cyclic_active_ = typed_voice_ && voice_dsp_.live_stage_active();
        if (live_cyclic_active_ && !exact) {
            release();
            return SampleHeritagePrepareStatus::UnsupportedRateConversion;
        }

        if (exact) {
            const auto maximum_combined_factor =
                maximum_runtime_clock_factor * maximum_note_pitch_factor;
            runtime_dynamic_clock_ = maximum_combined_factor > 1.0;
            const auto maximum_return_ratio =
                return_ratio_ * maximum_combined_factor;
            const auto maximum_input_ratio =
                input_ratio_ * maximum_runtime_clock_factor;
            const auto maximum_ratio =
                std::max(maximum_input_ratio, maximum_return_ratio);
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
            const auto input_selection = input_identity_ &&
                    maximum_runtime_clock_factor == 1.0
                ? SampleSincKernelSelection{}
                : bank.select(input_ratio_);
            const auto return_selection = return_identity_
                ? SampleSincKernelSelection{}
                : bank.select(return_ratio_);
            if ((!(input_identity_ && maximum_runtime_clock_factor == 1.0) &&
                 !input_selection.valid()) ||
                (!return_identity_ && !return_selection.valid())) {
                release();
                return SampleHeritagePrepareStatus::KernelBuildFailed;
            }
            const auto input_status = maximum_runtime_clock_factor == 1.0
                ? input_src_.prepare(input_ratio_, channel_count,
                                     input_selection, input_identity_)
                : input_src_.prepare_variable(
                      input_ratio_,
                      input_ratio_ / maximum_runtime_clock_factor,
                      maximum_input_ratio, channel_count, bank);
            const auto return_status = maximum_combined_factor == 1.0
                ? return_src_.prepare(return_ratio_, channel_count,
                                      return_selection, return_identity_)
                : return_src_.prepare_variable(return_ratio_,
                                               return_ratio_ /
                                                   maximum_combined_factor,
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
                                         maximum_combined_factor == 1.0,
                                     maximum_machine_frames_) ||
                (channel_count != 0 && maximum_machine_frames_ >
                     std::numeric_limits<std::size_t>::max() / channel_count)) {
                release();
                return SampleHeritagePrepareStatus::SizeOverflow;
            }
            maximum_pre_live_machine_frames_ = maximum_machine_frames_;
            if (live_cyclic_active_) {
                const auto live_index = voice_dsp_.live_stage_index();
                const auto* block =
                    std::get_if<SampleHeritageVoiceLiveCyclicStretchBlock>(
                        &profile_.voice[live_index].parameters);
                std::size_t cycle_samples = 0;
                std::size_t splice_samples = 0;
                if (block == nullptr ||
                    !milliseconds_to_machine_frames(block->cycle_ms,
                                                    machine_sample_rate_,
                                                    cycle_samples) ||
                    !milliseconds_to_machine_frames(block->splice_ms,
                                                    machine_sample_rate_,
                                                    splice_samples, true) ||
                    splice_samples > cycle_samples / 2) {
                    release();
                    return SampleHeritagePrepareStatus::InvalidProfile;
                }
                const auto divisions = block->shuffle_divisions == 0
                    ? std::size_t{1}
                    : static_cast<std::size_t>(block->shuffle_divisions);
                const SampleHeritageLiveCyclicConfig live_config{
                    .factor = block->factor,
                    .cycle_samples = cycle_samples,
                    .crossfade_samples = splice_samples,
                    .shuffle_divisions = divisions,
                    .linked_channels = block->stereo_link,
                    .seed = block->seed,
                    .shuffle = block->shuffle_divisions == 0
                        ? SampleHeritageLiveCyclicShuffle::Identity
                        : SampleHeritageLiveCyclicShuffle::FisherYates,
                    .max_block_samples = maximum_machine_frames_,
                    .channel_count = channel_count,
                    .pitch_mode = block->pitch_mode,
                    .tempo_lock = block->tempo_lock};
                const auto live_status = live_cyclic_.prepare(live_config);
                if (live_status != SampleHeritageLiveCyclicStatus::Ok) {
                    release();
                    return live_status ==
                               SampleHeritageLiveCyclicStatus::AllocationFailed
                        ? SampleHeritagePrepareStatus::AllocationFailed
                        : SampleHeritagePrepareStatus::SizeOverflow;
                }
                maximum_pre_live_machine_frames_ =
                    live_cyclic_.resources().maximum_input_frames;
                if (maximum_pre_live_machine_frames_ == 0) {
                    release();
                    return SampleHeritagePrepareStatus::SizeOverflow;
                }
            }
            if (!checked_frame_bound(maximum_pre_live_machine_frames_,
                                     input_ratio_ /
                                         maximum_runtime_clock_factor,
                                     input_identity_ &&
                                         maximum_combined_factor == 1.0,
                                     maximum_input_frames_) ||
                (channel_count != 0 && maximum_pre_live_machine_frames_ >
                     std::numeric_limits<std::size_t>::max() / channel_count)) {
                release();
                return SampleHeritagePrepareStatus::SizeOverflow;
            }
            try {
                machine_scratch_.assign(channel_count * maximum_machine_frames_, 0.0f);
                if (live_cyclic_active_)
                    pre_live_scratch_.assign(
                        channel_count * maximum_pre_live_machine_frames_, 0.0f);
                dynamic_identity_exact_ = runtime_dynamic_clock_ &&
                                          runtime_clock_identity_only() &&
                                          input_identity_ && return_ratio_ == 1.0;
                dynamic_input_latency_machine_frames_ = runtime_dynamic_clock_
                    ? std::ceil(
                          static_cast<double>(kHighQualitySampleSincHalfWidth) /
                          (input_ratio_ / maximum_runtime_clock_factor))
                    : 0.0;
                dynamic_return_src_latency_output_frames_ = runtime_dynamic_clock_
                    ? std::ceil(
                          static_cast<double>(kHighQualitySampleSincHalfWidth) /
                          (return_ratio_ / maximum_combined_factor))
                    : 0.0;
                dynamic_return_latency_output_frames_ = runtime_dynamic_clock_
                    ? std::ceil(dynamic_input_latency_machine_frames_ /
                                (return_ratio_ /
                                 maximum_combined_factor)) +
                          dynamic_return_src_latency_output_frames_
                    : 0.0;
                dynamic_identity_delay_frames_ = runtime_dynamic_clock_
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
                if (live_cyclic_active_) {
                    auto* pre_live = pre_live_scratch_.data() +
                        channel * maximum_pre_live_machine_frames_;
                    pre_live_ptrs_[channel] = pre_live;
                    pre_live_const_ptrs_[channel] = pre_live;
                }
            }
            channel_count_ = channel_count;
            maximum_output_frames_ = maximum_output_frames;
            maximum_runtime_clock_factor_ = maximum_combined_factor;
            maximum_artifact_clock_factor_ =
                maximum_runtime_clock_factor;
            maximum_note_pitch_factor_ = maximum_note_pitch_factor;
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

    /// Millisecond controls use nearest-frame rounding with ties toward the
    /// next frame, making the same profile deterministic at a given rate.
    static bool milliseconds_to_machine_frames(double milliseconds,
                                               double sample_rate,
                                               std::size_t& result,
                                               bool allow_zero = false) noexcept {
        const auto frames = std::floor(milliseconds * sample_rate * 0.001 + 0.5);
        if (!std::isfinite(frames) || frames < 0.0 ||
            frames >= static_cast<double>(std::numeric_limits<std::size_t>::max()) ||
            (!allow_zero && frames < 1.0))
            return false;
        result = static_cast<std::size_t>(frames);
        return true;
    }

    bool valid_runtime_clock_multiplier(double multiplier) const noexcept {
        return std::isfinite(multiplier) &&
               multiplier >= 1.0 / maximum_runtime_clock_factor_ &&
               multiplier <= maximum_runtime_clock_factor_;
    }

    bool valid_runtime_rate_multipliers(double clock_multiplier,
                                        double pitch_multiplier) const noexcept {
        if (!(clock_multiplier > 0.0) || !std::isfinite(clock_multiplier) ||
            !(pitch_multiplier > 0.0) || !std::isfinite(pitch_multiplier))
            return false;
        return clock_multiplier >= 1.0 / maximum_artifact_clock_factor_ &&
               clock_multiplier <= maximum_artifact_clock_factor_ &&
               pitch_multiplier >= 1.0 / maximum_note_pitch_factor_ &&
               pitch_multiplier <= maximum_note_pitch_factor_;
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
                        has_pitch_marker = true;
                        return true;
                    }
                    return false;
                },
                spec.parameters);
            if (!supported) return false;
        }
        return has_pitch_marker;
    }

    void process_dynamic_latency_delay(BufferView<float> output,
                                       std::size_t delay_frames) noexcept {
        if (dynamic_identity_delay_frames_ == 0) return;
        delay_frames = std::min(delay_frames, dynamic_identity_delay_frames_);
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            for (std::size_t channel = 0; channel < channel_count_; ++channel) {
                const auto read_index =
                    (dynamic_identity_delay_head_ +
                     dynamic_identity_delay_frames_ - delay_frames) %
                    dynamic_identity_delay_frames_;
                auto& destination = dynamic_identity_delay_[
                    channel * dynamic_identity_delay_frames_ +
                    dynamic_identity_delay_head_];
                const auto delayed = dynamic_identity_delay_[
                    channel * dynamic_identity_delay_frames_ + read_index];
                destination = output.channel(channel)[frame];
                if (delay_frames != 0)
                    output.channel(channel)[frame] = delayed;
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
                voice_dsp_.process_machine_tail(buffer);
            else
                voice_dsp_.process_before_live(buffer);
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
    SampleHeritageLiveCyclicStretch live_cyclic_;
    SampleHeritageVoiceDsp voice_dsp_;
    std::vector<float> machine_scratch_;
    std::vector<float> pre_live_scratch_;
    std::vector<float> dynamic_identity_delay_;
    std::array<float*, kSampleHeritageMaximumChannels> machine_ptrs_{};
    std::array<const float*, kSampleHeritageMaximumChannels> machine_const_ptrs_{};
    std::array<float*, kSampleHeritageMaximumChannels> pre_live_ptrs_{};
    std::array<const float*, kSampleHeritageMaximumChannels>
        pre_live_const_ptrs_{};
    bool prepared_ = false;
    bool exact_prepared_ = false;
    bool all_bypassed_ = false;
    bool typed_voice_ = false;
    bool live_cyclic_active_ = false;
    bool input_identity_ = true;
    bool return_identity_ = true;
    bool runtime_dynamic_clock_ = false;
    bool dynamic_identity_exact_ = false;
    bool dynamic_identity_epoch_ = false;
    std::size_t channel_count_ = 0;
    std::size_t maximum_output_frames_ = 0;
    std::size_t maximum_input_frames_ = 0;
    std::size_t maximum_machine_frames_ = 0;
    std::size_t maximum_pre_live_machine_frames_ = 0;
    double machine_sample_rate_ = 0.0;
    double clock_ratio_ = 1.0;
    double clocked_sample_rate_ = 0.0;
    double input_ratio_ = 1.0;
    double return_ratio_ = 1.0;
    double maximum_runtime_clock_factor_ = 1.0;
    double maximum_artifact_clock_factor_ = 1.0;
    double maximum_note_pitch_factor_ = 1.0;
    double dynamic_return_latency_output_frames_ = 0.0;
    double dynamic_input_latency_machine_frames_ = 0.0;
    double dynamic_return_src_latency_output_frames_ = 0.0;
    std::size_t dynamic_identity_delay_frames_ = 0;
    std::size_t dynamic_identity_delay_head_ = 0;
    std::uint64_t prepare_epoch_ = 1;
    std::uint64_t process_sequence_ = 0;
    std::size_t last_valid_output_frames_ = 0;
};

}  // namespace pulp::audio
