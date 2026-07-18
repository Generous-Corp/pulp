#pragma once

/// PulpSampler — sample-buffer sampler with MIDI triggering and ADSR envelope.
/// Demonstrates: controller-thread sample-slot publication, primitive loop
/// rendering, ADSR, pitch shifting, and processor parameter serialization.

#include "sampler_source_publication.hpp"
#include "sampler_heritage_runtime.hpp"
#include "sampler_heritage_state.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/sample_key_map.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/signal/adsr.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pulp::examples {

enum SamplerParams : state::ParamID {
    kSamplerGain     = 1,
    kSamplerAttack   = 2,
    kSamplerDecay    = 3,
    kSamplerSustain  = 4,
    kSamplerRelease  = 5,
    kSamplerPitch    = 6,  // semitones offset
    kSamplerLoop     = 7,  // 0 = one-shot, 1 = loop
    kSamplerReverse  = 8,  // 0 = forward entry, 1 = reverse entry
    kSamplerInterpolation = 9,
};

class PulpSamplerProcessor : public format::Processor {
public:
    static constexpr int kMaxVoices =
        static_cast<int>(SamplerStreamingRuntime::maximum_voice_count());
    static constexpr std::uint32_t kMaxSampleChannels = SamplerSampleStore::kMaxChannels;
    static constexpr std::uint32_t kMaxOutputChannels = 8;

    ~PulpSamplerProcessor() override { release(); }

    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpSampler",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.sampler",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            .tail_samples = -1,
        };
    }

    int latency_samples() const override {
        return heritage_.latency_samples();
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({.id = kSamplerGain, .name = "Gain",
            .unit = "dB", .range = {-60, 12, 0, 0.1f}});
        store.add_parameter({.id = kSamplerAttack, .name = "Attack",
            .unit = "ms", .range = {0, 5000, 10, 1}});
        store.add_parameter({.id = kSamplerDecay, .name = "Decay",
            .unit = "ms", .range = {0, 5000, 100, 1}});
        store.add_parameter({.id = kSamplerSustain, .name = "Sustain",
            .unit = "%", .range = {0, 100, 80, 1}});
        store.add_parameter({.id = kSamplerRelease, .name = "Release",
            .unit = "ms", .range = {0, 10000, 200, 1}});
        store.add_parameter({.id = kSamplerPitch, .name = "Pitch",
            .unit = "st", .range = {-24, 24, 0, 1}});
        store.add_parameter({.id = kSamplerLoop, .name = "Loop",
            .unit = "", .range = {0, 1, 0, 1}});
        store.add_parameter({.id = kSamplerReverse, .name = "Reverse",
            .unit = "", .range = {0, 1, 0, 1}});
        store.add_parameter({.id = kSamplerInterpolation, .name = "Interpolation",
            .unit = "", .range = {0, 5, 2, 1}});
    }

    std::vector<std::uint8_t> serialize_plugin_state() const override {
        SamplerHeritagePersistentState state;
        state.enabled = heritage_.configured();
        if (!state.enabled) return {};
        state.profile = heritage_.configured_profile();

        if (callback_runtime_snapshot_available_.load(
                std::memory_order_acquire)) {
            const auto published = callback_runtime_snapshot_.read();
            state.has_runtime_state = published.valid;
            state.runtime_state = published.state;
        } else if (restored_runtime_state_available_) {
            state.has_runtime_state = true;
            state.runtime_state = restored_runtime_state_;
        }
        auto written = write_sampler_heritage_state(state);
        if (!written.valid() && state.has_runtime_state) {
            // A bad continuation snapshot must not make the strict outer state
            // envelope silently look like "heritage disabled". Preserve the
            // validated profile and reset only the optional RNG continuation.
            state.has_runtime_state = false;
            written = write_sampler_heritage_state(state);
        }
        return written.valid() ? written.bytes : std::vector<std::uint8_t>{};
    }

    bool deserialize_plugin_state(
        std::span<const std::uint8_t> bytes) override {
        const auto parsed = parse_sampler_heritage_state(bytes);
        if (!parsed.valid()) return false;

        const auto previous_latency = latency_samples();
        if (!parsed.state.enabled) {
            reset_all_voices();
            heritage_.disable();
            clear_heritage_runtime_persistence();
            if (latency_samples() != previous_latency) flag_latency_changed();
            return true;
        }

        const auto* runtime_state = parsed.state.has_runtime_state
            ? &parsed.state.runtime_state
            : nullptr;
        if (prepared_) {
            const auto result = heritage_.replace_prepared(
                parsed.state.profile, host_sample_rate_,
                prepared_output_channels_, max_block_frames_, runtime_state);
            if (!heritage_ready(result)) return false;
            reset_all_voices();
            clear_heritage_runtime_persistence();
            if (result == PulpSamplerHeritageStatus::Ready &&
                runtime_state != nullptr) {
                restored_runtime_state_ = *runtime_state;
                restored_runtime_state_available_ = true;
            }
            if (latency_samples() != previous_latency) flag_latency_changed();
            return true;
        }

        const auto configured = heritage_.configure(parsed.state.profile);
        if (configured != PulpSamplerHeritageStatus::PendingPrepare) return false;
        reset_all_voices();
        clear_heritage_runtime_persistence();
        if (runtime_state != nullptr) {
            pending_runtime_state_ = *runtime_state;
            pending_runtime_state_available_ = true;
            restored_runtime_state_ = *runtime_state;
            restored_runtime_state_available_ = true;
        }
        return true;
    }

    /// Load a mono sample buffer. Call off the audio thread after prepare().
    bool load_sample(const float* data, int num_samples, float sample_rate) {
        return source_publication_.load_mono(
            streaming_, data, num_samples, sample_rate);
    }

    /// Load a sample from interleaved stereo. Call off the audio thread after prepare().
    bool load_sample_stereo(const float* interleaved, int num_frames, float sample_rate) {
        return source_publication_.load_stereo(
            streaming_, interleaved, num_frames, sample_rate);
    }

    /// Load a true ranged WAV or uncompressed AIFF asset. The call may perform
    /// file I/O and wait for the sampler's non-audio owner thread.
    bool load_sample_file(std::string_view path) {
        return streaming_.load_sample_file(path);
    }

    bool has_sample() const {
        return streaming_.published_source().kind != SamplerPublishedSourceKind::None;
    }

    int sample_length() const {
        const auto source = streaming_.published_source();
        const auto frames = source.kind == SamplerPublishedSourceKind::Streamed
            ? source.streamed.total_frames
            : source.resident.num_frames;
        return frames > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(frames);
    }

    PulpSamplerStreamStats stream_stats() const noexcept {
        return streaming_.stats();
    }

    /// Replaces the optional heritage profile while audio is stopped. Existing
    /// voices are reset because a profile change establishes a new processing
    /// timeline and latency contract.
    PulpSamplerHeritageStatus set_heritage_profile(
        const audio::SampleHeritageProfile& profile) {
        const auto previous_latency = latency_samples();
        if (prepared_) {
            const auto result = heritage_.replace_prepared(
                profile, host_sample_rate_, prepared_output_channels_,
                max_block_frames_);
            if (result != PulpSamplerHeritageStatus::Ready &&
                result !=
                    PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate) {
                return result;
            }
            reset_all_voices();
            clear_heritage_runtime_persistence();
            if (latency_samples() != previous_latency) flag_latency_changed();
            return result;
        }

        const auto configured = heritage_.configure(profile);
        if (configured != PulpSamplerHeritageStatus::PendingPrepare) {
            return configured;
        }
        reset_all_voices();
        clear_heritage_runtime_persistence();
        return configured;
    }

    /// Disables heritage processing while audio is stopped. Like profile
    /// replacement, this must not race process().
    void disable_heritage() noexcept {
        const auto previous_latency = latency_samples();
        reset_all_voices();
        heritage_.disable();
        clear_heritage_runtime_persistence();
        if (latency_samples() != previous_latency) flag_latency_changed();
    }

    PulpSamplerHeritageDiagnostics heritage_diagnostics() const noexcept {
        return heritage_.diagnostics();
    }

    void prepare(const format::PrepareContext& ctx) override {
        const auto previous_latency = latency_samples();
        release();
        host_sample_rate_ = static_cast<float>(ctx.sample_rate);
        max_block_frames_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(ctx.max_buffer_size));
        prepared_output_channels_ = std::clamp<std::uint32_t>(
            static_cast<std::uint32_t>(ctx.output_channels), 1, kMaxOutputChannels);

        for (std::uint32_t ch = 0; ch < kMaxOutputChannels; ++ch) {
            voice_scratch_[ch].assign(max_block_frames_, 0.0f);
        }
        source_publication_.prepare();
        const auto maximum_source_frames_per_output =
            SamplerStreamingRuntime::maximum_pitch_ratio() *
            SamplerStreamingRuntime::maximum_source_sample_rate() /
            static_cast<double>(host_sample_rate_);
        sinc_bank_.build_dense_for_maximum_consumption(
            maximum_source_frames_per_output);
        for (auto& voice : voices_) voice.reset();
        for (auto& generation : requester_generations_) generation = 0;
        for (auto& cancellation : pending_cancellations_) cancellation = {};
#if defined(PULP_SAMPLER_TEST_HOOKS)
        retire_reverse_attack_after_horizon_for_test_ = false;
        stream_rate_capacity_override_for_test_ = 0.0;
#endif
        streaming_.prepare(host_sample_rate_, max_block_frames_);
        source_publication_.acknowledge_audio(
            streaming_.published_source(), voices_, streaming_);
        callback_runtime_snapshot_available_.store(false,
                                                   std::memory_order_release);
        restored_runtime_state_available_ = false;
        if (heritage_.configured()) {
            const auto* runtime_state = pending_runtime_state_available_
                ? &pending_runtime_state_
                : nullptr;
            const auto result = heritage_.prepare(
                host_sample_rate_, prepared_output_channels_, max_block_frames_,
                runtime_state);
            if (heritage_ready(result)) {
                if (result == PulpSamplerHeritageStatus::Ready &&
                    runtime_state != nullptr) {
                    restored_runtime_state_ = *runtime_state;
                    restored_runtime_state_available_ = true;
                }
                pending_runtime_state_available_ = false;
            }
        }
        prepared_ = true;
        if (latency_samples() != previous_latency) flag_latency_changed();
    }

    void release() override {
        for (auto& voice : voices_) voice.reset();
        for (auto& cancellation : pending_cancellations_) cancellation = {};
        streaming_.release();
        source_publication_.release();
        sinc_bank_.release();
        heritage_.release_processing();
        prepared_ = false;
    }

    void process(
        audio::BufferView<float>& output,
        const audio::BufferView<const float>&,
        midi::MidiBuffer& midi_in,
        midi::MidiBuffer&,
        const format::ProcessContext&) override
    {
        clear_output(output);

        const auto audio_generation = streaming_.begin_audio_callback();
        flush_pending_cancellations();

        const auto published = streaming_.published_source();
        const bool can_trigger = published_source_valid(published);

        const auto params = current_params();
        const auto block_frames = static_cast<std::uint32_t>(output.num_samples());
        midi_in.sort();

        std::uint32_t cursor = 0;
        for (std::size_t i = 0; i < midi_in.size(); ++i) {
            const auto& event = midi_in[i];
            const auto offset = static_cast<std::uint32_t>(
                std::clamp(event.sample_offset, 0, static_cast<int32_t>(block_frames)));
            if (offset > cursor) {
                render_output_segment(output, cursor, offset - cursor, params);
            }

            if (event.message.isNoteOn() && can_trigger) {
                trigger_note(event.message.getNoteNumber(),
                             static_cast<float>(event.message.getVelocity()) / 127.0f,
                             published,
                             params);
            } else if (event.message.isNoteOff()) {
                release_note(event.message.getNoteNumber());
            }
            cursor = offset;
        }

        if (cursor < block_frames) {
            render_output_segment(output, cursor, block_frames - cursor, params);
        }

        source_publication_.acknowledge_audio(published, voices_, streaming_);
        flush_pending_cancellations();
        streaming_.complete_audio_callback(audio_generation);
        publish_heritage_runtime_snapshot();
    }

private:
    friend struct PulpSamplerTestAccess;
    friend struct PulpSamplerHeritageTestAccess;

    struct RenderParams {
        float gain = 1.0f;
        signal::Adsr::Params adsr;
        float pitch_semitones = 0.0f;
        bool loop = false;
        bool reverse = false;
        audio::SampleInterpolationPolicy interpolation =
            audio::SampleInterpolationPolicy::Linear;
    };

    struct PendingCancellation {
        audio::SampleStreamRequesterToken requester{};
        bool valid = false;
    };

    struct PublishedHeritageRuntimeState {
        audio::SampleHeritageRuntimeState state{};
        bool valid = false;
    };

    static_assert(std::is_trivially_copyable_v<PublishedHeritageRuntimeState>);

    static bool heritage_ready(PulpSamplerHeritageStatus status) noexcept {
        return status == PulpSamplerHeritageStatus::Ready ||
               status ==
                   PulpSamplerHeritageStatus::ReadyRuntimeResetForHostRate;
    }

    void clear_heritage_runtime_persistence() noexcept {
        callback_runtime_snapshot_available_.store(false,
                                                   std::memory_order_release);
        restored_runtime_state_available_ = false;
        pending_runtime_state_available_ = false;
    }

    void publish_heritage_runtime_snapshot() noexcept {
        const auto captured = heritage_.capture_runtime_state();
        PublishedHeritageRuntimeState published;
        published.valid = captured.valid();
        if (published.valid) published.state = captured.state;
        callback_runtime_snapshot_.write(published);
        callback_runtime_snapshot_available_.store(published.valid,
                                                   std::memory_order_release);
    }

    SamplerSourcePublicationOwner source_publication_;
    SamplerStreamingRuntime streaming_;
    SamplerHeritageRuntime heritage_;
    runtime::SeqLock<PublishedHeritageRuntimeState>
        callback_runtime_snapshot_{};
    std::atomic<bool> callback_runtime_snapshot_available_{false};
    audio::SampleHeritageRuntimeState restored_runtime_state_{};
    audio::SampleHeritageRuntimeState pending_runtime_state_{};
    bool restored_runtime_state_available_ = false;
    bool pending_runtime_state_available_ = false;
    audio::SampleSincKernelBank sinc_bank_;
    audio::SampleKeyMap key_map_;
    std::array<std::vector<float>, kMaxOutputChannels> voice_scratch_{};
    std::array<float*, kMaxOutputChannels> voice_scratch_ptrs_{};
    float host_sample_rate_ = 44100.0f;
    std::uint32_t max_block_frames_ = 512;
    std::uint32_t prepared_output_channels_ = 2;
    bool prepared_ = false;
    SamplerVoice voices_[kMaxVoices]{};
    std::array<std::uint64_t, kMaxVoices> requester_generations_{};
    std::array<PendingCancellation, kMaxVoices> pending_cancellations_{};
#if defined(PULP_SAMPLER_TEST_HOOKS)
    bool retire_reverse_attack_after_horizon_for_test_ = false;
    std::uint64_t lookahead_plans_last_callback_for_test_ = 0;
    double stream_rate_capacity_override_for_test_ = 0.0;
#endif

    bool published_source_valid(const SamplerPublishedSource& source) const noexcept {
        if (source.kind == SamplerPublishedSourceKind::Resident)
            return source_publication_.sample_store().slot_view_valid(source.resident);
        return source.kind == SamplerPublishedSourceKind::Streamed &&
               source.streamed.valid();
    }

    RenderParams current_params() const {
        RenderParams params;
        const float gain_db = state().get_value(kSamplerGain);
        params.gain = std::pow(10.0f, gain_db / 20.0f);
        params.adsr.attack = state().get_value(kSamplerAttack) / 1000.0f;
        params.adsr.decay = state().get_value(kSamplerDecay) / 1000.0f;
        params.adsr.sustain = state().get_value(kSamplerSustain) / 100.0f;
        params.adsr.release = state().get_value(kSamplerRelease) / 1000.0f;
        params.pitch_semitones = state().get_value(kSamplerPitch);
        params.loop = state().get_value(kSamplerLoop) >= 0.5f;
        params.reverse = state().get_value(kSamplerReverse) >= 0.5f;
        const auto interpolation = std::clamp(
            static_cast<int>(std::lround(
                state().get_value(kSamplerInterpolation))), 0, 5);
        params.interpolation = static_cast<audio::SampleInterpolationPolicy>(
            interpolation);
        return params;
    }

    static void clear_output(audio::BufferView<float>& output) noexcept {
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            std::fill_n(output.channel_ptr(ch), output.num_samples(), 0.0f);
        }
    }

    static void clear_output_segment(audio::BufferView<float>& output,
                                     std::uint32_t start_frame,
                                     std::uint32_t frames) noexcept {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
            std::fill_n(output.channel_ptr(channel) + start_frame, frames, 0.0f);
        }
    }

    audio::LoopRegion make_region(std::uint64_t frames,
                                  double sample_rate,
                                  bool loop,
                                  bool reverse) const noexcept {
        audio::LoopRegion region;
        region.start_frame = 0;
        region.end_frame = frames;
        region.source_sample_rate = sample_rate;
        region.playback_mode = loop
            ? (reverse ? audio::LoopPlaybackMode::Reverse
                       : audio::LoopPlaybackMode::Forward)
            : (reverse ? audio::LoopPlaybackMode::ReverseOnce
                       : audio::LoopPlaybackMode::OneShot);
        region.reverse_entry = reverse;
        region.interpolation = audio::LoopInterpolationMode::Linear;
        region.crossfade_curve = audio::LoopCrossfadeCurve::Linear;
        if (loop && frames >= 32) {
            region.crossfade_frames =
                std::min<std::uint64_t>({64, frames / 8, frames / 2});
        }
        return region;
    }

    audio::LoopRegion make_region(const audio::PublishedSampleView& sample,
                                  const RenderParams& params) const noexcept {
        return make_region(sample.num_frames, sample.sample_rate,
                           params.loop, params.reverse);
    }

    double playback_speed(int note,
                          const audio::PublishedSampleView& sample,
                          const RenderParams& params) const noexcept {
        return key_map_.playback_rate_for_note(note,
                                               sample.sample_rate,
                                               static_cast<double>(host_sample_rate_),
                                               params.pitch_semitones);
    }

    double playback_speed(int note,
                          double sample_rate,
                          const RenderParams& params) const noexcept {
        return key_map_.playback_rate_for_note(note,
                                               sample_rate,
                                               static_cast<double>(host_sample_rate_),
                                               params.pitch_semitones);
    }

    static bool polynomial_mip_policy(
        audio::SampleInterpolationPolicy policy) noexcept {
        return policy == audio::SampleInterpolationPolicy::CubicHermite ||
               policy == audio::SampleInterpolationPolicy::CubicLagrange;
    }

    static SamplerMipLevelView select_resident_mip(
        const SamplerPublishedSource& source,
        audio::SampleInterpolationPolicy policy,
        double base_source_frames_per_output,
        bool loop,
        bool reverse) noexcept {
        // Stored forward one-shot mips use clamped FIR boundaries. Loops and
        // reverse entries stay on the base asset so wrapped edges and the final
        // source-frame phase remain exact.
        if (!polynomial_mip_policy(policy) || loop || reverse) return {};
        const auto octave = sampler_exact_mip_octave(
            base_source_frames_per_output);
        const auto* level = source.resident_mips.level(octave);
        return level == nullptr ? SamplerMipLevelView{} : *level;
    }

    static const SamplerStreamMipLevelView* select_streamed_mip(
        const SamplerPublishedSource& source,
        audio::SampleInterpolationPolicy policy,
        double base_source_frames_per_output,
        bool loop,
        bool reverse) noexcept {
        if (!polynomial_mip_policy(policy) || loop || reverse) return nullptr;
        return source.streamed_mips.level(
            sampler_exact_mip_octave(base_source_frames_per_output));
    }

    audio::PreparedSampleInterpolation prepared_interpolation(
        audio::SampleInterpolationPolicy policy,
        double source_frames_per_output) const noexcept {
        audio::PreparedSampleInterpolation interpolation{.policy = policy};
        if (policy == audio::SampleInterpolationPolicy::RatioTrackingSinc) {
            interpolation.sinc =
                sinc_bank_.view().select(source_frames_per_output);
            if (!interpolation.sinc.valid()) {
                interpolation = {
                    .policy = audio::SampleInterpolationPolicy::CubicHermite};
            }
        }
        return interpolation;
    }

    audio::PreparedSampleInterpolation prepared_rate_safe_interpolation(
        audio::SampleInterpolationPolicy policy,
        double source_frames_per_output) const noexcept {
        if (polynomial_mip_policy(policy) && source_frames_per_output > 1.0) {
            policy = audio::SampleInterpolationPolicy::RatioTrackingSinc;
        }
        return prepared_interpolation(policy, source_frames_per_output);
    }

    bool stream_rate_contract_allows(
        const audio::SampleAssetView& candidate,
        double candidate_source_frames_per_output,
        const RenderParams& params,
        std::size_t replaced_voice,
        bool exclude_fading_voices) const noexcept {
        // Asset/preload validation remains owned by the voice reader so its
        // distinct InvalidPreloadContract diagnostic is not masked as a rate
        // rejection.
        if (!candidate.has_stream_source || candidate.stream_source.page_frames == 0 ||
            !(candidate_source_frames_per_output > 0.0)) {
            return false;
        }
        double aggregate_frames_per_second =
            candidate_source_frames_per_output * host_sample_rate_;
        for (std::size_t index = 0; index < kMaxVoices; ++index) {
            if (index == replaced_voice) continue;
            const auto& voice = voices_[index];
            if (!voice.active || !voice.streamed ||
                (exclude_fading_voices &&
                 voice.stream_contract_fade_pending)) continue;
            const auto source = voice.streamed_asset.source;
            if (source.source_id != candidate.source.source_id ||
                source.source_generation != candidate.source.source_generation) {
                continue;
            }
            const auto ratio = key_map_.pitch_ratio_for_note(
                voice.note, params.pitch_semitones);
            aggregate_frames_per_second +=
                ratio * voice.streamed_asset.sample_rate;
        }
        auto certified_frames_per_second =
            static_cast<double>(candidate.stream_source.page_frames) /
            SamplerStreamingRuntime::certified_decoder_latency_seconds();
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (stream_rate_capacity_override_for_test_ > 0.0)
            certified_frames_per_second = stream_rate_capacity_override_for_test_;
#endif
        return std::isfinite(aggregate_frames_per_second) &&
               aggregate_frames_per_second <= certified_frames_per_second;
    }

    void render_output_segment(audio::BufferView<float>& output,
                               std::uint32_t start_frame,
                               std::uint32_t frames,
                               const RenderParams& params) noexcept {
        if (frames == 0) return;
        if (heritage_.direct_path_allowed()) {
            render_active_voices(output, start_frame, frames, params);
            return;
        }
        if (!heritage_.processing_required()) {
            clear_output_segment(output, start_frame, frames);
            return;
        }
        if (output.num_channels() > kMaxOutputChannels) {
            heritage_.reject_process();
            clear_output_segment(output, start_frame, frames);
            return;
        }

        audio::SampleHeritageProcessPlan plan;
        if (!heritage_.plan(frames, plan)) {
            clear_output_segment(output, start_frame, frames);
            return;
        }

        auto dry = heritage_.dry_buffer(plan.input_frames);
        clear_output(dry);
        render_active_voices(dry, 0,
                             static_cast<std::uint32_t>(plan.input_frames),
                             params);

        std::array<float*, kMaxOutputChannels> segment_ptrs{};
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            segment_ptrs[channel] = output.channel_ptr(channel) + start_frame;
        audio::BufferView<float> segment(segment_ptrs.data(),
                                         output.num_channels(), frames);
        if (!heritage_.process(plan, segment))
            clear_output_segment(output, start_frame, frames);
    }

    void render_active_voices(audio::BufferView<float>& output,
                              std::uint32_t start_frame,
                              std::uint32_t frames,
                              const RenderParams& params) noexcept {
        if (frames == 0) return;

        const auto output_channels = std::min<std::uint32_t>(
            {static_cast<std::uint32_t>(output.num_channels()),
             prepared_output_channels_,
             kMaxOutputChannels});
        if (output_channels == 0) return;

        for (std::uint32_t ch = 0; ch < kMaxOutputChannels; ++ch) {
            voice_scratch_ptrs_[ch] = voice_scratch_[ch].data();
        }

        std::uint32_t rendered = 0;
        while (rendered < frames) {
            const auto chunk = std::min(frames - rendered, max_block_frames_);
            audio::BufferView<float> scratch(voice_scratch_ptrs_.data(),
                                             output_channels,
                                             chunk);
            for (std::size_t voice_index = 0; voice_index < kMaxVoices; ++voice_index) {
                auto& voice = voices_[voice_index];
                if (!voice.active) continue;

                if (voice.streamed) {
                    render_streamed_voice(voice_index,
                                          voice,
                                          output,
                                          start_frame + rendered,
                                          chunk,
                                          output_channels,
                                          params);
                    continue;
                }

                std::array<const float*, kMaxSampleChannels> sample_ptrs{};
                std::uint64_t source_frames = voice.sample.num_frames;
                double source_sample_rate = voice.sample.sample_rate;
                if (voice.resident_mip.valid()) {
                    sample_ptrs = voice.resident_mip.channels;
                    source_frames = voice.resident_mip.frames;
                    source_sample_rate = voice.resident_mip.sample_rate;
                } else {
                    if (!source_publication_.sample_store().populate_channel_ptrs(voice.sample,
                                                             sample_ptrs.data(),
                                                             sample_ptrs.size())) {
                        voice.reset();
                        continue;
                    }
                }
                audio::BufferView<const float> source(
                    sample_ptrs.data(),
                    voice.sample.num_channels,
                    static_cast<std::size_t>(source_frames));

                voice.adsr.set_params(params.adsr);
                const auto source_frames_per_output =
                    playback_speed(voice.note, source_sample_rate, params);
                const auto interpolation = prepared_rate_safe_interpolation(
                    params.interpolation, source_frames_per_output);
                if (!(source_frames_per_output > 0.0) ||
                    !interpolation.valid() ||
                    !voice.renderer.set_interpolation(interpolation)) {
                    voice.reset();
                    continue;
                }
                voice.renderer.set_playback_rate(source_frames_per_output);
                // LoopRenderer::render() is overwrite-only, so this scratch
                // buffer can be reused for each voice before additive mixdown.
                const auto loop_result = voice.renderer.render(source, scratch, chunk);

                bool voice_finished = false;
                for (std::uint32_t i = 0; i < chunk; ++i) {
                    const float env = voice.adsr.next();
                    if (env <= 0.0001f && voice.released) {
                        voice_finished = true;
                        break;
                    }

                    const float scale = env * voice.velocity * params.gain;
                    for (std::uint32_t ch = 0; ch < output_channels; ++ch) {
                        output.channel_ptr(ch)[start_frame + rendered + i] +=
                            voice_scratch_[ch][i] * scale;
                    }
                }

                if (voice_finished || !loop_result.active) {
                    voice.reset();
                }
            }
            rendered += chunk;
        }
    }

    void render_streamed_voice(std::size_t voice_index,
                               SamplerVoice& voice,
                               audio::BufferView<float>& output,
                               std::uint32_t output_start,
                               std::uint32_t frames,
                               std::uint32_t output_channels,
                               const RenderParams& params) noexcept {
        const auto pitch_ratio = key_map_.pitch_ratio_for_note(
            voice.note, params.pitch_semitones);
        if (!(pitch_ratio > 0.0) ||
            pitch_ratio > SamplerStreamingRuntime::maximum_pitch_ratio()) {
            streaming_.record_voice_outcome(
                audio::SampleStreamVoiceOutcomeClass::InvalidRenderContract);
            queue_voice_cancellation(voice_index, voice.requester);
            voice.reset();
            return;
        }
        const auto source_frames_per_output =
            pitch_ratio * static_cast<double>(voice.streamed_asset.sample_rate) /
            static_cast<double>(host_sample_rate_);
        if (!voice.stream_contract_fade_pending &&
            !stream_rate_contract_allows(voice.streamed_asset,
                                         source_frames_per_output,
                                         params,
                                         voice_index,
                                         true)) {
            streaming_.record_aggregate_rate_automation_rejection();
            voice.stream_contract_fade_pending = true;
            voice.stream_contract_fade_position = 0;
        }
        const auto interpolation = prepared_rate_safe_interpolation(
            params.interpolation, source_frames_per_output);
        if (!interpolation.valid()) {
            streaming_.record_voice_outcome(
                audio::SampleStreamVoiceOutcomeClass::InvalidRenderContract);
            queue_voice_cancellation(voice_index, voice.requester);
            voice.reset();
            return;
        }

        const bool rate_changed =
            voice.stream_playback_rate != source_frames_per_output;
        const bool interpolation_changed = !audio::same_sample_interpolation(
            voice.stream_reader.interpolation(), interpolation);
        if (rate_changed || interpolation_changed) {
            if ((rate_changed &&
                 !voice.stream_reader.set_playback_rate(
                     voice.streamed_asset, source_frames_per_output)) ||
                !voice.stream_reader.set_interpolation(
                    voice.streamed_asset, interpolation) ||
                !voice.lookahead_reader.set_interpolation(
                    voice.streamed_asset, interpolation) ||
                !voice.lookahead_reader.synchronize_cursor(
                    voice.streamed_asset, voice.stream_reader.cursor())) {
                queue_voice_cancellation(voice_index, voice.requester);
                voice.reset();
                return;
            }
            voice.stream_playback_rate = source_frames_per_output;
            voice.lookahead_lead_source_frames = 0.0;
            voice.pending_lookahead = {};
            voice.pending_demand_index = 0;
            voice.pending_refresh_index = 0;
            voice.pending_lookahead_valid = false;
        }

        enqueue_forward_stream_boundary(voice, source_frames_per_output);

        const bool holding_stream_attack = voice.stream_attack_pending;
        if (holding_stream_attack) {
            if (!prepare_reverse_attack_horizon(
                    voice, source_frames_per_output)) {
                voice.stream_reader.mark_held_starvation(frames);
                streaming_.record_voice_outcome(
                    audio::SampleStreamVoiceOutcomeClass::ServiceStarvation,
                    frames);
                return;
            }
#if defined(PULP_SAMPLER_TEST_HOOKS)
            retire_reverse_attack_page_after_horizon_for_test(voice);
#endif
        }

        enqueue_stream_lookahead(voice, frames, source_frames_per_output);
        auto plan = voice.stream_reader.plan_block(voice.streamed_asset,
                                                   frames,
                                                   host_sample_rate_);
        if (plan.supply == audio::SampleStreamVoiceSupply::InvalidContract ||
            plan.supply == audio::SampleStreamVoiceSupply::StaleGeneration) {
            streaming_.record_voice_outcome(
                plan.supply == audio::SampleStreamVoiceSupply::StaleGeneration
                    ? audio::SampleStreamVoiceOutcomeClass::StaleGeneration
                    : audio::SampleStreamVoiceOutcomeClass::InvalidPreloadContract);
            queue_voice_cancellation(voice_index, voice.requester);
            voice.reset();
            return;
        }
        if (holding_stream_attack && !stream_plan_pages_ready(plan)) {
            voice.stream_reader.mark_held_starvation(frames);
            streaming_.record_voice_outcome(
                audio::SampleStreamVoiceOutcomeClass::ServiceStarvation,
                frames);
            return;
        }
        if (holding_stream_attack) voice.stream_attack_pending = false;
        audio::BufferView<float> source_scratch(voice_scratch_ptrs_.data(),
                                                voice.streamed_asset.channels,
                                                frames);
        const auto rendered =
            voice.stream_reader.render_block(voice.streamed_asset, plan, source_scratch);
        streaming_.record_voice_outcome(
            rendered.outcome,
            rendered.supply == audio::SampleStreamVoiceSupply::Starved
                ? frames - rendered.ready_output_frames
                : 0);
        if (rendered.supply == audio::SampleStreamVoiceSupply::InvalidContract ||
            rendered.supply == audio::SampleStreamVoiceSupply::StaleGeneration) {
            queue_voice_cancellation(voice_index, voice.requester);
            voice.reset();
            return;
        }
        voice.lookahead_lead_source_frames -=
            static_cast<double>(frames) * source_frames_per_output;
        voice.adsr.set_params(params.adsr);
        bool voice_finished = false;
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            const float envelope = voice.adsr.next();
            if (envelope <= 0.0001f && voice.released) {
                voice_finished = true;
                break;
            }
            float contract_gain = 1.0f;
            if (voice.stream_contract_fade_pending) {
                constexpr float kHalfPi = 1.57079632679489661923f;
                constexpr std::uint32_t kContractFadeFrames = 64;
                const auto fade_position = std::min(
                    voice.stream_contract_fade_position,
                    kContractFadeFrames - 1);
                contract_gain = std::cos(
                    kHalfPi * static_cast<float>(fade_position) /
                    static_cast<float>(kContractFadeFrames - 1));
                ++voice.stream_contract_fade_position;
            }
            const float scale =
                envelope * voice.velocity * params.gain * contract_gain;
            for (std::uint32_t channel = 0; channel < output_channels; ++channel) {
                float sample = 0.0f;
                if (voice.streamed_asset.channels == 1) {
                    sample = voice_scratch_[0][frame];
                } else if (channel < voice.streamed_asset.channels) {
                    sample = voice_scratch_[channel][frame];
                }
                output.channel_ptr(channel)[output_start + frame] += sample * scale;
            }
        }

        const bool contract_fade_complete =
            voice.stream_contract_fade_pending &&
            voice.stream_contract_fade_position >= 64;
        if (voice_finished || contract_fade_complete ||
            rendered.supply == audio::SampleStreamVoiceSupply::EndOfSource ||
            !voice.stream_reader.active()) {
            queue_voice_cancellation(voice_index, voice.requester);
            voice.reset();
        }
    }

    bool prepare_reverse_attack_horizon(
        SamplerVoice& voice,
        double source_frames_per_output) noexcept {
        const auto& asset = voice.streamed_asset;
        if (asset.fully_resident()) return true;
        if (!asset.valid() || !asset.has_stream_source ||
            asset.stream_source.window == nullptr ||
            asset.stream_source.page_frames == 0 ||
            asset.preload_frames == 0) {
            return false;
        }

        const auto page_frames = asset.stream_source.page_frames;
        const auto tail_frame = asset.total_frames - 1;
        const auto first_frame = asset.total_frames > asset.preload_frames
            ? asset.total_frames - asset.preload_frames
            : 0;
        const auto first_page = first_frame / page_frames;
        const auto last_page = tail_frame / page_frames;
        bool ready = true;
        for (auto page = first_page; page <= last_page; ++page) {
            const auto page_start = page * page_frames;
            const auto probe_frame = std::max(first_frame, page_start);
            if (asset.stream_source.window->ready_page_for_frame(
                    asset.source.source_generation, probe_frame).valid) {
                continue;
            }
            ready = false;
            const auto page_end = page == last_page
                ? asset.total_frames
                : (page + 1) * page_frames;
            (void) streaming_.command_inbox().demand_page({
                .source = asset.source,
                .requester = voice.requester,
                .page_index = page,
                .resident_source_frames = tail_frame - (page_end - 1),
                .consumption_frames_per_second =
                    source_frames_per_output * host_sample_rate_,
                .demand_class = audio::SampleStreamDemandClass::Attack,
            });
        }
        return ready;
    }

    static bool stream_plan_pages_ready(
        const audio::SampleStreamLoopBlockPlan& plan) noexcept {
        if (plan.supply != audio::SampleStreamVoiceSupply::Ready ||
            plan.demand_count > plan.ready_pages.size()) {
            return false;
        }
        for (std::uint32_t index = 0; index < plan.demand_count; ++index) {
            if (!plan.ready_pages[index].valid) return false;
        }
        return true;
    }

#if defined(PULP_SAMPLER_TEST_HOOKS)
    void retire_reverse_attack_page_after_horizon_for_test(
        const SamplerVoice& voice) noexcept {
        if (!retire_reverse_attack_after_horizon_for_test_) return;
        retire_reverse_attack_after_horizon_for_test_ = false;
        const auto& asset = voice.streamed_asset;
        if (!asset.valid() || asset.stream_source.window == nullptr ||
            asset.total_frames == 0) {
            return;
        }
        const auto tail = asset.stream_source.window->ready_page_for_frame(
            asset.source.source_generation, asset.total_frames - 1);
        if (tail.valid) {
            (void) asset.stream_source.window->retire_page(tail.page_index, 1);
        }
    }
#endif

    void enqueue_stream_lookahead(SamplerVoice& voice,
                                  std::uint32_t frames,
                                  double source_frames_per_output) noexcept {
#if defined(PULP_SAMPLER_TEST_HOOKS)
        lookahead_plans_last_callback_for_test_ = 0;
#endif
        if (voice.pending_lookahead_valid) {
            const auto lead =
                stream_lead_source_frames(voice.lookahead_lead_source_frames);
            const auto refreshed = voice.lookahead_reader.enqueue_demands(
                voice.pending_lookahead,
                streaming_.command_inbox(),
                voice.pending_refresh_index,
                lead,
                voice.pending_demand_index);
            voice.pending_refresh_index = refreshed.next_demand_index;
            if (!refreshed.complete) return;
            const auto retry = voice.lookahead_reader.enqueue_demands(
                voice.pending_lookahead,
                streaming_.command_inbox(),
                voice.pending_demand_index,
                lead);
            voice.pending_demand_index = retry.next_demand_index;
            if (!retry.complete) {
                voice.pending_refresh_index = 0;
                return;
            }
            if (!voice.lookahead_reader.commit_planned_timeline(
                    voice.streamed_asset, voice.pending_lookahead)) {
                voice.pending_lookahead_valid = false;
                voice.pending_demand_index = 0;
                return;
            }
            voice.lookahead_lead_source_frames +=
                static_cast<double>(voice.pending_lookahead.output_frames) *
                source_frames_per_output;
            voice.pending_lookahead_valid = false;
            voice.pending_demand_index = 0;
            voice.pending_refresh_index = 0;
        }

        if (voice.streamed_asset.fully_resident() ||
            !voice.streamed_asset.has_preload_contract) {
            return;
        }
        const auto planning_frames = std::max(frames, max_block_frames_);
        const auto& contract = voice.streamed_asset.preload_contract;
        const auto latency_seconds = contract.certified_io_latency_seconds +
            contract.scheduler_margin_seconds + contract.decoder_latency_seconds;
        const auto plan_advance =
            static_cast<double>(planning_frames) * source_frames_per_output;
        const auto contract_lead =
            latency_seconds * source_frames_per_output * host_sample_rate_ +
            plan_advance + source_frames_per_output;
        const auto resident_horizon = voice.streamed_asset.preload_frames > 0
            ? static_cast<double>(voice.streamed_asset.preload_frames - 1)
            : 0.0;
        const auto bounded_preload_lead = std::min(
            resident_horizon,
            resident_horizon * source_frames_per_output) + plan_advance;
        const auto target_lead = std::max(contract_lead, bounded_preload_lead);
        if (!(plan_advance > 0.0) || !std::isfinite(plan_advance) ||
            !std::isfinite(target_lead)) {
            return;
        }
        if (voice.lookahead_lead_source_frames >= target_lead) return;
        const auto required_plans_value = std::ceil(
            (target_lead - voice.lookahead_lead_source_frames) / plan_advance);
        if (!(required_plans_value > 0.0) ||
            !std::isfinite(required_plans_value) ||
            required_plans_value >
                static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
            return;
        }
        const auto required_plans =
            static_cast<std::uint64_t>(required_plans_value);
        constexpr std::uint64_t kPlanBudgetPerCallback = 8;
        const auto plans_this_callback =
            std::min(required_plans, kPlanBudgetPerCallback);
        for (std::uint64_t iteration = 0;
             iteration < plans_this_callback;
             ++iteration) {
#if defined(PULP_SAMPLER_TEST_HOOKS)
            ++lookahead_plans_last_callback_for_test_;
#endif
            auto plan = voice.lookahead_reader.plan_block(
                voice.streamed_asset,
                planning_frames,
                host_sample_rate_,
                audio::SampleStreamDemandClass::Sustain);
            if (plan.supply != audio::SampleStreamVoiceSupply::Ready) return;
            const auto queued = voice.lookahead_reader.enqueue_demands(
                plan,
                streaming_.command_inbox(),
                0,
                stream_lead_source_frames(voice.lookahead_lead_source_frames));
            if (!queued.complete) {
                voice.pending_lookahead = plan;
                voice.pending_demand_index = queued.next_demand_index;
                voice.pending_refresh_index = 0;
                voice.pending_lookahead_valid = true;
                return;
            }
            if (!voice.lookahead_reader.commit_planned_timeline(
                    voice.streamed_asset, plan)) return;
            voice.lookahead_lead_source_frames += plan_advance;
        }
    }

    static std::uint64_t stream_lead_source_frames(double lead) noexcept {
        if (!(lead > 0.0)) return 0;
        if (!std::isfinite(lead) ||
            lead >= static_cast<double>(
                std::numeric_limits<std::uint64_t>::max())) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return static_cast<std::uint64_t>(std::floor(lead));
    }

    void enqueue_forward_stream_boundary(
        SamplerVoice& voice,
        double source_frames_per_output) noexcept {
        if (!voice.stream_boundary_pending) return;
        const auto& asset = voice.streamed_asset;
        if (!asset.valid() || !asset.has_stream_source ||
            asset.stream_source.page_frames == 0 ||
            asset.preload_frames >= asset.total_frames) {
            voice.stream_boundary_pending = false;
            voice.stream_boundary_demand_index = 0;
            return;
        }

        const auto page_frames = asset.stream_source.page_frames;
        const auto first_page = asset.preload_frames / page_frames;
        const auto guarded_frame = std::min(
            asset.total_frames - 1,
            asset.preload_frames +
                asset.preload_contract.interpolation_guard_frames);
        const auto last_page = guarded_frame / page_frames;
        const auto demand_count =
            static_cast<std::uint32_t>(last_page - first_page + 1);
        while (voice.stream_boundary_demand_index < demand_count) {
            const auto page = first_page + voice.stream_boundary_demand_index;
            const auto page_start = page * page_frames;
            const auto first_use = std::max(asset.preload_frames, page_start);
            const auto distance = std::max(
                0.0,
                static_cast<double>(first_use) -
                    voice.stream_reader.cursor().position());
            if (streaming_.command_inbox().demand_page({
                    .source = asset.source,
                    .requester = voice.requester,
                    .page_index = page,
                    .resident_source_frames =
                        stream_lead_source_frames(distance),
                    .consumption_frames_per_second =
                        source_frames_per_output * host_sample_rate_,
                    .demand_class = audio::SampleStreamDemandClass::Attack,
                }) != audio::SampleStreamCommandPushStatus::Enqueued) {
                return;
            }
            ++voice.stream_boundary_demand_index;
        }
        voice.stream_boundary_pending = false;
        voice.stream_boundary_demand_index = 0;
    }

    void trigger_note(int note,
                      float velocity,
                      const SamplerPublishedSource& source,
                      const RenderParams& params) {
        SamplerVoice* target = nullptr;
        std::size_t target_index = 0;
        for (std::size_t index = 0; index < kMaxVoices; ++index) {
            if (!voices_[index].active && !pending_cancellations_[index].valid) {
                target = &voices_[index];
                target_index = index;
                break;
            }
        }
        if (target == nullptr) {
            for (std::size_t index = 0; index < kMaxVoices; ++index) {
                if (pending_cancellations_[index].valid) continue;
                target = &voices_[index];
                target_index = index;
                break;
            }
        }
        if (target == nullptr) return;

        if (source.kind == SamplerPublishedSourceKind::Streamed) {
            const auto pitch_ratio = key_map_.pitch_ratio_for_note(
                note, params.pitch_semitones);
            if (!(pitch_ratio > 0.0) ||
                pitch_ratio > SamplerStreamingRuntime::maximum_pitch_ratio())
                return;
            auto& requester_generation = requester_generations_[target_index];
            if (++requester_generation == 0) ++requester_generation;
            const auto base_source_frames_per_output =
                pitch_ratio * static_cast<double>(source.streamed.sample_rate) /
                static_cast<double>(host_sample_rate_);
            const auto* mip = select_streamed_mip(
                source, params.interpolation, base_source_frames_per_output,
                params.loop, params.reverse);
            const auto& selected = mip == nullptr
                ? source.streamed
                : mip->asset;
            const auto source_frames_per_output =
                pitch_ratio * selected.sample_rate /
                static_cast<double>(host_sample_rate_);
            if (!stream_rate_contract_allows(selected,
                                             source_frames_per_output,
                                             params,
                                             target_index,
                                             false)) {
                streaming_.record_aggregate_rate_admission_rejection();
                return;
            }
            if (target->streamed) {
                queue_voice_cancellation(target_index, target->requester);
                if (pending_cancellations_[target_index].valid) return;
            }
            const auto region = make_region(selected.total_frames,
                                            selected.sample_rate,
                                            params.loop,
                                            params.reverse);
            target->start_streamed(
                note,
                velocity,
                host_sample_rate_,
                selected,
                region,
                source_frames_per_output,
                prepared_rate_safe_interpolation(
                    params.interpolation, source_frames_per_output),
                {target_index + 1, requester_generation},
                source.selection_generation,
                mip == nullptr ? 0 : mip->octave);
            return;
        }

        const auto& sample = source.resident;
        if (target->streamed) {
            queue_voice_cancellation(target_index, target->requester);
            if (pending_cancellations_[target_index].valid) return;
        }
        const auto base_speed = playback_speed(note, sample, params);
        if (!(base_speed > 0.0)) return;
        const auto mip = select_resident_mip(source, params.interpolation,
                                             base_speed, params.loop,
                                             params.reverse);
        const auto selected_frames = mip.valid() ? mip.frames : sample.num_frames;
        const auto selected_rate = mip.valid() ? mip.sample_rate : sample.sample_rate;
        const auto region = make_region(selected_frames, selected_rate,
                                        params.loop, params.reverse);
        const auto speed = playback_speed(note, selected_rate, params);
        const auto interpolation = prepared_rate_safe_interpolation(
            params.interpolation, speed);
        if (!interpolation.valid()) return;
        target->start(note, velocity, speed, host_sample_rate_, sample, mip, region,
                      selected_frames, interpolation);
        target->selection_generation = source.selection_generation;
    }

    void queue_voice_cancellation(
        std::size_t voice_index,
        audio::SampleStreamRequesterToken requester) noexcept {
        if (requester.requester_id == 0 || requester.requester_generation == 0)
            return;
        if (streaming_.command_inbox().cancel_requester(requester) ==
            audio::SampleStreamCommandPushStatus::Enqueued) {
            return;
        }
        auto& pending = pending_cancellations_[voice_index];
        if (!pending.valid) pending = {requester, true};
    }

    void flush_pending_cancellations() noexcept {
        for (auto& pending : pending_cancellations_) {
            if (!pending.valid) continue;
            if (streaming_.command_inbox().cancel_requester(pending.requester) !=
                audio::SampleStreamCommandPushStatus::Enqueued) {
                return;
            }
            pending = {};
        }
    }

    void release_note(int note) {
        for (auto& voice : voices_) {
            if (voice.active && voice.note == note && !voice.released) {
                voice.release();
            }
        }
    }

    void reset_all_voices() noexcept {
        for (std::size_t index = 0; index < kMaxVoices; ++index) {
            auto& voice = voices_[index];
            if (voice.active && voice.streamed)
                queue_voice_cancellation(index, voice.requester);
            voice.reset();
        }
        flush_pending_cancellations();
    }
};

inline std::unique_ptr<format::Processor> create_pulp_sampler() {
    return std::make_unique<PulpSamplerProcessor>();
}

} // namespace pulp::examples
