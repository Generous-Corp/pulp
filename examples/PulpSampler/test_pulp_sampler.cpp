#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "pulp_sampler.hpp"
#include "sampler_stream_mip_sidecar.hpp"
#include "rt_allocation_probe.hpp"
#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;

namespace pulp::examples {

struct PulpSamplerTestAccess {
    static audio::SampleInterpolationPolicy interpolation_policy(
        const PulpSamplerProcessor& processor) {
        return processor.current_params().interpolation;
    }

    static audio::SampleSincKernelBankView sinc_bank(
        const PulpSamplerProcessor& processor) {
        return processor.sinc_bank_.view();
    }

    static audio::SampleInterpolationPolicy active_resident_interpolation(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && !voice.streamed)
                return voice.renderer.interpolation_policy();
        }
        return audio::SampleInterpolationPolicy::Hold;
    }

    static SamplerMipPyramidView resident_mips(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.published_source().resident_mips;
    }

    static std::uint32_t active_resident_mip_octave(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && !voice.streamed)
                return voice.resident_mip.octave;
        }
        return 0;
    }

    static double active_resident_position(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && !voice.streamed)
                return voice.renderer.position();
        }
        return 0.0;
    }

    static void pause_stream_dispatch(PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_.service_dispatch_paused_.store(
            paused, std::memory_order_release);
        processor.streaming_.service_wake_.notify_all();
    }

    static bool pause_stream_command_drain(
        PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_.service_command_drain_paused_for_test_.store(
            paused, std::memory_order_release);
        processor.streaming_.service_wake_.notify_all();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (processor.streaming_.service_command_drain_paused_ack_for_test_.load(
                   std::memory_order_acquire) != paused &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return processor.streaming_.service_command_drain_paused_ack_for_test_.load(
                   std::memory_order_acquire) == paused;
    }

    static void pause_file_stage(PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_.file_stage_paused_for_test_.store(
            paused, std::memory_order_release);
    }

    static bool file_stage_paused(const PulpSamplerProcessor& processor) {
        return processor.streaming_.file_stage_paused_ack_for_test_.load(
            std::memory_order_acquire);
    }

    static std::uint64_t file_stage_attempts(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.file_stage_attempts_for_test_.load(
            std::memory_order_acquire);
    }

    static void throw_during_next_file_stage(PulpSamplerProcessor& processor) {
        processor.streaming_.throw_during_file_stage_for_test_.store(
            true, std::memory_order_release);
    }

    static std::size_t fill_stream_command_inbox(
        PulpSamplerProcessor& processor,
        std::size_t remaining_capacity = 0) {
        const auto published = processor.streaming_.published_source();
        if (published.kind != SamplerPublishedSourceKind::Streamed ||
            !published.streamed.valid()) {
            return 0;
        }
        const auto& asset = published.streamed;
        const auto capacity = processor.streaming_.commands_.telemetry().capacity;
        const auto target = remaining_capacity < capacity
            ? capacity - remaining_capacity
            : 0;
        std::size_t enqueued = 0;
        while (processor.streaming_.commands_.telemetry().pending < target &&
               processor.streaming_.commands_.demand_page({
                   .source = asset.source,
                   .requester = {0x7175657565, 1},
                   .page_index = 0,
                   .resident_source_frames = 0,
                   .consumption_frames_per_second =
                       static_cast<double>(asset.sample_rate),
                   .demand_class = audio::SampleStreamDemandClass::Sustain,
               }) == audio::SampleStreamCommandPushStatus::Enqueued) {
            ++enqueued;
        }
        return enqueued;
    }

    static void set_reverse_prewarm_timeout(
        PulpSamplerProcessor& processor,
        std::chrono::milliseconds timeout) {
        processor.streaming_.reverse_prewarm_timeout_for_test_ = timeout;
        processor.streaming_.reverse_prewarm_timeout_override_for_test_ = true;
    }

    static bool streamed_tail_page_ready(
        const PulpSamplerProcessor& processor) {
        const auto published = processor.streaming_.published_source();
        if (published.kind != SamplerPublishedSourceKind::Streamed ||
            !published.streamed.valid() || published.streamed.total_frames == 0) {
            return false;
        }
        const auto& stream = published.streamed.stream_source;
        return stream.window->ready_page_for_frame(
            published.streamed.source.source_generation,
            published.streamed.total_frames - 1).valid;
    }

    static bool streamed_reverse_horizon_ready(
        const PulpSamplerProcessor& processor) {
        const auto published = processor.streaming_.published_source();
        if (published.kind != SamplerPublishedSourceKind::Streamed ||
            !published.streamed.valid() || published.streamed.total_frames == 0 ||
            published.streamed.preload_frames == 0) {
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
            if (!asset.stream_source.window->ready_page_for_frame(
                    asset.source.source_generation, probe_frame).valid) {
                return false;
            }
        }
        return true;
    }

    static void retire_reverse_attack_after_horizon(
        PulpSamplerProcessor& processor) {
        processor.retire_reverse_attack_after_horizon_for_test_ = true;
    }

    static SamplerPublishedSourceKind published_source_kind(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.published_source().kind;
    }

    static bool reverse_prewarm_pending(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.reverse_prewarm_pending_for_test_.load(
            std::memory_order_acquire);
    }

    static void block_next_reverse_decode(PulpSamplerProcessor& processor) {
        processor.streaming_.reverse_decode_entered_for_test_.store(
            false, std::memory_order_relaxed);
        processor.streaming_.release_reverse_decode_for_test_.store(
            false, std::memory_order_release);
        processor.streaming_.block_next_reverse_decode_for_test_.store(
            true, std::memory_order_release);
    }

    static bool reverse_decode_entered(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.reverse_decode_entered_for_test_.load(
            std::memory_order_acquire);
    }

    static void release_reverse_decode(PulpSamplerProcessor& processor) {
        processor.streaming_.release_reverse_decode_for_test_.store(
            true, std::memory_order_release);
    }

    static std::uint32_t unpublished_rollback_count(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.unpublished_rollback_count_for_test_.load(
            std::memory_order_acquire);
    }

    static std::uint64_t unpublished_rollback_attempts(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.unpublished_rollback_attempts_for_test_.load(
            std::memory_order_acquire);
    }

    static void fail_after_stream_member_count(
        PulpSamplerProcessor& processor, int count) {
        processor.streaming_.fail_after_stream_member_count_for_test_.store(
            count, std::memory_order_release);
    }

    static void pause_before_bundle_publish(
        PulpSamplerProcessor& processor, bool paused) {
        processor.streaming_.pause_before_bundle_publish_for_test_.store(
            paused, std::memory_order_release);
        processor.streaming_.service_wake_.notify_all();
    }

    static bool bundle_publish_paused(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.bundle_publish_paused_ack_for_test_.load(
            std::memory_order_acquire);
    }

    static std::uint64_t physical_stream_source_count(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.service_.cache_stats().source_count;
    }

    static bool service_contains_source(
        const PulpSamplerProcessor& processor,
        audio::SampleStreamSourceToken source) {
        return processor.streaming_.service_.cache_service().contains_source(source);
    }

    static std::uint32_t published_stream_mip_count(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.published_source().streamed_mips.level_count;
    }

    static audio::SampleAssetView published_stream_asset(
        const PulpSamplerProcessor& processor, std::uint32_t octave) {
        const auto published = processor.streaming_.published_source();
        if (octave == 0) return published.streamed;
        const auto* level = published.streamed_mips.level(octave);
        return level == nullptr ? audio::SampleAssetView{} : level->asset;
    }

    static std::uint64_t published_selection_generation(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.published_source().selection_generation;
    }

    static std::uint32_t active_streamed_mip_octave(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.streamed_mip_octave;
        }
        return 0;
    }

    static audio::SampleAssetView active_streamed_asset(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed) return voice.streamed_asset;
        }
        return {};
    }

    static std::uint32_t worst_case_dual_region_page_demands(
        const PulpSamplerProcessor& processor) {
        const auto& streaming = processor.streaming_;
        const auto source_frames = static_cast<std::uint64_t>(std::ceil(
            static_cast<double>(streaming.maximum_host_block_frames_) *
            SamplerStreamingRuntime::kMaximumPitchRatio *
            SamplerStreamingRuntime::kMaximumSourceRate /
            static_cast<double>(streaming.host_sample_rate_)));
        const auto advance_pages =
            (source_frames + streaming.page_frames_ - 1) /
            streaming.page_frames_;
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

    static std::chrono::milliseconds reverse_prewarm_timeout_for_pages(
        std::uint32_t page_count) {
        return SamplerStreamingRuntime::reverse_prewarm_timeout_for_pages(
            page_count);
    }

    static audio::SamplePreloadContract published_preload_contract(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.published_source().streamed.preload_contract;
    }

    static double active_streamed_position(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_reader.cursor().position();
        }
        return -1.0;
    }

    static std::uint64_t lookahead_plans_last_callback(
        const PulpSamplerProcessor& processor) {
        return processor.lookahead_plans_last_callback_for_test_;
    }

    static double active_streamed_lookahead_lead(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.lookahead_lead_source_frames;
        }
        return 0.0;
    }

    static bool active_streamed_lookahead_pending(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.pending_lookahead_valid;
        }
        return false;
    }

    static std::uint32_t active_pending_demand_index(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.pending_demand_index;
        }
        return 0;
    }

    static bool active_stream_boundary_pending(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_boundary_pending;
        }
        return false;
    }

    static audio::PreparedSampleInterpolation active_stream_interpolation(
        const PulpSamplerProcessor& processor) {
        for (const auto& voice : processor.voices_) {
            if (voice.active && voice.streamed)
                return voice.stream_reader.interpolation();
        }
        return {};
    }

    static std::size_t stream_command_count(
        const PulpSamplerProcessor& processor) {
        return processor.streaming_.commands_.telemetry().pending;
    }
};

}  // namespace pulp::examples

struct TempSamplerWav {
    std::string path;

    TempSamplerWav(const char* label,
                   std::uint64_t frames,
                   float value,
                   std::uint32_t sample_rate = 44100) {
        static std::atomic<std::uint64_t> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                (std::string("pulp_sampler_stream_") + label + "_" +
                 std::to_string(sequence.fetch_add(1)) + ".wav")).string();
        audio::AudioFileData data;
        data.sample_rate = sample_rate;
        data.channels = {std::vector<float>(static_cast<std::size_t>(frames), value)};
        REQUIRE(audio::write_wav_file(path, data, audio::WavBitDepth::Float32));
    }

    TempSamplerWav(const char* label, const std::vector<float>& samples) {
        static std::atomic<std::uint64_t> sequence{0};
        path = (std::filesystem::temp_directory_path() /
                (std::string("pulp_sampler_stream_") + label + "_" +
                 std::to_string(sequence.fetch_add(1)) + ".wav")).string();
        audio::AudioFileData data;
        data.sample_rate = 44100;
        data.channels = {samples};
        REQUIRE(audio::write_wav_file(path, data, audio::WavBitDepth::Float32));
    }

    ~TempSamplerWav() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

struct TempSamplerMipSidecar {
    std::string manifest_path;
    std::vector<std::string> payload_paths;

    explicit TempSamplerMipSidecar(const TempSamplerWav& source,
                                   std::uint32_t level_count = 2) {
        const auto base = audio::make_memory_mapped_frame_reader(
            source.path, true, true);
        REQUIRE(base.valid);
        REQUIRE(base.has_content_identity);
        REQUIRE(level_count > 0);
        REQUIRE(level_count <= SamplerStreamMipSidecar::kMaximumLevels);

        struct LevelIdentity {
            audio::FileFrameReader reader;
            std::uint32_t octave = 0;
        };
        std::vector<LevelIdentity> levels;
        auto previous_frames = base.total_frames;
        for (std::uint32_t octave = 1; octave <= level_count; ++octave) {
            const auto decimation = std::uint32_t{1} << octave;
            const auto frames = (previous_frames + 1) / 2;
            const auto path = sampler_stream_mip_detail::payload_path(
                source.path, base.content_sha256, octave);
            audio::AudioFileData data;
            data.sample_rate = static_cast<std::uint32_t>(std::llround(
                static_cast<double>(base.sample_rate) / decimation));
            data.channels.resize(base.channels);
            for (auto& channel : data.channels)
                channel.assign(static_cast<std::size_t>(frames),
                               0.25f / static_cast<float>(octave));
            REQUIRE(audio::write_wav_file(path, data,
                                          audio::WavBitDepth::Float32));
            payload_paths.push_back(path);
            auto reader = audio::make_memory_mapped_frame_reader(path, true, true);
            REQUIRE(reader.valid);
            levels.push_back({std::move(reader), octave});
            previous_frames = frames;
        }

        std::vector<std::uint8_t> bytes;
        const auto append_bytes = [&](const auto* data, std::size_t count) {
            bytes.insert(bytes.end(), data, data + count);
        };
        const auto append_le = [&](auto value) {
            using Integer = decltype(value);
            for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
                bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
        };
        append_bytes(sampler_stream_mip_detail::kMagic.data(),
                     sampler_stream_mip_detail::kMagic.size());
        append_le(sampler_stream_mip_detail::kVersion);
        append_le(sampler_stream_mip_detail::kHeaderBytes);
        append_le(level_count);
        append_bytes(base.content_sha256.data(), base.content_sha256.size());
        append_le(base.mapped_byte_size);
        append_le(base.channels);
        append_le(base.total_frames);
        append_le(base.sample_rate);
        append_le(sampler_stream_mip_detail::kBuilderRevision);
        bytes.insert(bytes.end(), 16, 0);
        previous_frames = base.total_frames;
        for (const auto& level : levels) {
            const auto decimation = std::uint32_t{1} << level.octave;
            const auto frames = (previous_frames + 1) / 2;
            append_le(level.octave);
            append_le(decimation);
            append_le(frames);
            append_le(base.sample_rate);
            append_le(decimation);
            append_le(level.reader.mapped_byte_size);
            append_bytes(level.reader.content_sha256.data(),
                         level.reader.content_sha256.size());
            bytes.insert(bytes.end(), 16, 0);
            previous_frames = frames;
        }
        manifest_path = source.path + ".pulpmip";
        std::ofstream output(manifest_path, std::ios::binary | std::ios::trunc);
        REQUIRE(output.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size())));
    }

    ~TempSamplerMipSidecar() {
        std::error_code error;
        std::filesystem::remove(manifest_path, error);
        for (const auto& path : payload_paths)
            std::filesystem::remove(path, error);
    }
};

struct SamplerProcessBlock {
    explicit SamplerProcessBlock(std::uint32_t frames = 512,
                                 double sample_rate = 44100.0)
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
        data[static_cast<size_t>(i)] = std::sin(2.0f * 3.14159f * freq * static_cast<float>(i) / sr);
    }
    return data;
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
        if (loader.joinable()) loader.join();
    }
};

TEST_CASE("PulpSampler descriptor", "[sampler]") {
    PulpSamplerProcessor proc;
    auto d = proc.descriptor();
    REQUIRE(d.name == "PulpSampler");
    REQUIRE(d.category == format::PluginCategory::Instrument);
    REQUIRE(d.accepts_midi);
    REQUIRE(d.input_buses.empty());
    REQUIRE(d.output_buses.size() == 1);
}

TEST_CASE("PulpSampler has 9 parameters", "[sampler]") {
    SamplerFixture f;
    REQUIRE(f.store.param_count() == 9);
}

TEST_CASE("PulpSampler exposes each scalar interpolation policy", "[sampler]") {
    SamplerFixture f;
    const audio::SampleInterpolationPolicy expected[] = {
        audio::SampleInterpolationPolicy::Hold,
        audio::SampleInterpolationPolicy::Nearest,
        audio::SampleInterpolationPolicy::Linear,
        audio::SampleInterpolationPolicy::CubicHermite,
        audio::SampleInterpolationPolicy::CubicLagrange,
        audio::SampleInterpolationPolicy::RatioTrackingSinc,
    };
    for (std::size_t index = 0; index < std::size(expected); ++index) {
        f.store.set_value(kSamplerInterpolation, static_cast<float>(index));
        REQUIRE(PulpSamplerTestAccess::interpolation_policy(*f.proc) ==
                expected[index]);
    }
    const auto sinc = PulpSamplerTestAccess::sinc_bank(*f.proc);
    REQUIRE(sinc.valid());
    const auto maximum_consumption =
        SamplerStreamingRuntime::maximum_pitch_ratio() *
        SamplerStreamingRuntime::maximum_source_sample_rate() / 44100.0;
    REQUIRE(sinc.select(maximum_consumption).valid());
}

TEST_CASE("Sampler resident mip pyramid preserves DC and octave coordinates",
          "[sampler][mip]") {
    SamplerResidentMipStore store;
    REQUIRE(store.prepare());
    std::vector<float> source(4097, 1.0f);
    REQUIRE(store.stage_mono(source.data(), source.size(), 48000.0, 0));

    const auto view = store.staged_view();
    REQUIRE(view.level_count > 1);
    const auto* level_one = view.level(1);
    REQUIRE(level_one != nullptr);
    REQUIRE(level_one->frames == 2049);
    REQUIRE_THAT(level_one->sample_rate, WithinAbs(24000.0, 1e-9));
    for (std::uint64_t frame = 0; frame < level_one->frames; ++frame) {
        REQUIRE_THAT(level_one->channels[0][frame], WithinAbs(1.0, 2e-6));
    }

    REQUIRE(sampler_exact_mip_octave(1.0) == 0);
    REQUIRE(sampler_exact_mip_octave(1.0001) == 0);
    REQUIRE(sampler_exact_mip_octave(2.0) == 1);
    REQUIRE(sampler_exact_mip_octave(2.0001) == 0);
}

TEST_CASE("Sampler resident mip rejects first-octave aliases",
          "[sampler][mip][quality]") {
    constexpr std::size_t frames = 32768;
    constexpr double pi = 3.14159265358979323846;
    auto render_tone = [&](double cycles_per_frame) {
        std::vector<float> source(frames);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            source[frame] = static_cast<float>(std::sin(
                2.0 * pi * cycles_per_frame * static_cast<double>(frame)));
        }
        SamplerResidentMipStore store;
        REQUIRE(store.prepare());
        REQUIRE(store.stage_mono(source.data(), source.size(), 48000.0, 0));
        const auto view = store.staged_view();
        const auto* level = view.level(1);
        REQUIRE(level != nullptr);
        double energy = 0.0;
        std::size_t count = 0;
        for (std::size_t frame = 512;
             frame + 512 < level->frames;
             ++frame) {
            const auto sample = static_cast<double>(level->channels[0][frame]);
            energy += sample * sample;
            ++count;
        }
        return std::sqrt(energy / static_cast<double>(count));
    };

    const auto passband_rms = render_tone(0.10);
    const auto stopband_rms = render_tone(0.35);
    REQUIRE_THAT(passband_rms * std::sqrt(2.0), WithinAbs(1.0, 1e-3));
    REQUIRE(stopband_rms < 1e-6);
}

TEST_CASE("Streamed mip sidecar validates source and payload identity",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_valid", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source);
    const auto base = audio::make_memory_mapped_frame_reader(
        source.path, true, true);
    REQUIRE(base.valid);

    auto loaded = load_sampler_stream_mip_sidecar(source.path, base);
    REQUIRE(loaded.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(loaded.level_count == 2);
    REQUIRE(loaded.levels[0].reader.total_frames == 2048);
    REQUIRE(loaded.levels[0].sample_rate == 24000);
    REQUIRE(loaded.levels[1].reader.total_frames == 1024);
    REQUIRE(loaded.levels[1].sample_rate == 12000);
    REQUIRE(loaded.levels[0].reader.content_sha256 !=
            loaded.levels[1].reader.content_sha256);
}

TEST_CASE("Streamed mip sidecar preserves fractional logical sample rates",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_fractional_rate", 4096, 0.5f, 22050);
    TempSamplerMipSidecar sidecar(source);
    const auto base = audio::make_memory_mapped_frame_reader(
        source.path, true, true);
    REQUIRE(base.valid);

    auto loaded = load_sampler_stream_mip_sidecar(source.path, base);
    REQUIRE(loaded.status == SamplerStreamMipSidecarStatus::Valid);
    REQUIRE(loaded.levels[0].sample_rate == 11025.0);
    REQUIRE(loaded.levels[0].reader.sample_rate == 11025);
    REQUIRE(loaded.levels[1].sample_rate == 5512.5);
    REQUIRE(loaded.levels[1].reader.sample_rate == 5513);
}

TEST_CASE("Streamed mip sidecar rejects stale and truncated manifests",
          "[sampler][mip][stream][sidecar]") {
    SECTION("stale source digest") {
        TempSamplerWav source("mip_sidecar_stale", 4096, 0.5f, 48000);
        TempSamplerMipSidecar sidecar(source);
        std::fstream manifest(sidecar.manifest_path,
                              std::ios::binary | std::ios::in | std::ios::out);
        manifest.seekp(16);
        const char corrupt = static_cast<char>(0xff);
        REQUIRE(manifest.write(&corrupt, 1));
        manifest.close();
        const auto base = audio::make_memory_mapped_frame_reader(
            source.path, true, true);
        REQUIRE(load_sampler_stream_mip_sidecar(source.path, base).status ==
                SamplerStreamMipSidecarStatus::Invalid);
    }
    SECTION("truncated record") {
        TempSamplerWav source("mip_sidecar_truncated", 4096, 0.5f, 48000);
        TempSamplerMipSidecar sidecar(source);
        std::error_code error;
        std::filesystem::resize_file(sidecar.manifest_path, 100, error);
        REQUIRE_FALSE(error);
        const auto base = audio::make_memory_mapped_frame_reader(
            source.path, true, true);
        REQUIRE(load_sampler_stream_mip_sidecar(source.path, base).status ==
                SamplerStreamMipSidecarStatus::Invalid);
    }
}

TEST_CASE("Streamed mip sidecar rejects payload replacement",
          "[sampler][mip][stream][sidecar]") {
    TempSamplerWav source("mip_sidecar_payload", 4096, 0.5f, 48000);
    TempSamplerMipSidecar sidecar(source);
    std::ofstream payload(sidecar.payload_paths[0],
                          std::ios::binary | std::ios::app);
    const char extra = 0;
    REQUIRE(payload.write(&extra, 1));
    payload.close();
    const auto base = audio::make_memory_mapped_frame_reader(
        source.path, true, true);
    REQUIRE(load_sampler_stream_mip_sidecar(source.path, base).status ==
            SamplerStreamMipSidecarStatus::Invalid);
}

TEST_CASE("PulpSampler atomically publishes authenticated streamed mip bundles",
          "[sampler][mip][stream][integration]") {
    TempSamplerWav source("mip_bundle_publish", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);
    SamplerFixture fixture;
    const auto resident_generation =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
    PulpSamplerTestAccess::pause_before_bundle_publish(*fixture.proc, true);

    std::atomic<bool> loaded{false};
    std::thread loader([&] {
        loaded.store(fixture.proc->load_sample_file(source.path),
                     std::memory_order_release);
    });
    const bool reached_publish_barrier = wait_for_condition([&] {
        return PulpSamplerTestAccess::bundle_publish_paused(*fixture.proc);
    });
    const auto kind_while_paused =
        PulpSamplerTestAccess::published_source_kind(*fixture.proc);
    const auto generation_while_paused =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
    PulpSamplerTestAccess::pause_before_bundle_publish(*fixture.proc, false);
    loader.join();

    REQUIRE(reached_publish_barrier);
    REQUIRE(kind_while_paused == SamplerPublishedSourceKind::Resident);
    REQUIRE(generation_while_paused == resident_generation);
    REQUIRE(loaded.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::published_stream_mip_count(*fixture.proc) == 2);
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 3);
    REQUIRE(fixture.proc->stream_stats().active_sources == 1);
    const auto base = PulpSamplerTestAccess::published_stream_asset(
        *fixture.proc, 0);
    const auto level_one = PulpSamplerTestAccess::published_stream_asset(
        *fixture.proc, 1);
    const auto level_two = PulpSamplerTestAccess::published_stream_asset(
        *fixture.proc, 2);
    REQUIRE(base.valid());
    REQUIRE(level_one.valid());
    REQUIRE(level_two.valid());
    REQUIRE(base.source.source_id != level_one.source.source_id);
    REQUIRE(level_one.source.source_id != level_two.source.source_id);
    REQUIRE(level_one.sample_rate == 22050.0);
    REQUIRE(level_two.sample_rate == 11025.0);
}

TEST_CASE("PulpSampler invalid streamed mip sidecars fall back to the base",
          "[sampler][mip][stream][integration]") {
    TempSamplerWav source("mip_bundle_invalid", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);
    std::fstream manifest(sidecar.manifest_path,
                          std::ios::binary | std::ios::in | std::ios::out);
    manifest.seekp(16);
    const char corrupt = static_cast<char>(0xff);
    REQUIRE(manifest.write(&corrupt, 1));
    manifest.close();
    SamplerFixture fixture;

    REQUIRE(fixture.proc->load_sample_file(source.path));
    REQUIRE(PulpSamplerTestAccess::published_stream_mip_count(*fixture.proc) == 0);
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 1);
    REQUIRE(PulpSamplerTestAccess::published_stream_asset(
                *fixture.proc, 0).valid());
}

TEST_CASE("PulpSampler rolls back every partial streamed mip admission",
          "[sampler][mip][stream][integration][rollback]") {
    TempSamplerWav source("mip_bundle_rollback", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);
    SamplerFixture fixture;
    const auto resident_generation =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);

    for (int admitted_members = 0; admitted_members <= 3; ++admitted_members) {
        CAPTURE(admitted_members);
        const auto attempts_before =
            PulpSamplerTestAccess::unpublished_rollback_attempts(*fixture.proc);
        PulpSamplerTestAccess::fail_after_stream_member_count(
            *fixture.proc, admitted_members);
        REQUIRE_FALSE(fixture.proc->load_sample_file(source.path));
        PulpSamplerTestAccess::fail_after_stream_member_count(*fixture.proc, -1);
        REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
                SamplerPublishedSourceKind::Resident);
        REQUIRE(PulpSamplerTestAccess::published_selection_generation(
                    *fixture.proc) == resident_generation);
        REQUIRE(wait_for_condition([&] {
            return PulpSamplerTestAccess::unpublished_rollback_count(
                       *fixture.proc) == 0 &&
                   PulpSamplerTestAccess::physical_stream_source_count(
                       *fixture.proc) == 0;
        }));
        REQUIRE(PulpSamplerTestAccess::unpublished_rollback_attempts(
                    *fixture.proc) - attempts_before ==
                static_cast<std::uint64_t>(admitted_members));
    }

    REQUIRE(fixture.proc->load_sample_file(source.path));
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 3);
}

TEST_CASE("PulpSampler selects and latches exact streamed mip octaves",
          "[sampler][mip][stream][integration][interpolation]") {
    TempSamplerWav source("mip_bundle_select", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(source.path));
    const auto level_one = PulpSamplerTestAccess::published_stream_asset(
        *fixture.proc, 1);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 1);
    const auto active = PulpSamplerTestAccess::active_streamed_asset(*fixture.proc);
    REQUIRE(active.source.source_id == level_one.source.source_id);
    REQUIRE(PulpSamplerTestAccess::active_stream_interpolation(
                *fixture.proc).policy ==
            audio::SampleInterpolationPolicy::CubicHermite);
    REQUIRE_THAT(block.left.front(), WithinAbs(0.25f, 1.0e-6f));

    const auto token = active.source;
    fixture.store.set_value(kSamplerPitch, 1.0f);
    block.midi_in.clear();
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 1);
    REQUIRE(PulpSamplerTestAccess::active_streamed_asset(
                *fixture.proc).source.source_id == token.source_id);
    REQUIRE(PulpSamplerTestAccess::active_stream_interpolation(
                *fixture.proc).policy ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
}

TEST_CASE("PulpSampler preserves fractional streamed mip rates in playback",
          "[sampler][mip][stream][integration][rate]") {
    TempSamplerWav source("mip_bundle_fractional", 24000, 0.5f, 22050);
    TempSamplerMipSidecar sidecar(source);
    SamplerFixture fixture(512, 22050.0);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(source.path));

    SamplerProcessBlock block(64, 22050.0);
    block.midi_in.add(midi::MidiEvent::note_on(0, 84, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 2);
    REQUIRE(PulpSamplerTestAccess::active_streamed_asset(
                *fixture.proc).sample_rate == 5512.5);
    REQUIRE_THAT(PulpSamplerTestAccess::active_streamed_position(*fixture.proc),
                 WithinAbs(64.0, 1.0e-9));
}

TEST_CASE("PulpSampler excludes streamed mips from loops reverse and fractional ratios",
          "[sampler][mip][stream][integration][policy]") {
    TempSamplerWav source("mip_bundle_policy", 24000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(source);

    for (const auto mode : {std::string_view{"loop"},
                            std::string_view{"reverse"},
                            std::string_view{"fractional"}}) {
        CAPTURE(mode);
        SamplerFixture fixture;
        fixture.store.set_value(kSamplerAttack, 0.0f);
        fixture.store.set_value(kSamplerDecay, 0.0f);
        fixture.store.set_value(kSamplerSustain, 100.0f);
        fixture.store.set_value(kSamplerInterpolation, 3.0f);
        fixture.store.set_value(kSamplerLoop, mode == "loop" ? 1.0f : 0.0f);
        fixture.store.set_value(kSamplerReverse,
                                mode == "reverse" ? 1.0f : 0.0f);
        REQUIRE(fixture.proc->load_sample_file(source.path));
        const auto base = PulpSamplerTestAccess::published_stream_asset(
            *fixture.proc, 0);

        SamplerProcessBlock block;
        block.midi_in.add(midi::MidiEvent::note_on(
            0, mode == "fractional" ? 73 : 72, 127));
        block.run(*fixture.proc);
        REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(
                    *fixture.proc) == 0);
        REQUIRE(PulpSamplerTestAccess::active_streamed_asset(
                    *fixture.proc).source.source_id == base.source.source_id);
        REQUIRE_THAT(block.left.front(), WithinAbs(0.5f, 1.0e-6f));
    }
}

TEST_CASE("PulpSampler bounds mip construction without rejecting the base sample",
          "[sampler][mip]") {
    SamplerFixture fixture;
    std::vector<float> source(
        SamplerResidentMipStore::kMaximumBuildSamples + 1, 0.25f);
    REQUIRE(fixture.proc->load_sample(
        source.data(), static_cast<int>(source.size()), 48000.0f));
    REQUIRE(PulpSamplerTestAccess::resident_mips(*fixture.proc).level_count == 0);
    REQUIRE(fixture.proc->sample_length() == static_cast<int>(source.size()));
}

TEST_CASE("PulpSampler latches resident mips for up-pitched polynomial reads",
          "[sampler][mip][interpolation]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);

    const auto mips = PulpSamplerTestAccess::resident_mips(*fixture.proc);
    REQUIRE(mips.level_count > 0);
    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 1);
    const auto peak = *std::max_element(
        block.left.begin(), block.left.end(),
        [](float a, float b) { return std::abs(a) < std::abs(b); });
    REQUIRE(std::abs(peak) > 0.01f);

    const auto position_before_automation =
        PulpSamplerTestAccess::active_resident_position(*fixture.proc);
    fixture.store.set_value(kSamplerPitch, 1.0f);
    fixture.store.set_value(kSamplerInterpolation, 2.0f);
    block.midi_in.clear();
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 1);
    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::Linear);
    const auto automated_advance =
        PulpSamplerTestAccess::active_resident_position(*fixture.proc) -
        position_before_automation;
    REQUIRE_THAT(automated_advance,
                 WithinAbs(512.0 * std::pow(2.0, 1.0 / 12.0), 1e-6));
}

TEST_CASE("PulpSampler uses ratio sinc between resident mip octaves",
          "[sampler][mip][interpolation]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 73, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 0);
    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
}

TEST_CASE("PulpSampler keeps loop boundaries on the base resident asset",
          "[sampler][mip][loop]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 0);
    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
}

TEST_CASE("PulpSampler keeps reverse one-shots on the phase-correct base asset",
          "[sampler][mip][reverse]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_resident_mip_octave(*fixture.proc) == 0);
    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
}

TEST_CASE("PulpSampler keeps extreme resident notes audible when sinc coverage ends",
          "[sampler][interpolation][sinc]") {
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 127, 127));
    block.run(*fixture.proc);

    REQUIRE(PulpSamplerTestAccess::active_resident_interpolation(*fixture.proc) ==
            audio::SampleInterpolationPolicy::CubicHermite);
    const auto peak = *std::max_element(
        block.left.begin(), block.left.end(),
        [](float left, float right) {
            return std::abs(left) < std::abs(right);
        });
    REQUIRE(std::abs(peak) > 0.01f);
}

TEST_CASE("PulpSampler loads sample", "[sampler]") {
    SamplerFixture f;
    REQUIRE(f.proc->has_sample());
    REQUIRE(f.proc->sample_length() == 44100);
}

TEST_CASE("PulpSampler reports invalid sample loads", "[sampler]") {
    SamplerFixture f;
    REQUIRE_FALSE(f.proc->load_sample(nullptr, 128, 44100.0f));
    std::vector<float> sample(128, 0.0f);
    REQUIRE_FALSE(f.proc->load_sample(sample.data(), 0, 44100.0f));
    REQUIRE_FALSE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 0.0f));
    REQUIRE_FALSE(f.proc->load_sample_stereo(sample.data(),
                                             std::numeric_limits<int>::max(),
                                             44100.0f));
}

TEST_CASE("PulpSampler requires prepare before sample loading", "[sampler]") {
    state::StateStore store;
    PulpSamplerProcessor proc;
    proc.set_state_store(&store);
    proc.define_parameters(store);

    std::vector<float> sample(128, 0.0f);
    REQUIRE_FALSE(proc.load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
}

TEST_CASE("PulpSampler silence without MIDI", "[sampler]") {
    SamplerFixture f;

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 512);

    midi::MidiBuffer midi_in, midi_out;
    format::ProcessContext ctx{44100, 512};

    f.proc->process(out, in, midi_in, midi_out, ctx);

    // No MIDI input → silence
    float sum = 0;
    for (int i = 0; i < 512; ++i) sum += std::abs(out_l[static_cast<size_t>(i)]);
    REQUIRE_THAT(sum, WithinAbs(0.0, 0.001));
}

TEST_CASE("PulpSampler produces audio on note-on", "[sampler]") {
    SamplerFixture f;

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 512);

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100)); // Middle C
    format::ProcessContext ctx{44100, 512};

    f.proc->process(out, in, midi_in, midi_out, ctx);

    // Should produce non-zero output
    float peak = 0;
    for (int i = 0; i < 512; ++i) {
        peak = std::max(peak, std::abs(out_l[static_cast<size_t>(i)]));
    }
    REQUIRE(peak > 0.01f);
}

TEST_CASE("PulpSampler process runs under no-allocation guard after prepare",
          "[sampler][rt]") {
    SamplerFixture f;
    f.store.set_value(kSamplerAttack, 0.0f);

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 512);

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100));
    format::ProcessContext ctx{44100, 512};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float peak = 0.0f;
    for (float sample : out_l) {
        peak = std::max(peak, std::abs(sample));
    }
    REQUIRE(peak > 0.01f);
}

TEST_CASE("PulpSampler handles dense MIDI and voice stealing under no-allocation guard",
          "[sampler][rt]") {
    SamplerFixture f;

    std::vector<float> sample(2048, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    std::vector<float> out_l(256, 0), out_r(256, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 256);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 256);

    midi::MidiBuffer midi_in, midi_out;
    for (int i = 0; i <= PulpSamplerProcessor::kMaxVoices; ++i) {
        auto event = midi::MidiEvent::note_on(0, 60 + i, 127);
        event.sample_offset = static_cast<int32_t>(i * 8);
        midi_in.add(event);
    }
    format::ProcessContext ctx{44100, 256};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float sum = 0.0f;
    for (float sample_value : out_l) {
        sum += std::abs(sample_value);
    }
    REQUIRE(sum > 100.0f);
    REQUIRE(out_r[96] > 0.1f);
}

TEST_CASE("PulpSampler sorts and clamps MIDI offsets under no-allocation guard",
          "[sampler][rt]") {
    SamplerFixture f;

    std::vector<float> sample(1024, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    std::vector<float> out_l(130, 123.0f), out_r(130, -123.0f);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 128);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 128);

    midi::MidiBuffer midi_in, midi_out;
    auto later_note = midi::MidiEvent::note_on(0, 60, 127);
    later_note.sample_offset = 64;
    midi_in.add(later_note);

    auto early_note = midi::MidiEvent::note_on(0, 62, 127);
    early_note.sample_offset = -32;
    midi_in.add(early_note);

    auto end_note_off = midi::MidiEvent::note_off(0, 62);
    end_note_off.sample_offset = 999;
    midi_in.add(end_note_off);

    format::ProcessContext ctx{44100, 128};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float early_sum = 0.0f;
    for (int i = 0; i < 16; ++i) early_sum += std::abs(out_l[static_cast<size_t>(i)]);
    REQUIRE(early_sum > 8.0f);
    REQUIRE(out_l[96] > 0.1f);
    REQUIRE(out_r[96] > 0.1f);
    REQUIRE_THAT(out_l[128], WithinAbs(123.0f, 0.0f));
    REQUIRE_THAT(out_r[129], WithinAbs(-123.0f, 0.0f));
}

TEST_CASE("PulpSampler tolerates controller-thread sample loads during process",
          "[sampler][rt][stress]") {
    SamplerFixture f;
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    std::vector<float> sample_a(128, 0.35f);
    std::vector<float> sample_b(128, 0.9f);
    std::atomic<bool> loader_ready{false};
    std::atomic<bool> start{false};
    std::atomic<bool> running{true};
    std::atomic<int> load_attempts{0};
    std::atomic<int> load_successes{0};

    std::thread loader([&] {
        loader_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            if (!running.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }

        for (int i = 0; running.load(std::memory_order_acquire); ++i) {
            const auto& source = (i % 2 == 0) ? sample_a : sample_b;
            load_attempts.fetch_add(1, std::memory_order_relaxed);
            if (f.proc->load_sample(source.data(),
                                    static_cast<int>(source.size()),
                                    44100.0f)) {
                load_successes.fetch_add(1, std::memory_order_relaxed);
            }
            if ((i % 8) == 0) {
                std::this_thread::yield();
            }
        }
    });
    LoaderThreadGuard loader_guard{running, loader};

    std::vector<float> out_l(64, 0), out_r(64, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 64);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 64);
    format::ProcessContext ctx{44100, 64};
    midi::MidiBuffer midi_in, midi_out;

    REQUIRE(wait_for_condition([&] {
        return loader_ready.load(std::memory_order_acquire);
    }));

    start.store(true, std::memory_order_release);
    REQUIRE(wait_for_condition([&] {
        return load_successes.load(std::memory_order_relaxed) > 0;
    }));
    REQUIRE(load_attempts.load(std::memory_order_relaxed) > 0);
    REQUIRE(load_successes.load(std::memory_order_relaxed) > 0);

    const int attempts_before_process = load_attempts.load(std::memory_order_relaxed);
    const int successes_before_process = load_successes.load(std::memory_order_relaxed);
    float observed_peak = 0.0f;
    bool finite_output = true;
    for (int block = 0; block < 400; ++block) {
        midi_in.clear();
        if ((block % 8) == 0) {
            midi_in.add(midi::MidiEvent::note_on(0, 60 + (block % 12), 127));
        }

        {
            pulp::runtime::ScopedNoAlloc guard;
            f.proc->process(out, in, midi_in, midi_out, ctx);
        }

        for (float sample_value : out_l) {
            finite_output = finite_output && std::isfinite(sample_value);
            observed_peak = std::max(observed_peak, std::abs(sample_value));
        }
        for (float sample_value : out_r) {
            finite_output = finite_output && std::isfinite(sample_value);
        }
        std::this_thread::yield();
    }
    const int attempts_after_process = load_attempts.load(std::memory_order_relaxed);
    const int successes_after_process = load_successes.load(std::memory_order_relaxed);

    running.store(false, std::memory_order_release);
    if (loader.joinable()) {
        loader.join();
    }

    REQUIRE(load_attempts.load(std::memory_order_relaxed) > 0);
    REQUIRE(load_successes.load(std::memory_order_relaxed) > 0);
    REQUIRE(attempts_after_process > attempts_before_process);
    REQUIRE(successes_after_process > successes_before_process);
    REQUIRE(finite_output);
    REQUIRE(observed_peak > 0.1f);
}

TEST_CASE("PulpSampler tolerates controller-thread stereo loads during process",
          "[sampler][rt][stress]") {
    SamplerFixture f;
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    constexpr int kFrames = 128;
    std::vector<float> stereo_a(static_cast<std::size_t>(kFrames) * 2u);
    std::vector<float> stereo_b(static_cast<std::size_t>(kFrames) * 2u);
    for (int i = 0; i < kFrames; ++i) {
        stereo_a[static_cast<std::size_t>(i) * 2u] = 0.2f;
        stereo_a[static_cast<std::size_t>(i) * 2u + 1u] = 0.9f;
        stereo_b[static_cast<std::size_t>(i) * 2u] = 0.85f;
        stereo_b[static_cast<std::size_t>(i) * 2u + 1u] = 0.25f;
    }

    std::atomic<bool> loader_ready{false};
    std::atomic<bool> start{false};
    std::atomic<bool> running{true};
    std::atomic<int> load_attempts{0};
    std::atomic<int> load_successes{0};

    std::thread loader([&] {
        loader_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            if (!running.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }

        for (int i = 0; running.load(std::memory_order_acquire); ++i) {
            const auto& source = (i % 2 == 0) ? stereo_a : stereo_b;
            load_attempts.fetch_add(1, std::memory_order_relaxed);
            if (f.proc->load_sample_stereo(source.data(), kFrames, 44100.0f)) {
                load_successes.fetch_add(1, std::memory_order_relaxed);
            }
            if ((i % 8) == 0) {
                std::this_thread::yield();
            }
        }
    });
    LoaderThreadGuard loader_guard{running, loader};

    std::vector<float> out_l(64, 0), out_r(64, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 64);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 64);
    format::ProcessContext ctx{44100, 64};
    midi::MidiBuffer midi_in, midi_out;

    REQUIRE(wait_for_condition([&] {
        return loader_ready.load(std::memory_order_acquire);
    }));

    start.store(true, std::memory_order_release);
    REQUIRE(wait_for_condition([&] {
        return load_successes.load(std::memory_order_relaxed) > 0;
    }));
    REQUIRE(load_attempts.load(std::memory_order_relaxed) > 0);
    REQUIRE(load_successes.load(std::memory_order_relaxed) > 0);

    const int attempts_before_process = load_attempts.load(std::memory_order_relaxed);
    const int successes_before_process = load_successes.load(std::memory_order_relaxed);
    float peak_l = 0.0f;
    float peak_r = 0.0f;
    bool finite_output = true;
    bool channel_diverged = false;
    for (int block = 0; block < 400; ++block) {
        midi_in.clear();
        if ((block % 8) == 0) {
            midi_in.add(midi::MidiEvent::note_on(0, 60 + (block % 12), 127));
        }

        {
            pulp::runtime::ScopedNoAlloc guard;
            f.proc->process(out, in, midi_in, midi_out, ctx);
        }

        for (std::size_t i = 0; i < out_l.size(); ++i) {
            finite_output = finite_output &&
                            std::isfinite(out_l[i]) &&
                            std::isfinite(out_r[i]);
            peak_l = std::max(peak_l, std::abs(out_l[i]));
            peak_r = std::max(peak_r, std::abs(out_r[i]));
            channel_diverged = channel_diverged || std::abs(out_l[i] - out_r[i]) > 0.05f;
        }
        std::this_thread::yield();
    }
    const int attempts_after_process = load_attempts.load(std::memory_order_relaxed);
    const int successes_after_process = load_successes.load(std::memory_order_relaxed);

    running.store(false, std::memory_order_release);
    if (loader.joinable()) {
        loader.join();
    }

    REQUIRE(load_attempts.load(std::memory_order_relaxed) > 0);
    REQUIRE(load_successes.load(std::memory_order_relaxed) > 0);
    REQUIRE(attempts_after_process > attempts_before_process);
    REQUIRE(successes_after_process > successes_before_process);
    REQUIRE(finite_output);
    REQUIRE(peak_l > 0.05f);
    REQUIRE(peak_r > 0.05f);
    REQUIRE(channel_diverged);
}

TEST_CASE("PulpSampler serializes multiple controller loaders during process",
          "[sampler][rt][stress]") {
    SamplerFixture f;
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    constexpr int kFrames = 128;
    std::vector<float> mono_a(kFrames, 0.25f);
    std::vector<float> mono_b(kFrames, 0.8f);
    std::vector<float> stereo_a(static_cast<std::size_t>(kFrames) * 2u);
    std::vector<float> stereo_b(static_cast<std::size_t>(kFrames) * 2u);
    for (int i = 0; i < kFrames; ++i) {
        stereo_a[static_cast<std::size_t>(i) * 2u] = 0.15f;
        stereo_a[static_cast<std::size_t>(i) * 2u + 1u] = 0.95f;
        stereo_b[static_cast<std::size_t>(i) * 2u] = 0.9f;
        stereo_b[static_cast<std::size_t>(i) * 2u + 1u] = 0.2f;
    }

    std::atomic<bool> mono_ready{false};
    std::atomic<bool> stereo_ready{false};
    std::atomic<bool> start{false};
    std::atomic<bool> running{true};
    std::atomic<int> mono_attempts{0};
    std::atomic<int> mono_successes{0};
    std::atomic<int> stereo_attempts{0};
    std::atomic<int> stereo_successes{0};

    std::thread mono_loader([&] {
        mono_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            if (!running.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }

        for (int i = 0; running.load(std::memory_order_acquire); ++i) {
            const auto& source = (i % 2 == 0) ? mono_a : mono_b;
            mono_attempts.fetch_add(1, std::memory_order_relaxed);
            if (f.proc->load_sample(source.data(),
                                    static_cast<int>(source.size()),
                                    44100.0f)) {
                mono_successes.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            std::this_thread::yield();
        }
    });
    LoaderThreadGuard mono_guard{running, mono_loader};

    std::thread stereo_loader([&] {
        stereo_ready.store(true, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            if (!running.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }

        for (int i = 0; running.load(std::memory_order_acquire); ++i) {
            const auto& source = (i % 2 == 0) ? stereo_a : stereo_b;
            stereo_attempts.fetch_add(1, std::memory_order_relaxed);
            if (f.proc->load_sample_stereo(source.data(), kFrames, 44100.0f)) {
                stereo_successes.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            std::this_thread::yield();
        }
    });
    LoaderThreadGuard stereo_guard{running, stereo_loader};

    std::vector<float> out_l(64, 0), out_r(64, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 64);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 64);
    format::ProcessContext ctx{44100, 64};
    midi::MidiBuffer midi_in, midi_out;

    REQUIRE(wait_for_condition([&] {
        return mono_ready.load(std::memory_order_acquire) &&
               stereo_ready.load(std::memory_order_acquire);
    }));

    start.store(true, std::memory_order_release);
    REQUIRE(wait_for_condition([&] {
        return mono_attempts.load(std::memory_order_relaxed) > 0 &&
               stereo_attempts.load(std::memory_order_relaxed) > 0;
    }));

    const int mono_attempts_before = mono_attempts.load(std::memory_order_relaxed);
    const int mono_successes_before = mono_successes.load(std::memory_order_relaxed);
    const int stereo_attempts_before = stereo_attempts.load(std::memory_order_relaxed);
    const int stereo_successes_before = stereo_successes.load(std::memory_order_relaxed);

    float peak_l = 0.0f;
    float peak_r = 0.0f;
    bool finite_output = true;
    for (int block = 0; block < 600; ++block) {
        midi_in.clear();
        if (block >= 150 && (block % 4) == 0) {
            midi_in.add(midi::MidiEvent::note_on(0, 60 + (block % 12), 127));
        }

        {
            pulp::runtime::ScopedNoAlloc guard;
            f.proc->process(out, in, midi_in, midi_out, ctx);
        }

        for (std::size_t i = 0; i < out_l.size(); ++i) {
            finite_output = finite_output &&
                            std::isfinite(out_l[i]) &&
                            std::isfinite(out_r[i]);
            peak_l = std::max(peak_l, std::abs(out_l[i]));
            peak_r = std::max(peak_r, std::abs(out_r[i]));
        }
        std::this_thread::yield();
    }

    const int mono_attempts_after = mono_attempts.load(std::memory_order_relaxed);
    const int mono_successes_after = mono_successes.load(std::memory_order_relaxed);
    const int stereo_attempts_after = stereo_attempts.load(std::memory_order_relaxed);
    const int stereo_successes_after = stereo_successes.load(std::memory_order_relaxed);

    running.store(false, std::memory_order_release);
    if (mono_loader.joinable()) {
        mono_loader.join();
    }
    if (stereo_loader.joinable()) {
        stereo_loader.join();
    }

    REQUIRE(mono_attempts_after > mono_attempts_before);
    REQUIRE(stereo_attempts_after > stereo_attempts_before);
    REQUIRE(mono_successes_after > mono_successes_before);
    REQUIRE(stereo_successes_after > stereo_successes_before);
    REQUIRE(finite_output);
    REQUIRE(peak_l > 0.05f);
    REQUIRE(peak_r > 0.05f);
}

TEST_CASE("PulpSampler handles note release under no-allocation guard",
          "[sampler][rt]") {
    SamplerFixture f;

    std::vector<float> sample(2048, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);
    f.store.set_value(kSamplerRelease, 50.0f);

    std::vector<float> out_l(128, 0), out_r(128, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 128);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 128);
    format::ProcessContext ctx{44100, 128};

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    midi_in.clear();
    auto note_off = midi::MidiEvent::note_off(0, 60);
    note_off.sample_offset = 64;
    midi_in.add(note_off);
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float held_sum = 0.0f;
    float release_sum = 0.0f;
    for (int i = 0; i < 64; ++i) held_sum += std::abs(out_l[static_cast<size_t>(i)]);
    for (int i = 64; i < 128; ++i) release_sum += std::abs(out_l[static_cast<size_t>(i)]);
    REQUIRE(held_sum > 40.0f);
    REQUIRE(release_sum > 40.0f);
}

TEST_CASE("PulpSampler loads interleaved stereo into separate channels",
          "[sampler]") {
    SamplerFixture f;

    std::vector<float> interleaved(512);
    for (std::size_t i = 0; i < 256; ++i) {
        interleaved[i * 2u] = 0.25f;
        interleaved[i * 2u + 1u] = 1.0f;
    }
    REQUIRE(f.proc->load_sample_stereo(interleaved.data(), 256, 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);

    std::vector<float> out_l(128, 0), out_r(128, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 128);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 128);

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    format::ProcessContext ctx{44100, 128};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float peak_l = 0.0f;
    float peak_r = 0.0f;
    for (std::size_t i = 0; i < out_l.size(); ++i) {
        peak_l = std::max(peak_l, std::abs(out_l[i]));
        peak_r = std::max(peak_r, std::abs(out_r[i]));
    }
    REQUIRE(peak_l > 0.1f);
    REQUIRE(peak_r > peak_l * 2.0f);
}

TEST_CASE("PulpSampler respects MIDI sample offsets", "[sampler]") {
    SamplerFixture f;

    std::vector<float> sample(256, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 512);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 512);

    midi::MidiBuffer midi_in, midi_out;
    auto event = midi::MidiEvent::note_on(0, 60, 127);
    event.sample_offset = 128;
    midi_in.add(event);
    format::ProcessContext ctx{44100, 512};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    float pre_sum = 0.0f;
    for (int i = 0; i < 128; ++i) pre_sum += std::abs(out_l[static_cast<size_t>(i)]);
    REQUIRE_THAT(pre_sum, WithinAbs(0.0, 0.001));
    REQUIRE(std::abs(out_l[200]) > 0.01f);
}

TEST_CASE("PulpSampler loops through the primitive loop renderer", "[sampler]") {
    SamplerFixture f;

    std::vector<float> sample(32, 1.0f);
    REQUIRE(f.proc->load_sample(sample.data(), static_cast<int>(sample.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerLoop, 1.0f);

    std::vector<float> out_l(256, 0), out_r(256, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 256);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 256);

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    format::ProcessContext ctx{44100, 256};

    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    REQUIRE(std::abs(out_l[64]) > 0.1f);
    REQUIRE(std::abs(out_l[128]) > 0.1f);
    REQUIRE(std::abs(out_r[128]) > 0.1f);
}

TEST_CASE("PulpSampler keeps active voices on their original sample generation",
          "[sampler]") {
    SamplerFixture f;

    std::vector<float> first(64, 1.0f);
    std::vector<float> second(64, 0.25f);
    REQUIRE(f.proc->load_sample(first.data(), static_cast<int>(first.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);
    f.store.set_value(kSamplerLoop, 1.0f);

    std::vector<float> out_l(128, 0), out_r(128, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 128);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 128);
    format::ProcessContext ctx{44100, 128};

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    REQUIRE(f.proc->load_sample(second.data(), static_cast<int>(second.size()), 44100.0f));
    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    midi_in.clear();
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    REQUIRE(out_l[16] > 0.75f);
    REQUIRE(out_r[16] > 0.75f);
}

TEST_CASE("PulpSampler clears per-voice scratch when short voices finish",
          "[sampler]") {
    SamplerFixture f;

    std::vector<float> first(64, 1.0f);
    REQUIRE(f.proc->load_sample(first.data(), static_cast<int>(first.size()), 44100.0f));
    f.store.set_value(kSamplerAttack, 0.0f);
    f.store.set_value(kSamplerSustain, 100.0f);

    std::vector<float> out_l(16, 0), out_r(16, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    audio::BufferView<float> out(out_ptrs, 2, 16);

    const float* in_ptrs[] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, 16);
    format::ProcessContext ctx{44100, 16};

    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    std::vector<float> second(4, 0.25f);
    REQUIRE(f.proc->load_sample(second.data(), static_cast<int>(second.size()), 44100.0f));
    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    midi_in.clear();
    midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    {
        pulp::runtime::ScopedNoAlloc guard;
        f.proc->process(out, in, midi_in, midi_out, ctx);
    }

    REQUIRE(out_l[1] > 1.1f);
    REQUIRE(out_l[8] > 0.75f);
    REQUIRE(out_l[8] < 1.1f);
    REQUIRE(out_r[8] > 0.75f);
    REQUIRE(out_r[8] < 1.1f);
}

TEST_CASE("PulpSampler state round-trip", "[sampler]") {
    SamplerFixture f;

    f.store.set_value(kSamplerGain, -12.0f);
    f.store.set_value(kSamplerAttack, 50.0f);

    auto saved = f.store.serialize();
    REQUIRE_FALSE(saved.empty());

    f.store.reset_all_to_defaults();
    REQUIRE(f.store.deserialize(saved));
    REQUIRE(std::abs(f.store.get_value(kSamplerGain) - (-12.0f)) < 0.01f);
}

TEST_CASE("PulpSampler streams continuously across the preload boundary",
          "[sampler][stream]") {
    TempSamplerWav wav("boundary", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    const auto preload = fixture.proc->stream_stats().preload_frames;
    REQUIRE(preload > 0);
    REQUIRE(preload < 24000);

    SamplerProcessBlock block;
    const auto pages_before_note =
        fixture.proc->stream_stats().pages_published;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    std::vector<float> rendered = block.left;
    block.midi_in.clear();
    block.run(*fixture.proc);
    rendered.insert(rendered.end(), block.left.begin(), block.left.end());
    block.run(*fixture.proc);
    rendered.insert(rendered.end(), block.left.begin(), block.left.end());
    REQUIRE(wait_for_condition([&] {
        return fixture.proc->stream_stats().pages_published > pages_before_note;
    }));

    while (rendered.size() < preload + 32) {
        block.run(*fixture.proc);
        rendered.insert(rendered.end(), block.left.begin(), block.left.end());
    }

    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    for (std::uint64_t frame = preload - 8; frame < preload + 8; ++frame) {
        REQUIRE_THAT(rendered[static_cast<std::size_t>(frame)], WithinAbs(0.5f, 1e-6));
    }
}

TEST_CASE("PulpSampler retunes streamed sinc across pitch modulation",
          "[sampler][stream][interpolation][sinc][modulation]") {
    TempSamplerWav wav("sinc_modulation", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block(64);
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    auto interpolation = PulpSamplerTestAccess::active_stream_interpolation(
        *fixture.proc);
    REQUIRE(interpolation.policy ==
            audio::SampleInterpolationPolicy::RatioTrackingSinc);
    REQUIRE(interpolation.sinc.wider.cutoff() == 1.0);

    fixture.store.set_value(kSamplerPitch, 12.0f);
    block.midi_in.clear();
    block.run(*fixture.proc);
    interpolation = PulpSamplerTestAccess::active_stream_interpolation(
        *fixture.proc);
    REQUIRE(interpolation.sinc.wider.cutoff() == 0.5);
    REQUIRE(interpolation.sinc.narrower.cutoff() == 0.5);

    fixture.store.set_value(kSamplerInterpolation, 1.0f);
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_stream_interpolation(*fixture.proc).policy ==
            audio::SampleInterpolationPolicy::Nearest);
}

TEST_CASE("PulpSampler prewarms reverse entry before publishing a stream",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry", 24000, 0.5f);
    SamplerFixture fixture;

    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(fixture.proc->stream_stats().preload_frames < 24000);
    REQUIRE(PulpSamplerTestAccess::streamed_tail_page_ready(*fixture.proc));
    const auto contract =
        PulpSamplerTestAccess::published_preload_contract(*fixture.proc);
    const auto evaluated = audio::evaluate_sample_preload_contract(contract);
    REQUIRE(evaluated.valid());
    REQUIRE(contract.loop_prefetch_guard_frames == evaluated.block_guard_frames);
}

TEST_CASE("PulpSampler services active streams while staging a replacement",
          "[sampler][stream][sidecar]") {
    TempSamplerWav active("stage_service_active", 24000, 0.25f);
    TempSamplerWav replacement("stage_service_replacement", 24000, 0.75f);
    TempSamplerMipSidecar sidecar(replacement);
    SamplerFixture fixture;
    REQUIRE(fixture.proc->load_sample_file(active.path));

    PulpSamplerTestAccess::pause_file_stage(*fixture.proc, true);
    std::atomic<bool> load_result{false};
    std::thread loader([&] {
        load_result.store(fixture.proc->load_sample_file(replacement.path),
                          std::memory_order_release);
    });
    FileStageLoaderGuard loader_guard{*fixture.proc, loader};
    REQUIRE(wait_for_condition([&] {
        return PulpSamplerTestAccess::file_stage_paused(*fixture.proc);
    }));

    REQUIRE(PulpSamplerTestAccess::fill_stream_command_inbox(*fixture.proc) > 0);
    REQUIRE(wait_for_condition([&] {
        return PulpSamplerTestAccess::stream_command_count(*fixture.proc) == 0;
    }));

    PulpSamplerTestAccess::pause_file_stage(*fixture.proc, false);
    loader.join();
    REQUIRE(load_result.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::published_stream_mip_count(*fixture.proc) == 2);
}

TEST_CASE("PulpSampler reports file staging exceptions as load failures",
          "[sampler][stream]") {
    TempSamplerWav source("stage_exception", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::throw_during_next_file_stage(*fixture.proc);

    REQUIRE_FALSE(fixture.proc->load_sample_file(source.path));
    REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
            SamplerPublishedSourceKind::Resident);
}

TEST_CASE("PulpSampler establishes the certified lookahead for small blocks",
          "[sampler][stream]") {
    TempSamplerWav wav("small_block_lookahead", 24000, 0.5f);
    SamplerFixture fixture(1);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    const auto preload = fixture.proc->stream_stats().preload_frames;
    REQUIRE(preload > 0);
    REQUIRE(preload < 24000);

    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(
        *fixture.proc, true));
    SamplerProcessBlock block(1);
    block.midi_in.add(midi::MidiEvent::note_on(0, 84, 127));
    {
        runtime::ScopedNoAlloc no_alloc;
        block.run(*fixture.proc);
    }

    REQUIRE_FALSE(PulpSamplerTestAccess::active_stream_boundary_pending(
        *fixture.proc));
    REQUIRE(PulpSamplerTestAccess::stream_command_count(*fixture.proc) > 0);
    REQUIRE(PulpSamplerTestAccess::lookahead_plans_last_callback(
                *fixture.proc) <= 8);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(
        *fixture.proc, false));
}

TEST_CASE("PulpSampler bounds lookahead work for a low-rate pitched asset",
          "[sampler][stream][rt]") {
    TempSamplerWav wav("low_rate_lookahead", 64, 0.5f, 1);
    SamplerFixture fixture;
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(fixture.proc->stream_stats().preload_frames < 64);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 0, 127));
    {
        runtime::ScopedNoAlloc no_alloc;
        block.run(*fixture.proc);
    }

    REQUIRE(PulpSamplerTestAccess::lookahead_plans_last_callback(
                *fixture.proc) > 0);
    REQUIRE(PulpSamplerTestAccess::lookahead_plans_last_callback(
                *fixture.proc) <= 16);
}

TEST_CASE("PulpSampler recovers lookahead after command queue backpressure",
          "[sampler][stream][recovery]") {
    TempSamplerWav wav("lookahead_backpressure", 100000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(
        *fixture.proc, true));
    REQUIRE(PulpSamplerTestAccess::fill_stream_command_inbox(*fixture.proc) > 0);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    for (std::uint32_t callback = 0;
         callback < 8 &&
         !PulpSamplerTestAccess::active_streamed_lookahead_pending(*fixture.proc);
         ++callback) {
        block.run(*fixture.proc);
    }
    REQUIRE(PulpSamplerTestAccess::active_streamed_lookahead_pending(
        *fixture.proc));
    for (std::uint32_t callback = 0;
         callback < 32 &&
         PulpSamplerTestAccess::active_streamed_lookahead_lead(*fixture.proc) >= 0.0;
         ++callback) {
        block.run(*fixture.proc);
    }
    REQUIRE(PulpSamplerTestAccess::active_streamed_lookahead_lead(
                *fixture.proc) < 0.0);

    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(
        *fixture.proc, false));
    REQUIRE(wait_for_condition([&] {
        block.run(*fixture.proc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return !PulpSamplerTestAccess::active_streamed_lookahead_pending(
                   *fixture.proc) &&
               PulpSamplerTestAccess::active_streamed_lookahead_lead(
                   *fixture.proc) > 0.0 &&
               block.left.front() > 0.45f;
    }, std::chrono::seconds(5)));

    const auto recovered_starvation =
        fixture.proc->stream_stats().starved_output_frames;
    for (std::uint32_t callback = 0; callback < 10; ++callback) {
        block.run(*fixture.proc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(fixture.proc->stream_stats().starved_output_frames ==
            recovered_starvation);
    REQUIRE(block.left.front() > 0.45f);
}

TEST_CASE("PulpSampler refreshes a partially queued lookahead prefix",
          "[sampler][stream][recovery]") {
    TempSamplerWav wav("lookahead_partial_prefix", 100000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(
        *fixture.proc, true));
    REQUIRE(PulpSamplerTestAccess::fill_stream_command_inbox(
                *fixture.proc, 1) > 0);

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 84, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    REQUIRE(PulpSamplerTestAccess::active_streamed_lookahead_pending(
        *fixture.proc));
    REQUIRE(PulpSamplerTestAccess::active_pending_demand_index(
                *fixture.proc) > 0);

    REQUIRE(PulpSamplerTestAccess::pause_stream_command_drain(
        *fixture.proc, false));
    REQUIRE(wait_for_condition([&] {
        block.run(*fixture.proc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return !PulpSamplerTestAccess::active_streamed_lookahead_pending(
                   *fixture.proc) &&
               block.left.front() > 0.45f;
    }, std::chrono::seconds(5)));
}

TEST_CASE("PulpSampler emits streamed reverse one-shots from the tail",
          "[sampler][stream][reverse]") {
    std::vector<float> ramp(24000);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame) {
        ramp[frame] = 0.1f + 0.8f * static_cast<float>(frame) /
                                static_cast<float>(ramp.size() - 1);
    }
    TempSamplerWav wav("reverse_render", ramp);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);

    REQUIRE(block.left.front() > 0.85f);
    REQUIRE(block.left[400] < block.left.front());
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
}

TEST_CASE("PulpSampler holds a reverse attack while its tail is refilled",
          "[sampler][stream][reverse]") {
    constexpr std::uint64_t kFrames = 24000;
    std::vector<float> ramp(kFrames);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame) {
        ramp[frame] = 0.1f + 0.8f * static_cast<float>(frame) /
                                static_cast<float>(ramp.size() - 1);
    }
    TempSamplerWav wav("reverse_evicted_attack", ramp);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::streamed_reverse_horizon_ready(*fixture.proc));

    SamplerProcessBlock block;
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);
    PulpSamplerTestAccess::retire_reverse_attack_after_horizon(*fixture.proc);
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    REQUIRE_FALSE(PulpSamplerTestAccess::streamed_reverse_horizon_ready(
        *fixture.proc));
    REQUIRE(std::all_of(block.left.begin(), block.left.end(), [](float sample) {
        return sample == 0.0f;
    }));
    REQUIRE(fixture.proc->stream_stats().starved_output_frames ==
            block.left.size());
    REQUIRE(PulpSamplerTestAccess::active_streamed_position(*fixture.proc) ==
            static_cast<double>(kFrames - 1));

    block.midi_in.clear();
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
    REQUIRE(wait_for_condition([&] {
        if (PulpSamplerTestAccess::streamed_reverse_horizon_ready(
                *fixture.proc)) {
            return true;
        }
        block.run(*fixture.proc);
        return false;
    }, std::chrono::milliseconds(5000)));
    block.run(*fixture.proc);
    REQUIRE(block.left.front() > 0.85f);
    REQUIRE(block.left[400] < block.left.front());
}

TEST_CASE("PulpSampler keeps streamed reverse loops supplied across the seam",
          "[sampler][stream][reverse][loop]") {
    TempSamplerWav wav("reverse_loop", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerLoop, 1.0f);
    fixture.store.set_value(kSamplerReverse, 1.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    const auto blocks = static_cast<std::uint32_t>(24000 / block.left.size()) + 4;
    std::uint32_t first_starved_block = 0;
    for (std::uint32_t index = 1; index < blocks; ++index) {
        block.run(*fixture.proc);
        if (first_starved_block == 0 &&
            fixture.proc->stream_stats().starved_output_frames != 0) {
            first_starved_block = index;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CAPTURE(first_starved_block);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    REQUIRE(block.left.back() > 0.45f);
    REQUIRE(block.right.back() > 0.45f);
}

TEST_CASE("PulpSampler page geometry bounds dual-region crossfade demands",
          "[sampler][stream]") {
    state::StateStore store;
    PulpSamplerProcessor processor;
    processor.set_state_store(&store);
    processor.define_parameters(store);
    format::PrepareContext context;
    context.sample_rate = 44100;
    context.max_buffer_size = 8192;
    context.input_channels = 0;
    context.output_channels = 2;
    processor.prepare(context);

    const auto page_demands =
        PulpSamplerTestAccess::worst_case_dual_region_page_demands(processor);
    REQUIRE(page_demands <=
            PulpSamplerTestAccess::fixed_voice_demand_capacity());
    REQUIRE(page_demands <= PulpSamplerTestAccess::cache_pages_per_voice());
}

TEST_CASE("PulpSampler reverse prewarm deadline scales with its page horizon",
          "[sampler][stream][reverse]") {
    REQUIRE(PulpSamplerTestAccess::reverse_prewarm_timeout_for_pages(1) ==
            std::chrono::milliseconds(250));
    REQUIRE(PulpSamplerTestAccess::reverse_prewarm_timeout_for_pages(14) >=
            std::chrono::milliseconds(420));
}

TEST_CASE("PulpSampler does not publish a stream when reverse prewarm fails",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_failure", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::milliseconds(5));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    REQUIRE_FALSE(fixture.proc->load_sample_file(wav.path));
    REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
            SamplerPublishedSourceKind::Resident);
    REQUIRE(fixture.proc->stream_stats().active_sources == 0);
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
}

TEST_CASE("PulpSampler keeps the resident source published while prewarm waits",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_ordering", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::seconds(2));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    std::atomic<bool> loaded{false};
    std::thread loader([&] {
        loaded.store(fixture.proc->load_sample_file(wav.path),
                     std::memory_order_release);
    });
    const bool reached_prewarm = wait_for_condition([&] {
        return PulpSamplerTestAccess::reverse_prewarm_pending(*fixture.proc);
    });
    const auto kind_while_pending =
        PulpSamplerTestAccess::published_source_kind(*fixture.proc);
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
    loader.join();

    REQUIRE(reached_prewarm);
    REQUIRE(kind_while_pending == SamplerPublishedSourceKind::Resident);
    REQUIRE(loaded.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::streamed_tail_page_ready(*fixture.proc));
}

TEST_CASE("PulpSampler reclaims an in-flight failed prewarm registration",
          "[sampler][stream]") {
    TempSamplerWav failed("reverse_entry_in_flight", 24000, 0.5f);
    TempSamplerWav first("reverse_entry_reuse_a", 24000, 0.25f);
    TempSamplerWav second("reverse_entry_reuse_b", 24000, 0.75f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::milliseconds(20));
    PulpSamplerTestAccess::block_next_reverse_decode(*fixture.proc);

    const bool failed_load = fixture.proc->load_sample_file(failed.path);
    const bool decode_entered =
        PulpSamplerTestAccess::reverse_decode_entered(*fixture.proc);
    const auto rollback_count =
        PulpSamplerTestAccess::unpublished_rollback_count(*fixture.proc);
    PulpSamplerTestAccess::release_reverse_decode(*fixture.proc);
    const bool rollback_completed = wait_for_condition([&] {
        return PulpSamplerTestAccess::unpublished_rollback_count(*fixture.proc) == 0;
    });
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::milliseconds(250));

    REQUIRE_FALSE(failed_load);
    REQUIRE(decode_entered);
    REQUIRE(rollback_count == 1);
    REQUIRE(rollback_completed);
    REQUIRE(fixture.proc->load_sample_file(first.path));
    REQUIRE(fixture.proc->load_sample_file(second.path));
}

TEST_CASE("PulpSampler shutdown rejects an in-flight prewarm admission",
          "[sampler][stream]") {
    TempSamplerWav wav("reverse_entry_shutdown", 24000, 0.5f);
    SamplerFixture fixture;
    PulpSamplerTestAccess::set_reverse_prewarm_timeout(
        *fixture.proc, std::chrono::seconds(2));
    PulpSamplerTestAccess::block_next_reverse_decode(*fixture.proc);

    std::atomic<bool> load_result{true};
    std::thread loader([&] {
        load_result.store(fixture.proc->load_sample_file(wav.path),
                          std::memory_order_release);
    });
    const bool decode_entered = wait_for_condition([&] {
        return PulpSamplerTestAccess::reverse_decode_entered(*fixture.proc);
    });
    fixture.proc->release();
    loader.join();

    REQUIRE(decode_entered);
    REQUIRE_FALSE(load_result.load(std::memory_order_acquire));
    REQUIRE(PulpSamplerTestAccess::published_source_kind(*fixture.proc) ==
            SamplerPublishedSourceKind::None);
}

TEST_CASE("PulpSampler reports deterministic streamed starvation",
          "[sampler][stream]") {
    TempSamplerWav wav("starvation", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, true);

    const auto preload = fixture.proc->stream_stats().preload_frames;
    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    const auto blocks = static_cast<std::uint32_t>(preload / 512) + 3;
    for (std::uint32_t index = 1; index < blocks; ++index) {
        block.run(*fixture.proc);
    }

    REQUIRE(fixture.proc->stream_stats().starved_output_frames > 0);
    REQUIRE_THAT(block.left.back(), WithinAbs(0.0f, 0.0f));
    PulpSamplerTestAccess::pause_stream_dispatch(*fixture.proc, false);
}

TEST_CASE("PulpSampler keeps staggered streamed voices supplied",
          "[sampler][stream][polyphony]") {
    TempSamplerWav wav("polyphony", 200000, 0.25f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block;
    for (int voice = 0; voice < PulpSamplerProcessor::kMaxVoices; ++voice) {
        block.midi_in.clear();
        block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
        block.run(*fixture.proc);
        block.midi_in.clear();
        for (int spacing = 1; spacing < 16; ++spacing) {
            block.run(*fixture.proc);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (int tail = 0; tail < 20; ++tail) {
        block.run(*fixture.proc);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(fixture.proc->stream_stats().pages_published > 0);
    REQUIRE(fixture.proc->stream_stats().starved_output_frames == 0);
    REQUIRE_THAT(block.left.front(), WithinAbs(2.0f, 1e-5));
}

TEST_CASE("PulpSampler retains a replaced stream until its voice acknowledges",
          "[sampler][stream]") {
    TempSamplerWav first("replacement_a", 24000, 0.75f);
    TempSamplerWav second("replacement_b", 24000, 0.25f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    REQUIRE(fixture.proc->load_sample_file(first.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 60, 127));
    block.run(*fixture.proc);
    REQUIRE(fixture.proc->load_sample_file(second.path));
    REQUIRE(fixture.proc->stream_stats().active_sources == 2);
    REQUIRE(fixture.proc->stream_stats().sources_retired == 0);

    block.midi_in.clear();
    block.midi_in.add(midi::MidiEvent::note_off(0, 60));
    block.run(*fixture.proc);
    REQUIRE(wait_for_condition([&] {
        return fixture.proc->stream_stats().active_sources == 1 &&
               fixture.proc->stream_stats().sources_retired == 1;
    }));
}

TEST_CASE("PulpSampler retires replaced streamed mip bundles as one generation",
          "[sampler][mip][stream][integration][lifetime]") {
    TempSamplerWav first("mip_replacement_a", 24000, 0.75f, 44100);
    TempSamplerMipSidecar first_sidecar(first);
    TempSamplerWav second("mip_replacement_b", 24000, 0.5f, 44100);
    TempSamplerMipSidecar second_sidecar(second);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(first.path));
    const std::array first_tokens{
        PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 0).source,
        PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 1).source,
        PulpSamplerTestAccess::published_stream_asset(*fixture.proc, 2).source,
    };

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 1);
    REQUIRE(fixture.proc->load_sample_file(second.path));
    REQUIRE(fixture.proc->stream_stats().active_sources == 2);
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 6);
    for (const auto token : first_tokens) {
        REQUIRE(PulpSamplerTestAccess::service_contains_source(
            *fixture.proc, token));
    }

    block.midi_in.clear();
    block.midi_in.add(midi::MidiEvent::note_off(0, 72));
    block.run(*fixture.proc);
    REQUIRE(wait_for_condition([&] {
        return fixture.proc->stream_stats().active_sources == 1 &&
               fixture.proc->stream_stats().sources_retired == 1 &&
               PulpSamplerTestAccess::physical_stream_source_count(
                   *fixture.proc) == 3;
    }));
    for (const auto token : first_tokens) {
        REQUIRE_FALSE(PulpSamplerTestAccess::service_contains_source(
            *fixture.proc, token));
    }
}

TEST_CASE("PulpSampler rejects a third streamed mip bundle until a slot retires",
          "[sampler][mip][stream][integration][capacity]") {
    TempSamplerWav first("mip_capacity_a", 24000, 0.75f, 44100);
    TempSamplerMipSidecar first_sidecar(first);
    TempSamplerWav second("mip_capacity_b", 24000, 0.5f, 44100);
    TempSamplerMipSidecar second_sidecar(second);
    TempSamplerWav third("mip_capacity_c", 24000, 0.25f, 44100);
    TempSamplerMipSidecar third_sidecar(third);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerRelease, 0.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(first.path));

    SamplerProcessBlock block;
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    REQUIRE(fixture.proc->load_sample_file(second.path));
    const auto second_generation =
        PulpSamplerTestAccess::published_selection_generation(*fixture.proc);
    const auto stage_attempts =
        PulpSamplerTestAccess::file_stage_attempts(*fixture.proc);
    REQUIRE_FALSE(fixture.proc->load_sample_file(third.path));
    REQUIRE(PulpSamplerTestAccess::file_stage_attempts(*fixture.proc) ==
            stage_attempts);
    REQUIRE(PulpSamplerTestAccess::published_selection_generation(
                *fixture.proc) == second_generation);
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 6);

    block.midi_in.clear();
    block.midi_in.add(midi::MidiEvent::note_off(0, 72));
    block.run(*fixture.proc);
    REQUIRE(wait_for_condition([&] {
        return fixture.proc->stream_stats().active_sources == 1;
    }));
    REQUIRE(fixture.proc->load_sample_file(third.path));
    REQUIRE(PulpSamplerTestAccess::physical_stream_source_count(*fixture.proc) == 6);
}

TEST_CASE("PulpSampler streamed process stays allocation-free for 10000 callbacks",
          "[sampler][stream][rt]") {
    TempSamplerWav wav("rt", 24000, 0.5f);
    SamplerFixture fixture;
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 5.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block(64);
    const auto note_on = midi::MidiEvent::note_on(0, 60, 127);
    block.midi_in.add(note_on);
    block.run(*fixture.proc);
    block.midi_in.clear();

    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback) {
        block.midi_in.clear();
        if ((callback % 512) == 0) block.midi_in.add(note_on);
        block.run(*fixture.proc);
    }
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("PulpSampler streamed mip process stays allocation-free for 10000 callbacks",
          "[sampler][mip][stream][integration][rt]") {
    TempSamplerWav wav("mip_rt", 1400000, 0.5f, 44100);
    TempSamplerMipSidecar sidecar(wav);
    SamplerFixture fixture(64);
    fixture.store.set_value(kSamplerAttack, 0.0f);
    fixture.store.set_value(kSamplerDecay, 0.0f);
    fixture.store.set_value(kSamplerSustain, 100.0f);
    fixture.store.set_value(kSamplerInterpolation, 3.0f);
    REQUIRE(fixture.proc->load_sample_file(wav.path));

    SamplerProcessBlock block(64);
    block.midi_in.add(midi::MidiEvent::note_on(0, 72, 127));
    block.run(*fixture.proc);
    block.midi_in.clear();
    REQUIRE(PulpSamplerTestAccess::active_streamed_mip_octave(*fixture.proc) == 1);

    pulp::test::RtAllocationProbe probe;
    for (int callback = 0; callback < 10000; ++callback) {
        block.run(*fixture.proc);
    }
    REQUIRE(probe.allocation_count() == 0);
}
