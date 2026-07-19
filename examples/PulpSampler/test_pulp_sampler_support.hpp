#pragma once

#include "pulp_sampler.hpp"
#include "rt_allocation_probe.hpp"
#include "sampler_stream_mip_sidecar.hpp"
#include "../../test/support/sampler_parity.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/scope_guard.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <set>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;

namespace pulp::examples {

struct PulpSamplerTestAccess {
    static std::pair<std::uint64_t, std::uint64_t> control_load_counts(
        const PulpSamplerProcessor& processor) noexcept {
        return {
            processor.control_load_attempts_for_test_.load(
                std::memory_order_acquire),
            processor.control_load_entries_for_test_.load(
                std::memory_order_acquire),
        };
    }

    static std::pair<std::uint64_t, std::uint64_t> control_diagnostics_counts(
        const PulpSamplerProcessor& processor) noexcept {
        return {
            processor.control_diagnostics_attempts_for_test_.load(
                std::memory_order_acquire),
            processor.control_diagnostics_entries_for_test_.load(
                std::memory_order_acquire),
        };
    }

    static std::pair<std::uint64_t, std::uint64_t> control_release_counts(
        const PulpSamplerProcessor& processor) noexcept {
        return {
            processor.control_release_attempts_for_test_.load(
                std::memory_order_acquire),
            processor.control_release_entries_for_test_.load(
                std::memory_order_acquire),
        };
    }

    static void fail_next_service_prepare(PulpSamplerProcessor& processor) {
        processor.fail_next_stream_service_prepare_for_test_ = true;
    }

    static void fail_next_prepare_allocation(PulpSamplerProcessor& processor) {
        processor.fail_next_stream_slot_allocation_for_test_ = true;
    }

    static bool fully_released(const PulpSamplerProcessor& processor) {
        const auto stats = processor.streaming_->stats();
        return !processor.prepared_ &&
               processor.streaming_->published_source().kind ==
                   SamplerPublishedSourceKind::None &&
               stats.total_memory_capacity_bytes == 0 &&
               stats.current_total_memory_bytes == 0 &&
               processor.streaming_->fully_released_for_test() &&
               processor.source_publication_.fully_released_for_test() &&
               !processor.sinc_bank_->view().valid() &&
               processor.heritage_.processing_released_for_test() &&
               std::all_of(processor.voice_scratch_.begin(),
                           processor.voice_scratch_.end(),
                           [](const auto& scratch) {
                               return scratch.empty() && scratch.capacity() == 0;
                           });
    }

    static void fail_next_thread_start(PulpSamplerProcessor& processor) {
        processor.fail_next_stream_thread_start_for_test_ = true;
    }

    static void fail_next_resident_prepare(PulpSamplerProcessor& processor) {
        processor.source_publication_.fail_next_resident_prepare_for_test();
    }

    static void fail_next_mip_prepare(PulpSamplerProcessor& processor) {
        processor.source_publication_.fail_next_mip_prepare_for_test();
    }

    static void seed_and_harvest_envelope(PulpSamplerProcessor& processor,
                                          std::uint64_t starved_frames) {
        for (auto& voice : processor.voices_) {
            if (!voice.active || !voice.streamed) continue;
            voice.stream_reader.mark_held_starvation(
                static_cast<std::uint32_t>(starved_frames));
            processor.reset_voice(voice);
            return;
        }
    }

    static void publish_envelope(PulpSamplerProcessor& processor) {
        processor.publish_envelope_diagnostics();
    }

    static std::size_t mark_all_streamed_voices(
        PulpSamplerProcessor& processor, std::uint32_t frames) {
        std::size_t count = 0;
        for (auto& voice : processor.voices_) {
            if (!voice.active || !voice.streamed) continue;
            voice.stream_reader.mark_held_starvation(frames);
            ++count;
        }
        return count;
    }

    static void set_envelope_lifetime_starved(
        PulpSamplerProcessor& processor, std::uint64_t frames) {
        processor.envelope_lifetime_.starved_frames = frames;
    }

    static audio::SampleStreamSourceToken
    published_stream_source(const PulpSamplerProcessor& processor) {
        return processor.streaming_->published_source().streamed.source;
    }

    static std::uint64_t
    published_stream_page_frames(const PulpSamplerProcessor& processor) {
        return processor.streaming_->published_source().streamed.stream_source.page_frames;
    }

    static std::size_t active_streamed_voices_for_source(
        const PulpSamplerProcessor& processor,
        audio::SampleStreamSourceToken source) {
        std::size_t count = 0;
        for (const auto& voice : processor.voices_) {
            const auto candidate = voice.streamed_asset.source;
            if (voice.active && voice.streamed &&
                candidate.source_id == source.source_id &&
                candidate.source_generation == source.source_generation) {
                ++count;
            }
        }
        return count;
    }

    static std::size_t active_voice_count(const PulpSamplerProcessor& processor) {
        return static_cast<std::size_t>(std::count_if(
            std::begin(processor.voices_), std::end(processor.voices_),
            [](const SamplerVoice& voice) { return voice.active; }));
    }

    static std::array<double, PulpSamplerProcessor::kMaxVoices>
    active_voice_positions(const PulpSamplerProcessor& processor) {
        std::array<double, PulpSamplerProcessor::kMaxVoices> positions{};
        positions.fill(-1.0);
        for (std::size_t index = 0; index < positions.size(); ++index) {
            const auto& voice = processor.voices_[index];
            if (!voice.active) continue;
            positions[index] = voice.streamed
                ? voice.stream_reader.cursor().position()
                : voice.renderer.position();
        }
        return positions;
    }

    static std::array<std::uint64_t, PulpSamplerProcessor::kMaxVoices>
    requester_generations(const PulpSamplerProcessor& processor) {
        return processor.requester_generations_;
    }

    static audio::SampleStarvationEnvelopeStats
    active_stream_starvation_stats(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_reader.starvation_stats();
        }
        return {};
    }

    static audio::SampleStarvationMode
    active_stream_starvation_mode(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_reader.starvation_mode();
        }
        return audio::SampleStarvationMode::Silent;
    }

    static void reset_voices(PulpSamplerProcessor& processor) {
        processor.reset_all_voices();
    }

    static bool force_active_stream_rate_capacity(PulpSamplerProcessor& processor,
                                                  double frames_per_second) {
        processor.stream_rate_capacity_override_for_test_ = frames_per_second;
        return std::any_of(std::begin(processor.voices_), std::end(processor.voices_),
                           [](const SamplerVoice& voice) {
                               return voice.active && voice.streamed;
                           });
    }

    static std::vector<int> active_streamed_notes(
        const PulpSamplerProcessor& processor) {
        std::vector<int> notes;
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed) notes.push_back(voice.note);
        }
        std::sort(notes.begin(), notes.end());
        return notes;
    }

    static audio::SampleInterpolationPolicy
    interpolation_policy(const PulpSamplerProcessor& processor) {
        return processor.current_params().interpolation;
    }

    static audio::SampleSincKernelBankView sinc_bank(const PulpSamplerProcessor& processor) {
        return processor.sinc_bank_->view();
    }

    static audio::SampleInterpolationPolicy
    active_resident_interpolation(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && !voice.streamed)
                return voice.renderer.interpolation_policy();
        }
        return audio::SampleInterpolationPolicy::Hold;
    }

    static SamplerMipPyramidView resident_mips(const PulpSamplerProcessor& processor) {
        return processor.streaming_->published_source().resident_mips;
    }

    static std::uint32_t active_resident_mip_octave(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && !voice.streamed)
                return voice.resident_mip.octave;
        }
        return 0;
    }

    static double active_resident_position(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && !voice.streamed)
                return voice.renderer.position();
        }
        return 0.0;
    }

    static void pause_stream_dispatch(PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_->service_dispatch_paused_.store(paused, std::memory_order_release);
        processor.streaming_->service_wake_.notify_all();
    }

    static void fail_next_stream_decode(PulpSamplerProcessor& processor) {
        processor.streaming_->fail_next_stream_decode_for_test_.store(
            true, std::memory_order_release);
        processor.streaming_->service_wake_.notify_all();
    }

    static bool invalidate_active_stream_preload_contract(
        PulpSamplerProcessor& processor) {
        for (auto& voice : processor.voices_) {
            if (!voice.active || !voice.streamed) continue;
            voice.streamed_asset.preload_contract.maximum_host_block_frames = 1;
            return true;
        }
        return false;
    }

    static bool pause_stream_command_drain(PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_->service_command_drain_paused_for_test_.store(
            paused, std::memory_order_release);
        processor.streaming_->service_wake_.notify_all();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (processor.streaming_->service_command_drain_paused_ack_for_test_.load(
                   std::memory_order_acquire) != paused &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return processor.streaming_->service_command_drain_paused_ack_for_test_.load(
                   std::memory_order_acquire) == paused;
    }

    static void pause_file_stage(PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_->file_stage_paused_for_test_.store(paused, std::memory_order_release);
    }

    static bool file_stage_paused(const PulpSamplerProcessor& processor) {
        return processor.streaming_->file_stage_paused_ack_for_test_.load(std::memory_order_acquire);
    }

    static std::uint64_t file_stage_attempts(const PulpSamplerProcessor& processor) {
        return processor.streaming_->file_stage_attempts_for_test_.load(std::memory_order_acquire);
    }

    static void throw_during_next_file_stage(PulpSamplerProcessor& processor) {
        processor.streaming_->throw_during_file_stage_for_test_.store(true,
                                                                     std::memory_order_release);
    }

    static std::size_t fill_stream_command_inbox(PulpSamplerProcessor& processor,
                                                 std::size_t remaining_capacity = 0) {
        const auto published = processor.streaming_->published_source();
        if (published.kind != SamplerPublishedSourceKind::Streamed || !published.streamed.valid()) {
            return 0;
        }
        const auto& asset = published.streamed;
        const auto capacity = processor.streaming_->commands_.telemetry().capacity;
        const auto target = remaining_capacity < capacity ? capacity - remaining_capacity : 0;
        std::size_t enqueued = 0;
        while (processor.streaming_->commands_.telemetry().pending < target &&
               processor.streaming_->commands_.demand_page({
                   .source = asset.source,
                   .requester = {0x7175657565, 1},
                   .page_index = 0,
                   .resident_source_frames = 0,
                   .consumption_frames_per_second = static_cast<double>(asset.sample_rate),
                   .demand_class = audio::SampleStreamDemandClass::Sustain,
               }) == audio::SampleStreamCommandPushStatus::Enqueued) {
            ++enqueued;
        }
        return enqueued;
    }

    static void set_reverse_prewarm_timeout(PulpSamplerProcessor& processor,
                                            std::chrono::milliseconds timeout) {
        processor.streaming_->reverse_prewarm_timeout_for_test_ = timeout;
        processor.streaming_->reverse_prewarm_timeout_override_for_test_ = true;
    }

    static bool streamed_tail_page_ready(const PulpSamplerProcessor& processor) {
        const auto published = processor.streaming_->published_source();
        if (published.kind != SamplerPublishedSourceKind::Streamed || !published.streamed.valid() ||
            published.streamed.total_frames == 0) {
            return false;
        }
        const auto& stream = published.streamed.stream_source;
        return stream.window
            ->ready_page_for_frame(published.streamed.source.source_generation,
                                   published.streamed.total_frames - 1)
            .valid;
    }

    static bool streamed_reverse_horizon_ready(const PulpSamplerProcessor& processor) {
        const auto published = processor.streaming_->published_source();
        if (published.kind != SamplerPublishedSourceKind::Streamed || !published.streamed.valid() ||
            published.streamed.total_frames == 0 || published.streamed.preload_frames == 0) {
            return false;
        }
        const auto& asset = published.streamed;
        const auto page_frames = asset.stream_source.page_frames;
        const auto first_frame = asset.total_frames > asset.preload_frames
                                     ? asset.total_frames - asset.preload_frames
                                     : 0;
        const auto first_page = first_frame / page_frames;
        const auto last_page = (asset.total_frames - 1) / page_frames;
        for (auto page = first_page; page <= last_page; ++page) {
            const auto probe_frame = std::max(first_frame, page * page_frames);
            if (!asset.stream_source.window
                     ->ready_page_for_frame(asset.source.source_generation, probe_frame)
                     .valid) {
                return false;
            }
        }
        return true;
    }

    static void retire_reverse_attack_after_horizon(PulpSamplerProcessor& processor) {
        processor.retire_reverse_attack_after_horizon_for_test_ = true;
    }

    static SamplerPublishedSourceKind published_source_kind(const PulpSamplerProcessor& processor) {
        return processor.streaming_->published_source().kind;
    }

    static bool reverse_prewarm_pending(const PulpSamplerProcessor& processor) {
        return processor.streaming_->reverse_prewarm_pending_for_test_.load(
            std::memory_order_acquire);
    }

    static void block_next_reverse_decode(PulpSamplerProcessor& processor) {
        processor.streaming_->reverse_decode_entered_for_test_.store(false,
                                                                    std::memory_order_relaxed);
        processor.streaming_->release_reverse_decode_for_test_.store(false,
                                                                    std::memory_order_release);
        processor.streaming_->block_next_reverse_decode_for_test_.store(true,
                                                                       std::memory_order_release);
    }

    static bool reverse_decode_entered(const PulpSamplerProcessor& processor) {
        return processor.streaming_->reverse_decode_entered_for_test_.load(
            std::memory_order_acquire);
    }

    static void release_reverse_decode(PulpSamplerProcessor& processor) {
        processor.streaming_->release_reverse_decode_for_test_.store(true,
                                                                    std::memory_order_release);
    }

    static std::uint32_t unpublished_rollback_count(const PulpSamplerProcessor& processor) {
        return processor.streaming_->unpublished_rollback_count_for_test_.load(
            std::memory_order_acquire);
    }

    static std::uint64_t unpublished_rollback_attempts(const PulpSamplerProcessor& processor) {
        return processor.streaming_->unpublished_rollback_attempts_for_test_.load(
            std::memory_order_acquire);
    }

    static void fail_after_stream_member_count(PulpSamplerProcessor& processor, int count) {
        processor.streaming_->fail_after_stream_member_count_for_test_.store(
            count, std::memory_order_release);
    }

    static void pause_before_bundle_publish(PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_->pause_before_bundle_publish_for_test_.store(paused,
                                                                         std::memory_order_release);
        processor.streaming_->service_wake_.notify_all();
    }

    static bool bundle_publish_paused(const PulpSamplerProcessor& processor) {
        return processor.streaming_->bundle_publish_paused_ack_for_test_.load(
            std::memory_order_acquire);
    }

    static std::uint64_t physical_stream_source_count(const PulpSamplerProcessor& processor) {
        return processor.streaming_->service_.cache_stats().source_count;
    }

    static std::uint64_t active_stream_bundle_count(const PulpSamplerProcessor& processor) {
        return processor.streaming_->active_sources_.load(std::memory_order_acquire);
    }

    static audio::SampleStreamCacheServiceStats
    stream_cache_stats(const PulpSamplerProcessor& processor) {
        return processor.streaming_->service_.cache_stats();
    }

    static std::size_t stream_source_identity_capacity() noexcept {
        return SamplerStreamingRuntime::kSourceCapacity;
    }

    static std::optional<audio::SampleStreamSourceToken>
    take_stream_source_token(PulpSamplerProcessor& processor,
                             std::size_t identity_index) noexcept {
        return processor.streaming_->take_source_token(identity_index);
    }

    static void set_next_stream_source_generation(
        PulpSamplerProcessor& processor,
        std::size_t identity_index,
        std::uint64_t generation) noexcept {
        if (identity_index < processor.streaming_->next_source_generations_.size())
            processor.streaming_->next_source_generations_[identity_index] = generation;
    }

    static bool service_contains_source(const PulpSamplerProcessor& processor,
                                        audio::SampleStreamSourceToken source) {
        return processor.streaming_->service_.cache_service().contains_source(source);
    }

    static std::uint32_t published_stream_mip_count(const PulpSamplerProcessor& processor) {
        return processor.streaming_->published_source().streamed_mips.level_count;
    }

    static audio::SampleAssetView published_stream_asset(const PulpSamplerProcessor& processor,
                                                         std::uint32_t octave) {
        const auto published = processor.streaming_->published_source();
        if (octave == 0)
            return published.streamed;
        const auto* level = published.streamed_mips.level(octave);
        return level == nullptr ? audio::SampleAssetView{} : level->asset;
    }

    static std::uint64_t published_selection_generation(const PulpSamplerProcessor& processor) {
        return processor.streaming_->published_source().selection_generation;
    }

    static void exhaust_selection_generation(PulpSamplerProcessor& processor) {
        processor.streaming_->selection_generation_ =
            std::numeric_limits<std::uint64_t>::max();
    }

    static std::uint32_t active_streamed_mip_octave(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.streamed_mip_octave;
        }
        return 0;
    }

    static audio::SampleAssetView active_streamed_asset(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.streamed_asset;
        }
        return {};
    }

    static std::uint32_t
    worst_case_dual_region_page_demands(const PulpSamplerProcessor& processor) {
        const auto& streaming = *processor.streaming_;
        const auto source_frames = static_cast<std::uint64_t>(
            std::ceil(static_cast<double>(streaming.maximum_host_block_frames_) *
                      SamplerStreamingRuntime::kMaximumPitchRatio *
                      SamplerStreamingRuntime::kMaximumSourceRate /
                      static_cast<double>(streaming.host_sample_rate_)));
        const auto advance_pages =
            (source_frames + streaming.page_frames_ - 1) / streaming.page_frames_;
        return SamplerStreamingRuntime::kCrossfadeReadRegionCount *
               (static_cast<std::uint32_t>(advance_pages) +
                SamplerStreamingRuntime::kBoundaryPageDemandsPerRegion);
    }

    static constexpr std::uint32_t fixed_voice_demand_capacity() {
        return audio::kSampleStreamVoiceMaxPageDemands;
    }

    static constexpr std::uint32_t cache_pages_per_voice() {
        return SamplerStreamingRuntime::kPagesPerVoiceWorkingSet;
    }

    static std::chrono::milliseconds reverse_prewarm_timeout_for_pages(std::uint32_t page_count) {
        return SamplerStreamingRuntime::reverse_prewarm_timeout_for_pages(page_count);
    }

    static audio::SamplePreloadContract
    published_preload_contract(const PulpSamplerProcessor& processor) {
        return processor.streaming_->published_source().streamed.preload_contract;
    }

    static double active_streamed_position(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_reader.cursor().position();
        }
        return -1.0;
    }

    static std::uint64_t lookahead_plans_last_callback(const PulpSamplerProcessor& processor) {
        return processor.lookahead_plans_last_callback_for_test_;
    }

    static double active_streamed_lookahead_lead(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.lookahead_lead_source_frames;
        }
        return 0.0;
    }

    static bool active_streamed_lookahead_pending(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.pending_lookahead_valid;
        }
        return false;
    }

    static std::uint32_t active_pending_demand_index(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.pending_demand_index;
        }
        return 0;
    }

    static bool active_stream_boundary_pending(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_boundary_pending;
        }
        return false;
    }

    static audio::PreparedSampleInterpolation
    active_stream_interpolation(const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_reader.interpolation();
        }
        return {};
    }

    static std::size_t stream_command_count(const PulpSamplerProcessor& processor) {
        return processor.streaming_->commands_.telemetry().pending;
    }
};

} // namespace pulp::examples

namespace {

struct TempSamplerWav {
    std::string path;

    TempSamplerWav(const char* label, std::uint64_t frames, float value,
                   std::uint32_t sample_rate = 44100) {
        static std::atomic<std::uint64_t> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                (std::string("pulp_sampler_stream_") + label + "_" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" +
                 std::to_string(sequence.fetch_add(1)) + ".wav"))
                   .string();
        audio::AudioFileData data;
        data.sample_rate = sample_rate;
        data.channels = {std::vector<float>(static_cast<std::size_t>(frames), value)};
        REQUIRE(audio::write_wav_file(path, data, audio::WavBitDepth::Float32));
    }

    TempSamplerWav(const char* label, const std::vector<float>& samples) {
        static std::atomic<std::uint64_t> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                (std::string("pulp_sampler_stream_") + label + "_" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" +
                 std::to_string(sequence.fetch_add(1)) + ".wav"))
                   .string();
        audio::AudioFileData data;
        data.sample_rate = 44100;
        data.channels = {samples};
        REQUIRE(audio::write_wav_file(path, data, audio::WavBitDepth::Float32));
    }

    TempSamplerWav(const char* label, std::vector<std::vector<float>> channels,
                   std::uint32_t sample_rate) {
        static std::atomic<std::uint64_t> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                (std::string("pulp_sampler_stream_") + label + "_" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" +
                 std::to_string(sequence.fetch_add(1)) + ".wav"))
                   .string();
        audio::AudioFileData data;
        data.sample_rate = sample_rate;
        data.channels = std::move(channels);
        REQUIRE(audio::write_wav_file(path, data, audio::WavBitDepth::Float32));
    }

    ~TempSamplerWav() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

struct TempSamplerFlac {
    std::string path;

    explicit TempSamplerFlac(const char* label) {
        // A self-contained 32-frame, mono, 48 kHz, 16-bit silent FLAC. Keeping
        // the encoded bytes in the test makes this capability check portable:
        // no encoder executable or optional FLAC writer is needed at runtime.
        constexpr std::string_view kTinyFlacBase64 =
            "ZkxhQ4AAACIQABAAAAAMAAAMC7gA8AAAACA7XTx9IH433O7t0wHjXi5Y"
            "//hqCAAfegAAAPkU";
        const auto decoded = runtime::base64_decode(kTinyFlacBase64);
        REQUIRE(decoded.has_value());

        static std::atomic<std::uint64_t> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                (std::string("pulp_sampler_") + label + "_" +
                 std::to_string(
                     std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" + std::to_string(sequence.fetch_add(1)) + ".flac"))
                   .string();
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
        REQUIRE(output.good());
    }

    ~TempSamplerFlac() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

struct TempSamplerMipSidecar {
    std::string manifest_path;
    std::vector<std::string> payload_paths;

    explicit TempSamplerMipSidecar(const TempSamplerWav& source, std::uint32_t level_count = 2) {
        audio::SampleMipBuildOptions options;
        options.level_count = level_count;
        auto built = audio::build_sample_mip_sidecar(source.path, options);
        INFO(built.error);
        REQUIRE(built.ok);
        manifest_path = std::move(built.manifest_path);
        payload_paths = std::move(built.payload_paths);
    }

    ~TempSamplerMipSidecar() {
        std::error_code error;
        std::filesystem::remove(manifest_path, error);
        for (const auto& path : payload_paths)
            std::filesystem::remove(path, error);
    }
};

struct RetainedSamplerFile {
    std::shared_ptr<audio::MemoryMappedAudioReader> retained;
    audio::FileFrameReader reader;

    explicit RetainedSamplerFile(const std::string& path) {
        reader = audio::make_memory_mapped_frame_reader(
            path, true, true, std::numeric_limits<std::uint64_t>::max(), &retained);
    }
};

struct SamplerProcessBlock {
    explicit SamplerProcessBlock(std::uint32_t frames = 512, double sample_rate = 44100.0)
        : left(frames), right(frames), output_ptrs{left.data(), right.data()},
          output(output_ptrs, 2, frames), input(input_ptrs, 0, frames),
          context{sample_rate, static_cast<int>(frames)} {}

    void run(PulpSamplerProcessor& processor) {
        processor.process(output, input, midi_in, midi_out, context);
    }

    std::vector<float> left;
    std::vector<float> right;
    float* output_ptrs[2];
    const float* input_ptrs[2]{nullptr, nullptr};
    audio::BufferView<float> output;
    audio::BufferView<const float> input;
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    format::ProcessContext context;
};

// Generate a 1-second sine wave at 440 Hz
static std::vector<float> make_sine(float freq = 440.0f, float sr = 44100.0f, int samples = 44100) {
    std::vector<float> data(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        data[static_cast<size_t>(i)] =
            std::sin(2.0f * 3.14159f * freq * static_cast<float>(i) / sr);
    }
    return data;
}

[[maybe_unused]] static std::vector<std::uint8_t>
read_binary_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    const std::vector<char> characters(std::istreambuf_iterator<char>(input), {});
    return {characters.begin(), characters.end()};
}

[[maybe_unused]] static std::string mip_owner_filename_prefix(
    const std::string& source_path, const std::array<std::uint8_t, 32>& source_sha256) {
    std::error_code error;
    const auto source = std::filesystem::path(source_path);
    const auto canonical_parent =
        std::filesystem::weakly_canonical(source.parent_path(), error);
    REQUIRE_FALSE(error);
    const auto spelling =
        (canonical_parent / source.filename()).lexically_normal().generic_string();
    const auto namespace_digest = runtime::sha256(
        reinterpret_cast<const std::uint8_t*>(spelling.data()), spelling.size());
    REQUIRE(namespace_digest.size() == 32);
    return ".pulp-mip-" + runtime::hex_encode(namespace_digest.data(), 12) + "-" +
           runtime::hex_encode(source_sha256.data(), 8) + "-";
}

template <typename Integer>
static void append_unsigned_le(std::vector<std::uint8_t>& bytes, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
        bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}

[[maybe_unused]] static double measured_dc(const std::vector<float>& samples,
                                           std::size_t trim) {
    REQUIRE(samples.size() > trim * 2);
    double sum = 0.0;
    for (std::size_t index = trim; index < samples.size() - trim; ++index)
        sum += samples[index];
    return sum / static_cast<double>(samples.size() - trim * 2);
}

[[maybe_unused]] static double measured_tone_amplitude(
    const std::vector<float>& samples, double frequency, double sample_rate,
    std::size_t trim) {
    REQUIRE(samples.size() > trim * 2);
    double sine = 0.0;
    double cosine = 0.0;
    const auto count = samples.size() - trim * 2;
    for (std::size_t index = trim; index < samples.size() - trim; ++index) {
        const auto phase = 2.0 * std::acos(-1.0) * frequency *
                           static_cast<double>(index) / sample_rate;
        sine += static_cast<double>(samples[index]) * std::sin(phase);
        cosine += static_cast<double>(samples[index]) * std::cos(phase);
    }
    return 2.0 * std::hypot(sine, cosine) / static_cast<double>(count);
}

[[maybe_unused]] static double measured_rms(const std::vector<float>& samples,
                                            std::size_t trim) {
    REQUIRE(samples.size() > trim * 2);
    double sum_squares = 0.0;
    for (std::size_t index = trim; index < samples.size() - trim; ++index)
        sum_squares += static_cast<double>(samples[index]) * samples[index];
    return std::sqrt(sum_squares / static_cast<double>(samples.size() - trim * 2));
}

struct SamplerFixture {
    state::StateStore store;
    std::unique_ptr<PulpSamplerProcessor> proc;

    explicit SamplerFixture(std::uint32_t maximum_block_frames = 512,
                            double host_sample_rate = 44100.0) {
        proc = std::make_unique<PulpSamplerProcessor>();
        proc->set_state_store(&store);
        proc->define_parameters(store);

        format::PrepareContext ctx;
        ctx.sample_rate = host_sample_rate;
        ctx.max_buffer_size = maximum_block_frames;
        ctx.input_channels = 0;
        ctx.output_channels = 2;
        proc->prepare(ctx);

        auto sample = make_sine();
        REQUIRE(proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    }
};

template <typename Predicate>
static bool wait_for_condition(Predicate predicate,
                               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

struct LoaderThreadGuard {
    std::atomic<bool>& running;
    std::thread& loader;

    ~LoaderThreadGuard() {
        running.store(false, std::memory_order_release);
        if (loader.joinable()) {
            loader.join();
        }
    }
};

struct FileStageLoaderGuard {
    PulpSamplerProcessor& processor;
    std::thread& loader;

    ~FileStageLoaderGuard() {
        PulpSamplerTestAccess::pause_file_stage(processor, false);
        if (loader.joinable())
            loader.join();
    }
};

namespace sampler_parity = pulp::test::sampler_parity;

struct ProductionSamplerCapture {
    audio::Buffer<float> output;
    std::array<double, PulpSamplerProcessor::kMaxVoices> final_positions{};
    std::array<std::uint64_t, PulpSamplerProcessor::kMaxVoices>
        requester_generations{};
    PulpSamplerStreamStats stream_stats{};
};

[[maybe_unused]] static void configure_parity_sampler(SamplerFixture& fixture) {
    fixture.store.set_value(kSamplerGain, 0.0f);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    fixture.store.set_value(kSamplerPitch, 0.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    fixture.store.set_value(kSamplerReverse, 0.0f);
    fixture.store.set_value(kSamplerInterpolation, 0.0f);
}

[[maybe_unused]] static ProductionSamplerCapture render_production_sampler_schedule(
    PulpSamplerProcessor& processor,
    std::uint64_t total_frames,
    std::span<const std::uint32_t> partition,
    bool allow_stream_service) {
    // Twelve note-ons guarantee four steals after filling the eight prepared
    // voice slots. Their irregular offsets also cross several callback seams.
    constexpr std::array<std::uint64_t, 12> note_frames = {
        0, 53, 127, 241, 319, 509, 613, 767, 883, 977, 1109, 1261,
    };
    REQUIRE(total_frames > note_frames.back());
    REQUIRE_FALSE(partition.empty());

    ProductionSamplerCapture capture;
    capture.output.resize(2, static_cast<std::size_t>(total_frames));
    std::uint64_t offset = 0;
    std::size_t partition_index = 0;
    while (offset < total_frames) {
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            partition[partition_index], total_frames - offset));
        SamplerProcessBlock block(frames);
        for (const auto event_frame : note_frames) {
            if (event_frame < offset || event_frame >= offset + frames) continue;
            auto event = midi::MidiEvent::note_on(0, 60, 127);
            event.sample_offset = static_cast<std::int32_t>(event_frame - offset);
            block.midi_in.add(event);
        }
        block.run(processor);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            capture.output.channel(0)[static_cast<std::size_t>(offset) + frame] =
                block.left[frame];
            capture.output.channel(1)[static_cast<std::size_t>(offset) + frame] =
                block.right[frame];
        }
        if (allow_stream_service)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        offset += frames;
        partition_index = (partition_index + 1) % partition.size();
    }
    capture.final_positions =
        PulpSamplerTestAccess::active_voice_positions(processor);
    capture.requester_generations =
        PulpSamplerTestAccess::requester_generations(processor);
    capture.stream_stats = processor.stream_stats();
    return capture;
}

}  // namespace
