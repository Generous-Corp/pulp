#pragma once

#include "sampler_api.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_bus_dsp.hpp>
#include <pulp/audio/sample_heritage_engine.hpp>
#include <pulp/audio/sample_heritage_pitch.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::examples {

/// Off-thread profile ownership plus bounded callback processing for the
/// PulpSampler heritage path. Configuration and prepare must run while audio is
/// stopped; callback methods are allocation-free after a successful prepare.
class SamplerHeritageRuntime {
public:
    static constexpr std::size_t kVoiceSlots = 8;
    static constexpr double maximum_live_source_consumption_ratio() noexcept {
        return 1.0;
    }

    static double maximum_variable_clock_factor(
        double machine_sample_rate,
        double clock_ratio,
        double host_sample_rate) noexcept {
        const auto return_ratio =
            machine_sample_rate * clock_ratio / host_sample_rate;
        if (!(return_ratio > 0.0) || !std::isfinite(return_ratio)) return 0.0;
        auto factor = std::min(
            audio::SampleHeritagePitchProcessor::kMaximumFactor,
            audio::kMaximumDenseSampleSincConsumption / return_ratio);
        if (!(factor >= 1.0) || !std::isfinite(factor)) return 0.0;
        if (return_ratio * factor >
            audio::kMaximumDenseSampleSincConsumption) {
            factor = std::nextafter(factor, 0.0);
        }
        const auto maximum_consumption = return_ratio * factor;
        return maximum_consumption <= audio::kMaximumDenseSampleSincConsumption
            ? factor
            : 0.0;
    }

private:
    struct VoiceState {
        audio::SampleHeritageEngine engine;
        audio::SampleHeritagePitchProcessor pitch;
    };

public:
    struct VoiceProcessPlan {
        audio::SampleHeritageProcessPlan engine{};
        audio::SampleHeritagePitchPlan pitch{};
        double pitch_factor = 1.0;
        std::size_t input_frames = 0;
        bool pitch_active = false;

        bool valid() const noexcept {
            return engine.valid() && (!pitch_active || pitch.valid());
        }
    };

    static constexpr std::size_t prepared_object_bytes() noexcept {
        return sizeof(VoiceState) * kVoiceSlots +
               sizeof(audio::SampleHeritageEngine) +
               sizeof(audio::SampleHeritageBusDsp) +
               sizeof(audio::SampleSincKernelBank);
    }

    class PreparedReplacement {
    public:
        PreparedReplacement() = default;
        PreparedReplacement(PreparedReplacement&&) noexcept = default;
        PreparedReplacement& operator=(PreparedReplacement&&) noexcept = default;
        PreparedReplacement(const PreparedReplacement&) = delete;
        PreparedReplacement& operator=(const PreparedReplacement&) = delete;

    private:
        friend class SamplerHeritageRuntime;
        audio::SampleHeritagePreparedProfile profile{};
        std::unique_ptr<audio::SampleSincKernelBank> sinc_bank;
        std::unique_ptr<VoiceState[]> voices;
        std::unique_ptr<audio::SampleHeritageBusDsp> bus_dsp;
        std::unique_ptr<audio::SampleHeritageEngine> legacy_engine;
        std::vector<float> dry_storage;
        std::vector<float> pitch_storage;
        audio::SampleHeritageProfileStatus profile_status =
            audio::SampleHeritageProfileStatus::Ok;
        std::size_t channel_count = 0;
        std::size_t maximum_input_frames = 0;
        std::size_t maximum_engine_input_frames = 0;
        std::size_t maximum_stream_input_frames = 0;
        double clock_ratio = 1.0;
        double machine_sample_rate = 0.0;
        double nominal_latency_frames = 0.0;
        std::uint32_t reported_latency_frames = 0;
        double host_sample_rate = 0.0;
        bool typed = false;
        bool runtime_continuation_required = false;
        bool voice_processing_required = false;
        bool voice_artifact_path_active = false;
        bool pitch_active = false;
        audio::SampleHeritagePitchFamily pitch_family =
            audio::SampleHeritagePitchFamily::VariableClock;
        bool bus_processing_required = false;
        bool legacy_processing_required = false;
        bool all_stages_bypassed = true;
        audio::SampleHeritageRuntimeStateStatus runtime_state_status =
            audio::SampleHeritageRuntimeStateStatus::NotPrepared;
    };

    SamplerHeritageRuntime()
        : voices_(std::make_unique<VoiceState[]>(kVoiceSlots)),
          bus_dsp_(std::make_unique<audio::SampleHeritageBusDsp>()),
          legacy_engine_(std::make_unique<audio::SampleHeritageEngine>()) {}

    PulpSamplerHeritageStatus configure(
        const audio::SampleHeritageProfile& profile) {
        const auto validation = audio::validate_sample_heritage_profile(profile);
        if (!validation.valid())
            return PulpSamplerHeritageStatus::InvalidProfile;

        release_processing();
        configured_ = validation.profile;
        profile_status_ = validation.status;
        configured_valid_ = true;
        configured_typed_ = has_typed_blocks(configured_);
        all_stages_bypassed_ = profile_all_bypassed(configured_);
        status_.store(PulpSamplerHeritageStatus::PendingPrepare,
                      std::memory_order_relaxed);
        return PulpSamplerHeritageStatus::PendingPrepare;
    }

    void disable() noexcept {
        release_processing();
        configured_ = {};
        configured_valid_ = false;
        configured_typed_ = false;
        all_stages_bypassed_ = true;
        profile_status_ = audio::SampleHeritageProfileStatus::Ok;
        status_.store(PulpSamplerHeritageStatus::Disabled,
                      std::memory_order_relaxed);
    }

    PulpSamplerHeritageStatus prepare(
        double host_sample_rate,
        std::size_t channel_count,
        std::size_t maximum_output_frames,
        double maximum_stream_pitch_factor,
        const audio::SampleHeritageTypedRuntimeState* runtime_state = nullptr) noexcept {
        if (!configured_valid_) {
            release_processing();
            status_.store(PulpSamplerHeritageStatus::Disabled,
                          std::memory_order_relaxed);
            return PulpSamplerHeritageStatus::Disabled;
        }

        PreparedReplacement candidate;
        const auto result = prepare_candidate(
            configured_, profile_status_, host_sample_rate, channel_count,
            maximum_output_frames, maximum_stream_pitch_factor, runtime_state,
            candidate);
        if (result != PulpSamplerHeritageStatus::Ready &&
            result != PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate) {
            release_processing();
            status_.store(PulpSamplerHeritageStatus::PrepareFailed,
                          std::memory_order_relaxed);
            return PulpSamplerHeritageStatus::PrepareFailed;
        }
        publish_candidate(std::move(candidate), result);
        return result;
    }

    PulpSamplerHeritageStatus stage_replacement(
        const audio::SampleHeritageProfile& profile,
        double host_sample_rate,
        std::size_t channel_count,
        std::size_t maximum_output_frames,
        double maximum_stream_pitch_factor,
        const audio::SampleHeritageTypedRuntimeState* runtime_state,
        PreparedReplacement& candidate) noexcept {
        const auto validation = audio::validate_sample_heritage_profile(profile);
        if (!validation.valid()) return PulpSamplerHeritageStatus::InvalidProfile;
        return prepare_candidate(
            validation.profile, validation.status, host_sample_rate,
            channel_count, maximum_output_frames, maximum_stream_pitch_factor,
            runtime_state, candidate);
    }

    static double replacement_clock_ratio(
        const PreparedReplacement& candidate) noexcept {
        return candidate.clock_ratio;
    }

    static std::size_t replacement_maximum_input_frames(
        const PreparedReplacement& candidate,
        std::size_t maximum_output_frames) noexcept {
        return candidate.all_stages_bypassed
            ? maximum_output_frames
            : candidate.maximum_stream_input_frames;
    }

    void publish_replacement(PreparedReplacement&& candidate,
                             PulpSamplerHeritageStatus ready) noexcept {
        publish_candidate(std::move(candidate), ready);
    }

    void release_processing() noexcept {
        if (voices_ != nullptr) {
            for (std::size_t slot = 0; slot < kVoiceSlots; ++slot) {
                voices_[slot].engine.release();
                voices_[slot].pitch.release();
            }
        }
        if (bus_dsp_ != nullptr) bus_dsp_->release();
        if (legacy_engine_ != nullptr) legacy_engine_->release();
        if (sinc_bank_ != nullptr) sinc_bank_->release();
        std::vector<float>().swap(dry_storage_);
        std::vector<float>().swap(pitch_storage_);
        dry_ptrs_.fill(nullptr);
        dry_const_ptrs_.fill(nullptr);
        pitch_ptrs_.fill(nullptr);
        pitch_const_ptrs_.fill(nullptr);
        prepared_ = false;
        failed_closed_ = false;
        typed_ = false;
        voice_processing_required_ = false;
        voice_artifact_path_active_ = false;
        pitch_active_ = false;
        pitch_family_ = audio::SampleHeritagePitchFamily::VariableClock;
        bus_processing_required_ = false;
        legacy_processing_required_ = false;
        channel_count_ = 0;
        maximum_input_frames_ = 0;
        maximum_engine_input_frames_ = 0;
        maximum_stream_input_frames_ = 0;
        clock_ratio_ = 1.0;
        machine_sample_rate_ = 0.0;
        nominal_latency_frames_ = 0.0;
        reported_latency_frames_ = 0;
        host_sample_rate_ = 0.0;
        runtime_continuation_required_ = false;
        runtime_state_status_.store(
            audio::SampleHeritageRuntimeStateStatus::NotPrepared,
            std::memory_order_relaxed);
    }

    bool processing_required() const noexcept {
        return prepared_ && !all_stages_bypassed_ && !failed_closed_;
    }

    bool direct_path_allowed() const noexcept {
        return !configured_valid_ || (prepared_ && all_stages_bypassed_);
    }

    bool configured() const noexcept { return configured_valid_; }
    bool typed_profile() const noexcept { return configured_valid_ && configured_typed_; }
    bool all_stages_bypassed() const noexcept { return all_stages_bypassed_; }
    bool voice_processing_required() const noexcept {
        return prepared_ && voice_processing_required_ && !failed_closed_;
    }
    bool bus_processing_required() const noexcept {
        return prepared_ && bus_processing_required_ && !failed_closed_;
    }
    bool voice_artifact_path_active() const noexcept {
        return prepared_ && voice_artifact_path_active_ && !failed_closed_;
    }
    bool pitch_active() const noexcept {
        return prepared_ && pitch_active_ && !failed_closed_;
    }
    bool runtime_pitch_factor_supported(double factor) const noexcept {
        if (!pitch_active()) return true;
        if (!std::isfinite(factor) ||
            factor < audio::SampleHeritagePitchProcessor::kMinimumFactor ||
            factor > audio::SampleHeritagePitchProcessor::kMaximumFactor) {
            return false;
        }
        if (pitch_family_ != audio::SampleHeritagePitchFamily::VariableClock)
            return true;
        const auto maximum = maximum_variable_clock_factor(
            machine_sample_rate_, clock_ratio_, host_sample_rate_);
        return maximum >= 1.0 && factor >= 1.0 / maximum &&
               factor <= maximum;
    }
    audio::SampleHeritagePitchFamily pitch_family() const noexcept {
        return pitch_family_;
    }
    std::uint64_t voice_tail_output_frames() const noexcept {
        return voice_processing_required() && voices_ != nullptr
            ? voices_[0].engine.tail_output_frames()
            : 0;
    }

    audio::SampleHeritageProfile configured_profile() const {
        audio::SampleHeritageProfile result;
        if (!configured_valid_) return result;
        result.schema_version = configured_.schema_version;
        result.profile_id.assign(configured_.id());
        result.host_sample_rate = configured_.host_sample_rate;
        result.voice.assign(configured_.voice.begin(),
                            configured_.voice.begin() + configured_.voice_count);
        result.bus.assign(configured_.bus.begin(),
                          configured_.bus.begin() + configured_.bus_count);
        result.record_commit.assign(
            configured_.record_commit.begin(),
            configured_.record_commit.begin() + configured_.record_commit_count);
        result.stages.assign(configured_.stages.begin(),
                             configured_.stages.begin() + configured_.stage_count);
        return result;
    }

    double active_clock_ratio() const noexcept {
        return prepared_ && !all_stages_bypassed_ ? clock_ratio_ : 1.0;
    }
    double active_live_source_consumption_ratio() const noexcept {
        return maximum_live_source_consumption_ratio();
    }
    std::size_t maximum_input_frames() const noexcept {
        return prepared_ && !all_stages_bypassed_ ? maximum_input_frames_ : 0;
    }
    std::size_t maximum_stream_input_frames() const noexcept {
        return prepared_ && !all_stages_bypassed_
            ? maximum_stream_input_frames_
            : 0;
    }
#if defined(PULP_SAMPLER_TEST_HOOKS)
    bool processing_released_for_test() const noexcept {
        return !prepared_ && dry_storage_.empty() && maximum_input_frames_ == 0;
    }
#endif
    int latency_samples() const noexcept {
        return reported_latency_frames_ >
                static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(reported_latency_frames_);
    }

    bool plan(std::size_t output_frames,
              audio::SampleHeritageProcessPlan& result) noexcept {
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (fail_next_plan_for_test_.exchange(false, std::memory_order_relaxed)) {
            fail_plan();
            return false;
        }
#endif
        if (!processing_required() || typed_ || !legacy_processing_required_)
            return false;
        result = legacy_engine_->plan_exact(output_frames);
        if (result.valid()) return true;
        fail_plan();
        return false;
    }

    audio::BufferView<float> dry_buffer(std::size_t frames) noexcept {
        return {dry_ptrs_.data(), channel_count_, frames};
    }

    audio::BufferView<const float> dry_input(std::size_t frames) const noexcept {
        return {dry_const_ptrs_.data(), channel_count_, frames};
    }

    bool process(const audio::SampleHeritageProcessPlan& plan,
                 audio::BufferView<float> output) noexcept {
        if (!processing_required() || typed_ || !legacy_processing_required_ ||
            plan.input_frames > maximum_input_frames_) {
            fail_process();
            return false;
        }
        if (legacy_engine_->process_exact(plan, dry_input(plan.input_frames), output) ==
            audio::SampleHeritageProcessStatus::Ok) {
            return true;
        }
        fail_process();
        return false;
    }

    bool plan_voice(std::size_t slot,
                    std::size_t output_frames,
                    double pitch_factor,
                    VoiceProcessPlan& result) noexcept {
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (fail_next_plan_for_test_.exchange(false, std::memory_order_relaxed)) {
            fail_plan();
            return false;
        }
#endif
        if (slot >= kVoiceSlots || !voice_processing_required()) return false;
        result = {};
        result.pitch_factor = pitch_factor;
        result.pitch_active = pitch_active_;
        if (pitch_active_ &&
            voices_[slot].pitch.set_factor(pitch_factor) !=
                audio::SampleHeritagePitchStatus::Ok) {
            fail_plan();
            return false;
        }
        const auto clock_multiplier = pitch_active_ &&
                pitch_family_ == audio::SampleHeritagePitchFamily::VariableClock
            ? pitch_factor
            : 1.0;
        result.engine = voices_[slot].engine.plan_exact(
            output_frames, clock_multiplier);
        if (!result.engine.valid()) {
            fail_plan();
            return false;
        }
        if (pitch_active_) {
            result.pitch = voices_[slot].pitch.plan(result.engine.input_frames);
            if (!result.pitch.valid() ||
                result.pitch.input_frames > maximum_input_frames_) {
                fail_plan();
                return false;
            }
            result.input_frames = result.pitch.input_frames;
        } else {
            result.input_frames = result.engine.input_frames;
        }
        if (result.valid()) return true;
        fail_plan();
        return false;
    }

    bool plan_voice_tail(std::size_t slot,
                         std::size_t output_frames,
                         double pitch_factor,
                         VoiceProcessPlan& result) noexcept {
        if (slot >= kVoiceSlots || !voice_processing_required()) return false;
        result = {};
        result.pitch_factor = pitch_factor;
        const auto clock_multiplier = pitch_active_ &&
                pitch_family_ == audio::SampleHeritagePitchFamily::VariableClock
            ? pitch_factor
            : 1.0;
        result.engine = voices_[slot].engine.plan_exact(
            output_frames, clock_multiplier);
        if (!result.engine.valid() ||
            result.engine.input_frames > maximum_input_frames_) {
            fail_plan();
            return false;
        }
        result.input_frames = result.engine.input_frames;
        return true;
    }

    audio::BufferView<float> voice_dry_buffer(std::size_t slot,
                                               std::size_t frames) noexcept {
        if (slot >= kVoiceSlots || frames > maximum_input_frames_) return {};
        return dry_buffer(frames);
    }

    bool process_voice(std::size_t slot,
                       const VoiceProcessPlan& plan,
                       audio::BufferView<float> output) noexcept {
        if (slot >= kVoiceSlots || !voice_processing_required() ||
            !plan.valid() || plan.input_frames > maximum_input_frames_ ||
            plan.engine.input_frames > maximum_engine_input_frames_ ||
            (plan.pitch_active &&
             plan.pitch_factor != voices_[slot].pitch.factor())) {
            fail_process();
            return false;
        }
        auto engine_input = dry_input(plan.engine.input_frames);
        if (plan.pitch_active) {
            audio::BufferView<float> pitched(
                pitch_ptrs_.data(), channel_count_, plan.engine.input_frames);
            if (voices_[slot].pitch.process(
                    dry_input(plan.input_frames), pitched) !=
                audio::SampleHeritagePitchStatus::Ok) {
                fail_process();
                return false;
            }
            engine_input = audio::BufferView<const float>(
                pitch_const_ptrs_.data(), channel_count_, plan.engine.input_frames);
        }
        if (voices_[slot].engine.process_exact(
                plan.engine, engine_input, output) ==
            audio::SampleHeritageProcessStatus::Ok) {
            return true;
        }
        fail_process();
        return false;
    }

    bool process_voice_tail(std::size_t slot,
                            const VoiceProcessPlan& plan,
                            audio::BufferView<float> output) noexcept {
        if (slot >= kVoiceSlots || !voice_processing_required() ||
            !plan.valid() || plan.pitch_active ||
            plan.input_frames != plan.engine.input_frames ||
            plan.input_frames > maximum_input_frames_) {
            fail_process();
            return false;
        }
        if (voices_[slot].engine.process_tail_exact(
                plan.engine, dry_input(plan.input_frames), output) ==
            audio::SampleHeritageProcessStatus::Ok) {
            return true;
        }
        fail_process();
        return false;
    }

    void reset_voice(std::size_t slot) noexcept {
        if (slot < kVoiceSlots && voices_ != nullptr) {
            voices_[slot].engine.reset();
            voices_[slot].pitch.reset();
        }
    }

    bool process_bus(audio::BufferView<float> output,
                     std::span<const std::uint8_t> voice_activity) noexcept {
        if (!bus_processing_required() || output.num_channels() != channel_count_ ||
            output.num_samples() > maximum_input_frames_ ||
            voice_activity.size() != output.num_samples()) {
            fail_process();
            return false;
        }
        if (bus_dsp_->process(output, voice_activity) ==
            audio::SampleHeritageBusDspStatus::Ok) {
            return true;
        }
        fail_process();
        return false;
    }

    void reject_process() noexcept { fail_process(); }

    audio::SampleHeritageTypedRuntimeStateCapture
    capture_runtime_state() const noexcept {
        audio::SampleHeritageTypedRuntimeStateCapture result;
        if (!prepared_ || !typed_ || !runtime_continuation_required_)
            return result;
        result.state.profile_schema_version = configured_.schema_version;
        std::copy(configured_.profile_id.begin(), configured_.profile_id.end(),
                  result.state.profile_id.begin());
        result.state.profile_digest = configured_.profile_digest;
        result.state.host_sample_rate = host_sample_rate_;
        if (voice_processing_required_) {
            for (std::size_t slot = 0; slot < kVoiceSlots; ++slot) {
                const auto captured = voices_[slot].engine.capture_runtime_state();
                if (!captured.valid()) return {};
                copy_engine_state(captured.state,
                                  result.state.voice_states[slot].engine);
            }
        }
        if (bus_processing_required_) {
            if (bus_dsp_->capture_runtime_state(result.state.bus_state) !=
                audio::SampleHeritageRuntimeStateStatus::Ok)
                return {};
        }
        result.status = audio::SampleHeritageRuntimeStateStatus::Ok;
        return result;
    }

    void record_rate_admission_rejection() noexcept {
        rate_admission_rejections_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_rate_automation_rejection() noexcept {
        rate_automation_rejections_.fetch_add(1, std::memory_order_relaxed);
    }

    PulpSamplerHeritageDiagnostics diagnostics() const noexcept {
        PulpSamplerHeritageDiagnostics result;
        result.status = status_.load(std::memory_order_relaxed);
        result.profile_status = profile_status_;
        result.runtime_state_status =
            runtime_state_status_.load(std::memory_order_relaxed);
        result.all_stages_bypassed = all_stages_bypassed_;
        result.render_plan_failures =
            render_plan_failures_.load(std::memory_order_relaxed);
        result.render_failures = render_failures_.load(std::memory_order_relaxed);
        result.rate_admission_rejections =
            rate_admission_rejections_.load(std::memory_order_relaxed);
        result.rate_automation_rejections =
            rate_automation_rejections_.load(std::memory_order_relaxed);
        if (!configured_valid_) return result;
        std::copy(configured_.profile_id.begin(), configured_.profile_id.end(),
                  result.profile_id.begin());
        result.profile_digest = configured_.profile_digest;
        result.machine_sample_rate = machine_sample_rate_;
        result.clock_ratio = clock_ratio_;
        result.nominal_latency_frames = nominal_latency_frames_;
        result.reported_latency_frames = reported_latency_frames_;
        return result;
    }

#if defined(PULP_SAMPLER_TEST_HOOKS)
    void fail_next_plan_for_test() noexcept {
        fail_next_plan_for_test_.store(true, std::memory_order_relaxed);
    }
#endif

private:
    static bool has_typed_blocks(
        const audio::SampleHeritagePreparedProfile& profile) noexcept {
        return profile.voice_count != 0 || profile.bus_count != 0 ||
               profile.record_commit_count != 0;
    }

    static bool profile_all_bypassed(
        const audio::SampleHeritagePreparedProfile& profile) noexcept {
        bool result = true;
        for (std::size_t index = 0; index < profile.stage_count; ++index)
            result = result && profile.stages[index].bypass;
        for (std::size_t index = 0; index < profile.voice_count; ++index)
            result = result && profile.voice[index].bypass;
        for (std::size_t index = 0; index < profile.bus_count; ++index)
            result = result && profile.bus[index].bypass;
        return result;
    }

    static bool profile_has_continuable_state(
        const audio::SampleHeritagePreparedProfile& profile) noexcept {
        for (std::size_t index = 0; index < profile.voice_count; ++index) {
            if (profile.voice[index].bypass) continue;
            if (const auto* converter =
                    std::get_if<audio::SampleHeritageVoiceConverterBlock>(
                        &profile.voice[index].parameters);
                converter != nullptr &&
                converter->seed_policy ==
                    audio::SampleHeritageSeedPolicy::ContinueSerializedState)
                return true;
        }
        for (std::size_t index = 0; index < profile.bus_count; ++index) {
            if (profile.bus[index].bypass) continue;
            if (const auto* noise =
                    std::get_if<audio::SampleHeritageBusNoiseIdleBlock>(
                        &profile.bus[index].parameters);
                noise != nullptr &&
                noise->seed_policy ==
                    audio::SampleHeritageSeedPolicy::ContinueSerializedState)
                return true;
        }
        return false;
    }

    static bool typed_runtime_matches_profile(
        const audio::SampleHeritageTypedRuntimeState& state,
        const audio::SampleHeritagePreparedProfile& profile) noexcept {
        if (!(state.schema_version ==
                   audio::kSampleHeritageTypedRuntimeStateSchemaVersion &&
               state.profile_schema_version == profile.schema_version &&
               state.bound_profile_id() == profile.id() &&
               state.profile_digest_version ==
                   audio::kSampleHeritageProfileDigestVersion &&
               state.profile_digest == profile.profile_digest &&
               std::isfinite(state.host_sample_rate) &&
               state.host_sample_rate >= 8000.0 &&
               state.host_sample_rate <= 384000.0))
            return false;
        for (std::size_t index = 0; index < state.voice_states.size(); ++index) {
            if (state.voice_states[index].slot_index != index)
                return false;
        }
        return true;
    }

    static void copy_engine_state(
        const audio::SampleHeritageRuntimeState& source,
        audio::SampleHeritageRuntimeEngineState& destination) noexcept {
        destination.rng_states = source.rng_states;
        destination.rng_state_count = source.rng_state_count;
    }

    static audio::SampleHeritageRuntimeState engine_restore_state(
        const audio::SampleHeritagePreparedProfile& profile,
        const audio::SampleHeritageRuntimeEngineState& source) noexcept {
        audio::SampleHeritageRuntimeState result;
        result.profile_schema_version = profile.schema_version;
        std::copy(profile.profile_id.begin(), profile.profile_id.end(),
                  result.profile_id.begin());
        result.profile_digest = profile.profile_digest;
        result.rng_states = source.rng_states;
        result.rng_state_count = source.rng_state_count;
        return result;
    }

    static bool inspect_voice_block(
        const audio::SampleHeritageVoiceBlockSpec& spec,
        bool& artifact_path_active,
        bool& pitch_active,
        audio::SampleHeritagePitchFamily& pitch_family) {
        if (spec.bypass) return true;
        return std::visit(
            [&](const auto& block) {
                using Block = std::decay_t<decltype(block)>;
                if constexpr (std::is_same_v<
                                         Block,
                                         audio::SampleHeritageVoicePitchBlock>) {
                    artifact_path_active = true;
                    pitch_active = true;
                    pitch_family = block.family;
                    return true;
                } else if constexpr (std::is_same_v<
                                         Block,
                                         audio::SampleHeritageVoiceLiveCyclicStretchBlock>) {
                    // H8 owns the realtime live-stretch implementation. Keep
                    // an active block fail-closed until that processor exists.
                    return false;
                } else {
                    return true;
                }
            },
            spec.parameters);
    }

    static bool build_typed_runtime_profiles(
        const audio::SampleHeritagePreparedProfile& profile,
        double host_sample_rate,
        audio::SampleHeritagePreparedProfile& voice_profile,
        bool& voice_artifact_path_active,
        bool& pitch_active,
        audio::SampleHeritagePitchFamily& pitch_family) {
        try {
            for (std::size_t index = 0; index < profile.voice_count; ++index) {
                if (!inspect_voice_block(profile.voice[index],
                                         voice_artifact_path_active,
                                         pitch_active, pitch_family))
                    return false;
            }
            audio::SampleHeritageProfile native_voice;
            native_voice.schema_version = profile.schema_version;
            native_voice.profile_id.assign(profile.id());
            native_voice.host_sample_rate = host_sample_rate;
            native_voice.voice.assign(
                profile.voice.begin(), profile.voice.begin() + profile.voice_count);
            const auto voice_validation =
                audio::validate_sample_heritage_profile(native_voice);
            if (!voice_validation.valid()) return false;
            voice_profile = voice_validation.profile;
            return true;
        } catch (...) {
            return false;
        }
    }

    static bool checked_storage_size(std::size_t channel_count,
                                     std::size_t frames) noexcept {
        return frames <= std::numeric_limits<std::uint32_t>::max() &&
               (channel_count == 0 ||
                frames <= std::numeric_limits<std::size_t>::max() / channel_count);
    }

    static bool voice_profile_for_slot(
        const audio::SampleHeritagePreparedProfile& source,
        std::size_t slot,
        audio::SampleHeritagePreparedProfile& result) noexcept {
        try {
            audio::SampleHeritageProfile profile;
            profile.schema_version = source.schema_version;
            profile.profile_id.assign(source.id());
            profile.host_sample_rate = source.host_sample_rate;
            profile.voice.assign(source.voice.begin(),
                                 source.voice.begin() + source.voice_count);
            for (auto& spec : profile.voice) {
                auto* converter =
                    std::get_if<audio::SampleHeritageVoiceConverterBlock>(
                        &spec.parameters);
                if (converter == nullptr || converter->seed == 0) continue;
                converter->seed ^=
                    UINT64_C(0x9e3779b97f4a7c15) * (slot + 1);
                if (converter->seed == 0)
                    converter->seed = UINT64_C(0xd1b54a32d192ed03) ^ slot;
            }
            const auto validation = audio::validate_sample_heritage_profile(
                profile);
            if (!validation.valid()) return false;
            result = validation.profile;
            return true;
        } catch (...) {
            return false;
        }
    }

    static PulpSamplerHeritageStatus prepare_candidate(
        const audio::SampleHeritagePreparedProfile& profile,
        audio::SampleHeritageProfileStatus profile_status,
        double host_sample_rate,
        std::size_t channel_count,
        std::size_t maximum_output_frames,
        double maximum_stream_pitch_factor,
        const audio::SampleHeritageTypedRuntimeState* runtime_state,
        PreparedReplacement& candidate) noexcept {
        candidate = {};
        if (host_sample_rate < 8000.0 || host_sample_rate > 384000.0 ||
            !std::isfinite(host_sample_rate) ||
            channel_count == 0 ||
            channel_count > audio::kSampleHeritageMaximumChannels ||
            maximum_output_frames == 0 ||
            !(maximum_stream_pitch_factor > 0.0) ||
            !std::isfinite(maximum_stream_pitch_factor)) {
            return PulpSamplerHeritageStatus::PrepareFailed;
        }
        candidate.profile = profile;
        candidate.profile_status = profile_status;
        candidate.channel_count = channel_count;
        candidate.host_sample_rate = host_sample_rate;
        candidate.typed = has_typed_blocks(profile);
        candidate.runtime_continuation_required =
            candidate.typed && profile_has_continuable_state(profile);
        candidate.all_stages_bypassed = profile_all_bypassed(profile);
        if (runtime_state != nullptr &&
            (!candidate.typed ||
             !typed_runtime_matches_profile(*runtime_state, profile)))
            return PulpSamplerHeritageStatus::RuntimeStateRejected;
        const auto host_rate_changed = runtime_state != nullptr
            ? runtime_state->host_sample_rate != host_sample_rate
            : profile.host_sample_rate != host_sample_rate;

        try {
            candidate.sinc_bank =
                std::make_unique<audio::SampleSincKernelBank>();
            candidate.voices = std::make_unique<VoiceState[]>(kVoiceSlots);
            candidate.bus_dsp =
                std::make_unique<audio::SampleHeritageBusDsp>();
            candidate.legacy_engine =
                std::make_unique<audio::SampleHeritageEngine>();
        } catch (...) {
            return PulpSamplerHeritageStatus::PrepareFailed;
        }

        if (candidate.typed) {
            audio::SampleHeritagePreparedProfile voice_profile;
            if (!build_typed_runtime_profiles(
                    profile, host_sample_rate, voice_profile,
                    candidate.voice_artifact_path_active,
                    candidate.pitch_active, candidate.pitch_family)) {
                return PulpSamplerHeritageStatus::PrepareFailed;
            }
            candidate.voice_processing_required = false;
            for (std::size_t index = 0; index < voice_profile.voice_count; ++index)
                candidate.voice_processing_required =
                    candidate.voice_processing_required ||
                    !voice_profile.voice[index].bypass;
            candidate.bus_processing_required = false;
            for (std::size_t index = 0; index < profile.bus_count; ++index)
                candidate.bus_processing_required =
                    candidate.bus_processing_required ||
                    !profile.bus[index].bypass;

            if (candidate.voice_processing_required) {
                double machine_rate = host_sample_rate;
                double clock_ratio = 1.0;
                for (std::size_t index = 0; index < voice_profile.voice_count; ++index) {
                    const auto& spec = voice_profile.voice[index];
                    if (spec.bypass) continue;
                    if (const auto* machine =
                            std::get_if<audio::SampleHeritageVoiceMachineDomainBlock>(
                                &spec.parameters))
                        machine_rate = machine->sample_rate;
                    else if (const auto* clock =
                                 std::get_if<audio::SampleHeritageVoiceClockBlock>(
                                     &spec.parameters))
                        clock_ratio = clock->ratio;
                }
                const auto maximum_runtime_clock_factor =
                    candidate.pitch_active &&
                            candidate.pitch_family ==
                                audio::SampleHeritagePitchFamily::VariableClock
                        ? maximum_variable_clock_factor(
                              machine_rate, clock_ratio, host_sample_rate)
                        : 1.0;
                if (!(maximum_runtime_clock_factor >= 1.0))
                    return PulpSamplerHeritageStatus::PrepareFailed;
                const auto maximum_consumption = std::max(
                    host_sample_rate / machine_rate,
                    machine_rate * clock_ratio / host_sample_rate *
                        maximum_runtime_clock_factor);
                if (!candidate.sinc_bank->build_dense_for_maximum_consumption(
                        maximum_consumption))
                    return PulpSamplerHeritageStatus::PrepareFailed;
                const auto bank_view = candidate.sinc_bank->view();
                for (std::size_t slot = 0; slot < kVoiceSlots; ++slot) {
                    audio::SampleHeritagePreparedProfile slot_profile;
                    if (!voice_profile_for_slot(voice_profile, slot,
                                                slot_profile))
                        return PulpSamplerHeritageStatus::PrepareFailed;
                    if (candidate.voices[slot].engine.prepare({
                            .profile = slot_profile,
                            .channel_count = channel_count,
                            .maximum_output_frames = maximum_output_frames,
                            .external_sinc_bank = &bank_view,
                            .maximum_runtime_clock_factor =
                                candidate.pitch_active &&
                                        candidate.pitch_family ==
                                            audio::SampleHeritagePitchFamily::VariableClock
                                    ? maximum_runtime_clock_factor
                                    : 1.0}) !=
                        audio::SampleHeritagePrepareStatus::Ok)
                        return PulpSamplerHeritageStatus::PrepareFailed;
                    if (candidate.voices[slot].pitch.prepare(
                            candidate.pitch_family, 1.0, channel_count) !=
                        audio::SampleHeritagePitchStatus::Ok)
                        return PulpSamplerHeritageStatus::PrepareFailed;
                    if (runtime_state != nullptr && !host_rate_changed) {
                        candidate.runtime_state_status =
                            candidate.voices[slot].engine.restore_runtime_state(
                                engine_restore_state(
                                    slot_profile,
                                    runtime_state->voice_states[slot].engine));
                        if (candidate.runtime_state_status !=
                            audio::SampleHeritageRuntimeStateStatus::Ok)
                            return PulpSamplerHeritageStatus::RuntimeStateRejected;
                    }
                }
                candidate.clock_ratio =
                    candidate.voices[0].engine.clock_ratio();
                candidate.machine_sample_rate =
                    candidate.voices[0].engine.machine_sample_rate();
                candidate.maximum_engine_input_frames =
                    candidate.voices[0].engine.maximum_input_frames();
                candidate.maximum_input_frames =
                    candidate.maximum_engine_input_frames;
                candidate.maximum_stream_input_frames =
                    candidate.maximum_engine_input_frames;
                const auto admitted_stream_pitch_factor = std::min(
                    maximum_stream_pitch_factor,
                    maximum_stream_pitch_factor /
                        (clock_ratio *
                         maximum_live_source_consumption_ratio()));
                if (candidate.pitch_active &&
                    candidate.pitch_family !=
                        audio::SampleHeritagePitchFamily::VariableClock) {
                    if (candidate.voices[0].pitch.set_factor(
                            audio::SampleHeritagePitchProcessor::kMaximumFactor) !=
                        audio::SampleHeritagePitchStatus::Ok)
                        return PulpSamplerHeritageStatus::PrepareFailed;
                    const auto pitch_plan = candidate.voices[0].pitch.plan(
                        candidate.maximum_engine_input_frames);
                    candidate.voices[0].pitch.set_factor(1.0);
                    candidate.voices[0].pitch.reset();
                    if (!pitch_plan.valid())
                        return PulpSamplerHeritageStatus::PrepareFailed;
                    candidate.maximum_input_frames = pitch_plan.input_frames;
                    if (candidate.voices[0].pitch.set_factor(
                            admitted_stream_pitch_factor) !=
                        audio::SampleHeritagePitchStatus::Ok)
                        return PulpSamplerHeritageStatus::PrepareFailed;
                    const auto stream_pitch_plan = candidate.voices[0].pitch.plan(
                        candidate.maximum_engine_input_frames);
                    candidate.voices[0].pitch.set_factor(1.0);
                    candidate.voices[0].pitch.reset();
                    if (!stream_pitch_plan.valid())
                        return PulpSamplerHeritageStatus::PrepareFailed;
                    candidate.maximum_stream_input_frames =
                        stream_pitch_plan.input_frames;
                } else if (candidate.pitch_active) {
                    const auto stream_engine_plan =
                        candidate.voices[0].engine.plan_exact(
                            maximum_output_frames,
                            admitted_stream_pitch_factor);
                    if (!stream_engine_plan.valid())
                        return PulpSamplerHeritageStatus::PrepareFailed;
                    candidate.maximum_stream_input_frames =
                        stream_engine_plan.input_frames;
                }
            } else {
                if (runtime_state != nullptr && !host_rate_changed) {
                    for (const auto& voice : runtime_state->voice_states) {
                        if (voice.engine.rng_state_count != 0)
                            return PulpSamplerHeritageStatus::RuntimeStateRejected;
                    }
                }
                candidate.clock_ratio = 1.0;
                candidate.machine_sample_rate = host_sample_rate;
                candidate.maximum_input_frames = maximum_output_frames;
                candidate.maximum_engine_input_frames = maximum_output_frames;
                candidate.maximum_stream_input_frames = maximum_output_frames;
            }

            if (candidate.bus_processing_required) {
                if (candidate.bus_dsp->prepare(profile, host_sample_rate,
                                               channel_count) !=
                    audio::SampleHeritageBusDspStatus::Ok)
                    return PulpSamplerHeritageStatus::PrepareFailed;
                if (runtime_state != nullptr && !host_rate_changed) {
                    candidate.runtime_state_status =
                        candidate.bus_dsp->restore_runtime_state(
                            runtime_state->bus_state);
                    if (candidate.runtime_state_status !=
                        audio::SampleHeritageRuntimeStateStatus::Ok)
                        return PulpSamplerHeritageStatus::RuntimeStateRejected;
                }
                candidate.maximum_input_frames = std::max(
                    candidate.maximum_input_frames, maximum_output_frames);
            } else if (runtime_state != nullptr && !host_rate_changed &&
                       runtime_state->bus_state.rng_state_count != 0) {
                return PulpSamplerHeritageStatus::RuntimeStateRejected;
            }
            if (runtime_state != nullptr && !host_rate_changed)
                candidate.runtime_state_status =
                    audio::SampleHeritageRuntimeStateStatus::Ok;
            const auto voice_latency = candidate.voice_processing_required
                ? candidate.voices[0].engine.latency_output_frames()
                : 0.0;
            candidate.nominal_latency_frames = voice_latency;
        } else {
            auto runtime_profile = profile;
            runtime_profile.host_sample_rate = host_sample_rate;
            if (candidate.legacy_engine->prepare(
                    {runtime_profile, channel_count, maximum_output_frames}) !=
                audio::SampleHeritagePrepareStatus::Ok)
                return PulpSamplerHeritageStatus::PrepareFailed;
            candidate.legacy_processing_required =
                !candidate.all_stages_bypassed;
            candidate.clock_ratio = candidate.all_stages_bypassed
                ? 1.0
                : candidate.legacy_engine->clock_ratio();
            candidate.machine_sample_rate =
                candidate.legacy_engine->machine_sample_rate();
            candidate.maximum_input_frames =
                candidate.legacy_engine->maximum_input_frames();
            candidate.maximum_engine_input_frames =
                candidate.maximum_input_frames;
            candidate.maximum_stream_input_frames =
                candidate.maximum_input_frames;
            candidate.nominal_latency_frames =
                candidate.legacy_engine->latency_output_frames();
        }

        if (!candidate.all_stages_bypassed) {
            if (!checked_storage_size(channel_count,
                                      candidate.maximum_input_frames))
                return PulpSamplerHeritageStatus::PrepareFailed;
            try {
                candidate.dry_storage.assign(
                    channel_count * candidate.maximum_input_frames, 0.0f);
                candidate.pitch_storage.assign(
                    channel_count * candidate.maximum_engine_input_frames,
                    0.0f);
            } catch (...) {
                return PulpSamplerHeritageStatus::PrepareFailed;
            }
        }
        const auto rounded_latency = std::ceil(candidate.nominal_latency_frames);
        candidate.reported_latency_frames = rounded_latency >=
                static_cast<double>(std::numeric_limits<std::uint32_t>::max())
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(rounded_latency);
        return host_rate_changed
            ? PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate
            : PulpSamplerHeritageStatus::Ready;
    }

    void publish_candidate(PreparedReplacement&& candidate,
                           PulpSamplerHeritageStatus ready) noexcept {
        release_processing();
        configured_ = candidate.profile;
        profile_status_ = candidate.profile_status;
        configured_valid_ = true;
        configured_typed_ = candidate.typed;
        all_stages_bypassed_ = candidate.all_stages_bypassed;
        typed_ = candidate.typed;
        voice_processing_required_ = candidate.voice_processing_required;
        voice_artifact_path_active_ = candidate.voice_artifact_path_active;
        pitch_active_ = candidate.pitch_active;
        pitch_family_ = candidate.pitch_family;
        bus_processing_required_ = candidate.bus_processing_required;
        legacy_processing_required_ = candidate.legacy_processing_required;
        sinc_bank_.swap(candidate.sinc_bank);
        voices_.swap(candidate.voices);
        bus_dsp_.swap(candidate.bus_dsp);
        legacy_engine_.swap(candidate.legacy_engine);
        dry_storage_ = std::move(candidate.dry_storage);
        pitch_storage_ = std::move(candidate.pitch_storage);
        channel_count_ = candidate.channel_count;
        maximum_input_frames_ = candidate.maximum_input_frames;
        maximum_engine_input_frames_ = candidate.maximum_engine_input_frames;
        maximum_stream_input_frames_ = candidate.maximum_stream_input_frames;
        clock_ratio_ = candidate.clock_ratio;
        machine_sample_rate_ = candidate.machine_sample_rate;
        nominal_latency_frames_ = candidate.nominal_latency_frames;
        reported_latency_frames_ = candidate.reported_latency_frames;
        host_sample_rate_ = candidate.host_sample_rate;
        runtime_continuation_required_ =
            candidate.runtime_continuation_required;
        runtime_state_status_.store(candidate.runtime_state_status,
                                    std::memory_order_relaxed);
        if (!all_stages_bypassed_) {
            for (std::size_t channel = 0; channel < channel_count_; ++channel) {
                dry_ptrs_[channel] = dry_storage_.data() +
                    channel * maximum_input_frames_;
                dry_const_ptrs_[channel] = dry_ptrs_[channel];
                pitch_ptrs_[channel] = pitch_storage_.data() +
                    channel * maximum_engine_input_frames_;
                pitch_const_ptrs_[channel] = pitch_ptrs_[channel];
            }
        }
        prepared_ = true;
        failed_closed_ = false;
        status_.store(ready, std::memory_order_relaxed);
    }

    void fail_plan() noexcept {
        failed_closed_ = true;
        render_plan_failures_.fetch_add(1, std::memory_order_relaxed);
        status_.store(PulpSamplerHeritageStatus::RenderPlanFailed,
                      std::memory_order_relaxed);
    }

    void fail_process() noexcept {
        failed_closed_ = true;
        render_failures_.fetch_add(1, std::memory_order_relaxed);
        status_.store(PulpSamplerHeritageStatus::RenderFailed,
                      std::memory_order_relaxed);
    }

    audio::SampleHeritagePreparedProfile configured_{};
    std::unique_ptr<audio::SampleSincKernelBank> sinc_bank_;
    std::unique_ptr<VoiceState[]> voices_;
    std::unique_ptr<audio::SampleHeritageBusDsp> bus_dsp_;
    std::unique_ptr<audio::SampleHeritageEngine> legacy_engine_;
    std::vector<float> dry_storage_;
    std::vector<float> pitch_storage_;
    std::array<float*, audio::kSampleHeritageMaximumChannels> dry_ptrs_{};
    std::array<const float*, audio::kSampleHeritageMaximumChannels>
        dry_const_ptrs_{};
    std::array<float*, audio::kSampleHeritageMaximumChannels> pitch_ptrs_{};
    std::array<const float*, audio::kSampleHeritageMaximumChannels>
        pitch_const_ptrs_{};
    std::atomic<PulpSamplerHeritageStatus> status_{
        PulpSamplerHeritageStatus::Disabled};
    std::atomic<std::uint64_t> render_plan_failures_{0};
    std::atomic<std::uint64_t> render_failures_{0};
    std::atomic<audio::SampleHeritageRuntimeStateStatus> runtime_state_status_{
        audio::SampleHeritageRuntimeStateStatus::NotPrepared};
    std::atomic<std::uint64_t> rate_admission_rejections_{0};
    std::atomic<std::uint64_t> rate_automation_rejections_{0};
    audio::SampleHeritageProfileStatus profile_status_ =
        audio::SampleHeritageProfileStatus::Ok;
    std::size_t channel_count_ = 0;
    std::size_t maximum_input_frames_ = 0;
    std::size_t maximum_engine_input_frames_ = 0;
    std::size_t maximum_stream_input_frames_ = 0;
    double clock_ratio_ = 1.0;
    double machine_sample_rate_ = 0.0;
    double nominal_latency_frames_ = 0.0;
    std::uint32_t reported_latency_frames_ = 0;
    double host_sample_rate_ = 0.0;
    bool configured_valid_ = false;
    bool configured_typed_ = false;
    bool prepared_ = false;
    bool typed_ = false;
    bool runtime_continuation_required_ = false;
    bool voice_processing_required_ = false;
    bool voice_artifact_path_active_ = false;
    bool pitch_active_ = false;
    audio::SampleHeritagePitchFamily pitch_family_ =
        audio::SampleHeritagePitchFamily::VariableClock;
    bool bus_processing_required_ = false;
    bool legacy_processing_required_ = false;
    bool all_stages_bypassed_ = true;
    bool failed_closed_ = false;
#if defined(PULP_SAMPLER_TEST_HOOKS)
    std::atomic<bool> fail_next_plan_for_test_{false};
#endif
};

}  // namespace pulp::examples
