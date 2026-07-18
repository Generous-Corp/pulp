#pragma once

#include "sampler_api.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_engine.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace pulp::examples {

/// Off-thread profile ownership plus bounded callback processing for the
/// PulpSampler heritage path. Configuration and prepare must run while audio is
/// stopped; callback methods are allocation-free after a successful prepare.
class SamplerHeritageRuntime {
public:
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
        std::unique_ptr<audio::SampleHeritageEngine> engine;
        std::vector<float> dry_storage;
        audio::SampleHeritageProfileStatus profile_status =
            audio::SampleHeritageProfileStatus::Ok;
        std::size_t channel_count = 0;
        std::size_t maximum_input_frames = 0;
        double nominal_latency_frames = 0.0;
        std::uint32_t reported_latency_frames = 0;
        bool all_stages_bypassed = true;
        audio::SampleHeritageRuntimeStateStatus runtime_state_status =
            audio::SampleHeritageRuntimeStateStatus::NotPrepared;
    };

    SamplerHeritageRuntime()
        : engine_(std::make_unique<audio::SampleHeritageEngine>()) {}

    PulpSamplerHeritageStatus configure(
        const audio::SampleHeritageProfile& profile) {
        const auto validation = audio::validate_sample_heritage_profile(profile);
        if (!validation.valid()) {
            // Replacement is transactional: a rejected candidate must not tear
            // down the currently prepared profile or change its latency.
            return PulpSamplerHeritageStatus::InvalidProfile;
        }

        release_processing();
        configured_ = validation.profile;
        profile_status_ = validation.status;
        configured_valid_ = true;
        all_stages_bypassed_ = true;
        for (std::size_t index = 0; index < configured_.stage_count; ++index)
            all_stages_bypassed_ =
                all_stages_bypassed_ && configured_.stages[index].bypass;
        status_.store(PulpSamplerHeritageStatus::PendingPrepare,
                      std::memory_order_relaxed);
        return PulpSamplerHeritageStatus::PendingPrepare;
    }

    void disable() noexcept {
        release_processing();
        configured_ = {};
        configured_valid_ = false;
        all_stages_bypassed_ = true;
        profile_status_ = audio::SampleHeritageProfileStatus::Ok;
        status_.store(PulpSamplerHeritageStatus::Disabled,
                      std::memory_order_relaxed);
    }

    PulpSamplerHeritageStatus prepare(double host_sample_rate,
                                      std::size_t channel_count,
                                      std::size_t maximum_output_frames,
                                      const audio::SampleHeritageRuntimeState*
                                          runtime_state = nullptr,
                                      double runtime_host_sample_rate = 0.0) noexcept {
        if (!configured_valid_) {
            release_processing();
            status_.store(PulpSamplerHeritageStatus::Disabled,
                          std::memory_order_relaxed);
            return PulpSamplerHeritageStatus::Disabled;
        }

        PreparedReplacement candidate;
        const auto result = prepare_candidate(
            configured_, profile_status_, host_sample_rate, channel_count,
            maximum_output_frames, runtime_state, runtime_host_sample_rate,
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
        const audio::SampleHeritageRuntimeState* runtime_state,
        double runtime_host_sample_rate,
        PreparedReplacement& candidate) noexcept {
        const auto validation = audio::validate_sample_heritage_profile(profile);
        if (!validation.valid()) return PulpSamplerHeritageStatus::InvalidProfile;
        return prepare_candidate(
            validation.profile, validation.status, host_sample_rate,
            channel_count, maximum_output_frames, runtime_state,
            runtime_host_sample_rate, candidate);
    }

    static double replacement_clock_ratio(
        const PreparedReplacement& candidate) noexcept {
        return candidate.all_stages_bypassed ? 1.0
                                             : candidate.engine->clock_ratio();
    }

    static std::size_t replacement_maximum_input_frames(
        const PreparedReplacement& candidate,
        std::size_t maximum_output_frames) noexcept {
        return candidate.all_stages_bypassed ? maximum_output_frames
                                             : candidate.maximum_input_frames;
    }

    void publish_replacement(PreparedReplacement&& candidate,
                             PulpSamplerHeritageStatus ready) noexcept {
        publish_candidate(std::move(candidate), ready);
    }

    void release_processing() noexcept {
        engine_->release();
        std::vector<float>().swap(dry_storage_);
        dry_ptrs_.fill(nullptr);
        dry_const_ptrs_.fill(nullptr);
        prepared_ = false;
        failed_closed_ = false;
        channel_count_ = 0;
        maximum_input_frames_ = 0;
        nominal_latency_frames_ = 0.0;
        reported_latency_frames_ = 0;
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
    bool all_stages_bypassed() const noexcept { return all_stages_bypassed_; }
    audio::SampleHeritageProfile configured_profile() const {
        audio::SampleHeritageProfile result;
        if (!configured_valid_) return result;
        result.schema_version = configured_.schema_version;
        result.profile_id.assign(configured_.id());
        result.host_sample_rate = configured_.host_sample_rate;
        result.stages.assign(configured_.stages.begin(),
                             configured_.stages.begin() + configured_.stage_count);
        return result;
    }
    double active_clock_ratio() const noexcept {
        return prepared_ && !all_stages_bypassed_
            ? engine_->clock_ratio()
            : 1.0;
    }
    std::size_t maximum_input_frames() const noexcept {
        return prepared_ && !all_stages_bypassed_ ? maximum_input_frames_ : 0;
    }
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
        if (!processing_required()) return false;
        result = engine_->plan_exact(output_frames);
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
        if (!processing_required() || plan.input_frames > maximum_input_frames_) {
            fail_process();
            return false;
        }
        const auto input = dry_input(plan.input_frames);
        if (engine_->process_exact(plan, input, output) ==
            audio::SampleHeritageProcessStatus::Ok) {
            return true;
        }
        fail_process();
        return false;
    }

    void reject_process() noexcept { fail_process(); }

    audio::SampleHeritageRuntimeStateCapture capture_runtime_state() const noexcept {
        if (!prepared_) return {};
        return engine_->capture_runtime_state();
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
        result.machine_sample_rate = engine_->machine_sample_rate();
        result.clock_ratio = engine_->clock_ratio();
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
    static PulpSamplerHeritageStatus prepare_candidate(
        const audio::SampleHeritagePreparedProfile& profile,
        audio::SampleHeritageProfileStatus profile_status,
        double host_sample_rate,
        std::size_t channel_count,
        std::size_t maximum_output_frames,
        const audio::SampleHeritageRuntimeState* runtime_state,
        double runtime_host_sample_rate,
        PreparedReplacement& candidate) noexcept {
        candidate.profile = profile;
        candidate.profile_status = profile_status;
        candidate.channel_count = channel_count;
        auto runtime_profile = profile;
        const auto runtime_rate_known =
            runtime_host_sample_rate > 0.0 &&
            std::isfinite(runtime_host_sample_rate);
        const auto host_rate_changed = runtime_state != nullptr
            ? !runtime_rate_known || runtime_host_sample_rate != host_sample_rate
            : runtime_profile.host_sample_rate != host_sample_rate;
        runtime_profile.host_sample_rate = host_sample_rate;
        try {
            candidate.engine =
                std::make_unique<audio::SampleHeritageEngine>();
        } catch (...) {
            return PulpSamplerHeritageStatus::PrepareFailed;
        }
        if (candidate.engine->prepare({runtime_profile, channel_count,
                                       maximum_output_frames}) !=
            audio::SampleHeritagePrepareStatus::Ok) {
            return PulpSamplerHeritageStatus::PrepareFailed;
        }

        for (std::size_t index = 0; index < profile.stage_count; ++index) {
            candidate.all_stages_bypassed =
                candidate.all_stages_bypassed && profile.stages[index].bypass;
        }
        candidate.maximum_input_frames =
            candidate.engine->maximum_input_frames();
        if (!candidate.all_stages_bypassed) {
            if (candidate.maximum_input_frames >
                    std::numeric_limits<std::uint32_t>::max() ||
                (channel_count != 0 &&
                 candidate.maximum_input_frames >
                     std::numeric_limits<std::size_t>::max() / channel_count)) {
                return PulpSamplerHeritageStatus::PrepareFailed;
            }
            try {
                candidate.dry_storage.assign(
                    channel_count * candidate.maximum_input_frames, 0.0f);
            } catch (...) {
                return PulpSamplerHeritageStatus::PrepareFailed;
            }
        }
        candidate.nominal_latency_frames =
            candidate.engine->latency_output_frames();
        const auto rounded_latency =
            std::ceil(candidate.nominal_latency_frames);
        candidate.reported_latency_frames = rounded_latency >=
                static_cast<double>(std::numeric_limits<std::uint32_t>::max())
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>(rounded_latency);
        const auto ready = host_rate_changed
            ? PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate
            : PulpSamplerHeritageStatus::Ready;
        if (runtime_state != nullptr && !host_rate_changed) {
            candidate.runtime_state_status =
                candidate.engine->restore_runtime_state(*runtime_state);
            if (candidate.runtime_state_status !=
                audio::SampleHeritageRuntimeStateStatus::Ok) {
                return PulpSamplerHeritageStatus::RuntimeStateRejected;
            }
        }
        return ready;
    }

    void publish_candidate(PreparedReplacement&& candidate,
                           PulpSamplerHeritageStatus ready) noexcept {
        release_processing();
        configured_ = candidate.profile;
        profile_status_ = candidate.profile_status;
        configured_valid_ = true;
        all_stages_bypassed_ = candidate.all_stages_bypassed;
        engine_.swap(candidate.engine);
        dry_storage_ = std::move(candidate.dry_storage);
        channel_count_ = candidate.channel_count;
        maximum_input_frames_ = candidate.maximum_input_frames;
        nominal_latency_frames_ = candidate.nominal_latency_frames;
        reported_latency_frames_ = candidate.reported_latency_frames;
        runtime_state_status_.store(candidate.runtime_state_status,
                                    std::memory_order_relaxed);
        if (!all_stages_bypassed_) {
            for (std::size_t channel = 0; channel < channel_count_; ++channel) {
                dry_ptrs_[channel] = dry_storage_.data() +
                    channel * maximum_input_frames_;
                dry_const_ptrs_[channel] = dry_ptrs_[channel];
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
    std::unique_ptr<audio::SampleHeritageEngine> engine_;
    std::vector<float> dry_storage_;
    std::array<float*, audio::kSampleHeritageMaximumChannels> dry_ptrs_{};
    std::array<const float*, audio::kSampleHeritageMaximumChannels>
        dry_const_ptrs_{};
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
    double nominal_latency_frames_ = 0.0;
    std::uint32_t reported_latency_frames_ = 0;
    bool configured_valid_ = false;
    bool prepared_ = false;
    bool all_stages_bypassed_ = true;
    bool failed_closed_ = false;
#if defined(PULP_SAMPLER_TEST_HOOKS)
    std::atomic<bool> fail_next_plan_for_test_{false};
#endif
};

}  // namespace pulp::examples
