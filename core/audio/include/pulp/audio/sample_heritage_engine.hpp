#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_runtime_state.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

class SampleHeritageEngine {
public:
    bool prepare(const SampleHeritagePreparedProfile& profile) noexcept {
        release();
        if (profile.schema_version != kSampleHeritageProfileSchemaVersion ||
            profile.stage_count > stages_.size() ||
            !(profile.host_sample_rate > 0.0) || !std::isfinite(profile.host_sample_rate)) {
            return false;
        }
        try {
            SampleHeritageProfile authoring{
                .schema_version = profile.schema_version,
                .profile_id = std::string(profile.id()),
                .host_sample_rate = profile.host_sample_rate,
                .stages = std::vector<SampleHeritageStageSpec>(
                    profile.stages.begin(),
                    profile.stages.begin() +
                        static_cast<std::ptrdiff_t>(profile.stage_count)),
            };
            const auto validated = validate_sample_heritage_profile(authoring);
            if (!validated.valid() ||
                validated.profile.profile_digest != profile.profile_digest) return false;
            profile_ = validated.profile;
        } catch (...) {
            return false;
        }
        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            stages_[index].spec = profile_.stages[index];
            initialize_seed(stages_[index]);
            if (const auto* filter =
                    std::get_if<SampleHeritageReconstructionFilterStage>(
                        &stages_[index].spec.parameters)) {
                stages_[index].filter_pole = static_cast<float>(
                    std::exp(-2.0 * 3.14159265358979323846 * filter->cutoff_hz /
                             profile_.host_sample_rate));
            }
        }
        prepared_ = true;
        return true;
    }

    void release() noexcept {
        profile_ = {};
        stages_ = {};
        prepared_ = false;
    }

    void reset() noexcept {
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
    }

    bool prepared() const noexcept { return prepared_; }

    bool process(BufferView<float> buffer) noexcept {
        if (!prepared_ || buffer.num_channels() == 0 ||
            buffer.num_channels() > kSampleHeritageMaximumChannels) {
            return false;
        }
        for (std::size_t index = 0; index < profile_.stage_count; ++index) {
            auto& runtime = stages_[index];
            if (runtime.spec.bypass) continue;
            std::visit([&](const auto& stage) noexcept {
                process_stage(runtime, stage, buffer);
            }, runtime.spec.parameters);
        }
        return true;
    }

    /// Captures only ContinueSerializedState RNG streams. Hold/filter DSP
    /// transients are deliberately outside the serialized contract.
    SampleHeritageRuntimeStateCapture capture_runtime_state() const noexcept {
        SampleHeritageRuntimeStateCapture result;
        if (!prepared_) return result;
        result.state.profile_schema_version = profile_.schema_version;
        std::copy(profile_.profile_id.begin(), profile_.profile_id.end(),
                  result.state.profile_id.begin());
        result.state.profile_digest = profile_.profile_digest;
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
                            std::is_same_v<Stage,
                                           SampleHeritageQuantizationStage>
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

    /// Restores RNG-stream continuation after resetting hold/filter transients;
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
                    if constexpr (std::is_same_v<Stage,
                                                 SampleHeritageNoiseStage>)
                        return {stage.seed_policy ==
                                    SampleHeritageSeedPolicy::ContinueSerializedState,
                                SampleHeritageRuntimeRngStageType::Noise};
                    return {false,
                            SampleHeritageRuntimeRngStageType::Quantization};
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

        // This is RNG-stream continuation, not sample-exact whole-engine
        // continuation. Reset hold/filter transients and restart-policy seeds,
        // then restore only explicitly opted-in random streams.
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

    static void initialize_seed(RuntimeStage& runtime) noexcept {
        std::visit([&](const auto& stage) noexcept {
            using Stage = std::decay_t<decltype(stage)>;
            if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage> ||
                          std::is_same_v<Stage, SampleHeritageQuantizationStage>)
                runtime.random_state = stage.seed;
        }, runtime.spec.parameters);
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
                              const SampleHeritageReconstructionFilterStage& stage,
                              BufferView<float> buffer) noexcept {
        (void) stage;
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
    bool prepared_ = false;
};

}  // namespace pulp::audio
