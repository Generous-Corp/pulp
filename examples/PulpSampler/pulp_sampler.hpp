#pragma once

/// PulpSampler — sample-buffer sampler with MIDI triggering and ADSR envelope.
/// Demonstrates: controller-thread sample-slot publication, primitive loop
/// rendering, ADSR, pitch shifting, and processor parameter serialization.

#include "sampler_heritage_runtime.hpp"
#include "sampler_heritage_state.hpp"
#include "sampler_source_publication.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/loop_types.hpp>
#include <pulp/audio/sample_key_map.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/audio/sample_stream_consumption.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/signal/adsr.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pulp::examples {

enum SamplerParams : state::ParamID {
    kSamplerGain = 1,
    kSamplerAttack = 2,
    kSamplerDecay = 3,
    kSamplerSustain = 4,
    kSamplerRelease = 5,
    kSamplerPitch = 6,   // semitones offset
    kSamplerLoop = 7,    // 0 = one-shot, 1 = loop
    kSamplerReverse = 8, // 0 = forward entry, 1 = reverse entry
    kSamplerInterpolation = 9,
};

class PulpSamplerProcessor : public format::Processor {
  public:
    static constexpr int kMaxVoices =
        static_cast<int>(SamplerStreamingRuntime::maximum_voice_count());
    static constexpr std::uint32_t kMaxSampleChannels = SamplerSampleStore::kMaxChannels;
    static constexpr std::uint32_t kMaxOutputChannels = 8;

    explicit PulpSamplerProcessor(PulpSamplerConfig config = {});

    ~PulpSamplerProcessor() override;

    format::PluginDescriptor descriptor() const override;

    int latency_samples() const override;

    void define_parameters(state::StateStore& store) override;

    std::vector<std::uint8_t> serialize_plugin_state() const override;

    bool deserialize_plugin_state(std::span<const std::uint8_t> bytes) override;

    /// Load a mono sample buffer. Call off the audio thread after prepare().
    bool load_sample(const float* data, int num_samples, float sample_rate);

    /// Load a sample from interleaved stereo. Call off the audio thread after prepare().
    bool load_sample_stereo(const float* interleaved, int num_frames, float sample_rate);

    /// Load a true ranged WAV or uncompressed AIFF asset. The call may perform
    /// file I/O and wait for the sampler's non-audio owner thread.
    PulpSamplerLoadResult load_sample_file_result(std::string_view path);

    bool load_sample_file(std::string_view path);

    PulpSamplerLoadResult last_load_result() const noexcept;

    bool set_config(PulpSamplerConfig config);

    PulpSamplerConfig config() const;
    PulpSamplerPrepareResult prepare_result() const noexcept;

    bool has_sample() const;

    int sample_length() const;

    PulpSamplerStreamStats stream_stats() const;

    /// Replaces the optional heritage profile while audio is stopped. Existing
    /// voices are reset because a profile change establishes a new processing
    /// timeline and latency contract.
    PulpSamplerHeritageStatus set_heritage_profile(const audio::SampleHeritageProfile& profile);

    /// Disables heritage processing while audio is stopped. Like profile
    /// replacement, this must not race process(). A prepared clock-domain
    /// change rebinds the stream runtime before heritage is disabled.
    PulpSamplerHeritageStatus disable_heritage();

    PulpSamplerHeritageDiagnostics heritage_diagnostics() const;

    PulpSamplerDiagnostics diagnostics() const noexcept;

    format::PrepareResourceUsage
    estimate_prepare_resources(const format::PrepareContext& ctx) const override;

    void prepare(const format::PrepareContext& ctx) override;

    void prepare_transaction(const format::PrepareContext& ctx);

    void release() override;

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const format::ProcessContext&) override;

  private:
    friend struct PulpSamplerTestAccess;
    friend struct PulpSamplerHeritageTestAccess;

    struct RenderParams {
        float gain = 1.0f;
        signal::Adsr::Params adsr;
        float pitch_semitones = 0.0f;
        bool loop = false;
        bool reverse = false;
        audio::SampleInterpolationPolicy interpolation = audio::SampleInterpolationPolicy::Linear;
    };

    enum class StreamRateContract : std::uint8_t {
        Allowed,
        LegacyRejected,
        HeritageRejected,
    };

    struct PendingCancellation {
        audio::SampleStreamRequesterToken requester{};
        bool valid = false;
    };

    struct PublishedHeritageRuntimeState {
        audio::SampleHeritageTypedRuntimeState state{};
        bool valid = false;
    };

    struct StreamDomain {
        float sample_rate = 44100.0f;
        std::uint32_t maximum_block_frames = 512;

        bool operator==(const StreamDomain&) const noexcept = default;
    };

    static_assert(std::is_trivially_copyable_v<PublishedHeritageRuntimeState>);

    static bool heritage_ready(PulpSamplerHeritageStatus status) noexcept;

    std::uint64_t next_diagnostics_epoch_locked() const noexcept;

    StreamDomain current_stream_domain() const noexcept;

    StreamDomain replacement_stream_domain(
        const SamplerHeritageRuntime::PreparedReplacement& candidate) const noexcept;

    PulpSamplerPrepareResult bind_stream_domain_checked(StreamDomain domain,
                                                        std::uint64_t streaming_budget = 0);

    bool rebind_stream_domain(StreamDomain target) noexcept;

    bool apply_heritage_replacement(SamplerHeritageRuntime::PreparedReplacement&& candidate,
                                    PulpSamplerHeritageStatus ready);

    void clear_heritage_runtime_persistence() noexcept;

    void publish_heritage_runtime_snapshot() noexcept;

    static std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) noexcept;

    static void add_envelope_stats(audio::SampleStarvationEnvelopeStats& target,
                                   const audio::SampleStarvationEnvelopeStats& source) noexcept;

    void harvest_voice_envelope(const SamplerVoice& voice) noexcept;

    void reset_voice(SamplerVoice& voice) noexcept;

    void publish_envelope_diagnostics() noexcept;

    void publish_interpolation_diagnostics() noexcept;

    void fail_prepare(PulpSamplerPrepareResult result) noexcept;

    SamplerSourcePublicationOwner source_publication_;
    std::unique_ptr<SamplerStreamingRuntime> streaming_ =
        std::make_unique<SamplerStreamingRuntime>();
    SamplerHeritageRuntime heritage_;
    runtime::SeqLock<PublishedHeritageRuntimeState> callback_runtime_snapshot_{};
    std::atomic<bool> callback_runtime_snapshot_available_{false};
    runtime::SeqLock<PulpSamplerEnvelopeDiagnostics> envelope_snapshot_{};
    std::atomic<bool> envelope_snapshot_available_{false};
    audio::SampleStarvationEnvelopeStats envelope_lifetime_{};
    runtime::SeqLock<PulpSamplerInterpolationDiagnostics> interpolation_snapshot_{};
    std::atomic<bool> interpolation_snapshot_available_{false};
    std::uint64_t sinc_fallback_selections_ = 0;
    std::uint64_t resident_mip_suppressions_ = 0;
    std::uint64_t streamed_mip_suppressions_ = 0;
    std::uint64_t sinc_promotion_suppressions_ = 0;
    PulpSamplerConfig config_{};
    runtime::SeqLock<PulpSamplerPrepareResult> prepare_result_{};
    runtime::SeqLock<PulpSamplerLoadResult> last_load_result_{};
    mutable std::recursive_mutex control_mutex_;
    mutable runtime::SeqLock<PulpSamplerDiagnostics> diagnostics_snapshot_{};
    mutable std::uint64_t diagnostics_snapshot_epoch_ = 0;
    audio::SampleHeritageTypedRuntimeState restored_runtime_state_{};
    audio::SampleHeritageTypedRuntimeState pending_runtime_state_{};
    bool restored_runtime_state_available_ = false;
    bool pending_runtime_state_available_ = false;
    std::unique_ptr<audio::SampleSincKernelBank> sinc_bank_ =
        std::make_unique<audio::SampleSincKernelBank>();
    audio::SampleKeyMap key_map_;
    std::array<std::vector<float>, kMaxOutputChannels> voice_scratch_{};
    std::array<float*, kMaxOutputChannels> voice_scratch_ptrs_{};
    std::array<std::vector<float>, kMaxSampleChannels> stream_source_scratch_{};
    std::array<float*, kMaxSampleChannels> stream_source_scratch_ptrs_{};
    std::vector<std::uint8_t> bus_voice_activity_{};
    float host_sample_rate_ = 44100.0f;
    double stream_output_sample_rate_ = 44100.0;
    std::uint32_t max_block_frames_ = 512;
    std::uint32_t maximum_stream_block_frames_ = 512;
    std::uint32_t prepared_output_channels_ = 2;
    bool prepared_ = false;
    SamplerVoice voices_[kMaxVoices]{};
    std::array<std::uint64_t, kMaxVoices> requester_generations_{};
    std::array<PendingCancellation, kMaxVoices> pending_cancellations_{};
#if defined(PULP_SAMPLER_TEST_HOOKS)
    mutable std::atomic<std::uint64_t> control_diagnostics_attempts_for_test_{0};
    mutable std::atomic<std::uint64_t> control_diagnostics_entries_for_test_{0};
    std::atomic<std::uint64_t> control_load_attempts_for_test_{0};
    std::atomic<std::uint64_t> control_load_entries_for_test_{0};
    std::atomic<std::uint64_t> control_heritage_attempts_for_test_{0};
    std::atomic<std::uint64_t> control_heritage_entries_for_test_{0};
    std::atomic<std::uint64_t> control_release_attempts_for_test_{0};
    std::atomic<std::uint64_t> control_release_entries_for_test_{0};
    bool retire_reverse_attack_after_horizon_for_test_ = false;
    std::uint64_t lookahead_plans_last_callback_for_test_ = 0;
    double stream_rate_capacity_override_for_test_ = 0.0;
    double last_stream_demand_fps_for_test_ = 0.0;
    double last_lookahead_demand_fps_for_test_ = 0.0;
    bool fail_next_stream_domain_prepare_for_test_ = false;
    bool fail_next_stream_domain_source_restore_for_test_ = false;
    bool fail_next_stream_service_prepare_for_test_ = false;
    bool fail_next_stream_slot_allocation_for_test_ = false;
    bool fail_next_stream_thread_start_for_test_ = false;
#endif

    bool published_source_valid(const SamplerPublishedSource& source) const noexcept;

    RenderParams current_params() const;

    static void clear_output(audio::BufferView<float>& output) noexcept;

    static void clear_output_segment(audio::BufferView<float>& output, std::uint32_t start_frame,
                                     std::uint32_t frames) noexcept;

    PulpSamplerPrepareResult prepare_stream_domain();

    audio::LoopRegion make_region(std::uint64_t frames, double sample_rate, bool loop,
                                  bool reverse) const noexcept;

    audio::LoopRegion make_region(const audio::PublishedSampleView& sample,
                                  const RenderParams& params) const noexcept;

    double playback_speed(int note, const audio::PublishedSampleView& sample,
                          const RenderParams& params) const noexcept;

    double playback_speed(int note, double sample_rate, const RenderParams& params) const noexcept;

    static bool polynomial_mip_policy(audio::SampleInterpolationPolicy policy) noexcept;

    SamplerMipLevelView select_resident_mip(const SamplerPublishedSource& source,
                                            audio::SampleInterpolationPolicy policy,
                                            double base_source_frames_per_output, bool loop,
                                            bool reverse) noexcept;

    const SamplerStreamMipLevelView* select_streamed_mip(const SamplerPublishedSource& source,
                                                         audio::SampleInterpolationPolicy policy,
                                                         double base_source_frames_per_output,
                                                         bool loop, bool reverse) noexcept;

    audio::PreparedSampleInterpolation
    prepared_interpolation(audio::SampleInterpolationPolicy policy,
                           double source_frames_per_output) noexcept;

    audio::PreparedSampleInterpolation
    prepared_rate_safe_interpolation(audio::SampleInterpolationPolicy policy,
                                     double source_frames_per_output) noexcept;

    audio::PreparedSampleInterpolation
    prepared_pitch_source_interpolation(audio::SampleInterpolationPolicy policy,
                                        double source_frames_per_output,
                                        double artifact_source_frames_per_output) noexcept;

    StreamRateContract stream_rate_contract(const audio::SampleAssetView& candidate,
                                            double candidate_source_frames_per_output,
                                            const RenderParams& params,
                                            std::size_t replaced_voice) const noexcept;

    StreamRateContract pitch_rate_contract(double pitch_ratio) const noexcept;

    audio::SampleStreamConsumptionDeclaration
    stream_consumption_declaration(const SamplerVoice& voice) const noexcept;

    void render_output_segment(audio::BufferView<float>& output, std::uint32_t start_frame,
                               std::uint32_t frames, const RenderParams& params) noexcept;

    void render_active_voices(audio::BufferView<float>& output, std::uint32_t start_frame,
                              std::uint32_t frames, const RenderParams& params) noexcept;

    void render_streamed_voice(std::size_t voice_index, SamplerVoice& voice,
                               audio::BufferView<float>& output, std::uint32_t output_start,
                               std::uint32_t frames, std::uint32_t output_channels,
                               const RenderParams& params) noexcept;

    bool prepare_reverse_attack_horizon(SamplerVoice& voice, double) noexcept;

    static bool stream_plan_pages_ready(const audio::SampleStreamLoopBlockPlan& plan) noexcept;

#if defined(PULP_SAMPLER_TEST_HOOKS)
    void retire_reverse_attack_page_after_horizon_for_test(const SamplerVoice& voice) noexcept;
#endif

    void enqueue_stream_lookahead(SamplerVoice& voice, std::uint32_t frames,
                                  double source_frames_per_output) noexcept;

    static std::uint64_t stream_lead_source_frames(double lead) noexcept;

    void enqueue_forward_stream_boundary(SamplerVoice& voice, double) noexcept;

    void trigger_note(int note, float velocity, const SamplerPublishedSource& source,
                      const RenderParams& params);

    void queue_voice_cancellation(std::size_t voice_index,
                                  audio::SampleStreamRequesterToken requester) noexcept;

    void flush_pending_cancellations() noexcept;

    void release_note(int note);

    void reset_all_voices() noexcept;
};

inline std::unique_ptr<format::Processor> create_pulp_sampler() {
    return std::make_unique<PulpSamplerProcessor>();
}

} // namespace pulp::examples
