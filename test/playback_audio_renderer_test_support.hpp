#pragma once

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/sample_rate_conversion.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/transaction.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timeline;
using namespace pulp::timebase;
using Catch::Matchers::WithinAbs;
using pulp::test::audio::tone_gain_db;
using pulp::test::audio::tone_residual_db;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSrcPassbandGainToleranceDb = 0.1;
constexpr double kSrcPassbandPurityDb = -70.0;
constexpr double kSrcStopbandRejectionDb = -60.0;
constexpr double kLinearNegativeControlAliasDb = -1.0;

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

std::shared_ptr<const audio::AudioFileData>
audio_data(std::initializer_list<std::vector<float>> channels, std::uint32_t sample_rate = 48'000) {
    auto value = std::make_shared<audio::AudioFileData>();
    value->sample_rate = sample_rate;
    value->channels.assign(channels.begin(), channels.end());
    return value;
}

std::shared_ptr<const CompiledTempoMap> map_120() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

Clip absolute_media_clip(std::uint64_t id, std::int64_t start, std::uint64_t duration,
                         std::uint64_t asset_id, std::uint64_t source_start,
                         std::uint64_t source_count, ClipPlaybackProperties playback = {}) {
    return take(Clip::create_absolute(
        {id}, {start}, duration, {48'000, 1},
        MediaRef{{asset_id}, {static_cast<std::int64_t>(source_start)}, source_count}, playback));
}

Clip musical_media_clip(std::uint64_t id, std::int64_t start, std::int64_t duration,
                        std::uint64_t asset_id, std::uint64_t source_count,
                        ClipPlaybackProperties playback = {}) {
    return take(
        Clip::create({id}, {start}, {duration}, MediaRef{{asset_id}, {0}, source_count}, playback));
}

std::shared_ptr<const Project> project_with_tracks(std::vector<Track> tracks,
                                                   std::vector<MediaAsset> assets) {
    const auto test_hash = ContentHash::from_hex(std::string(64, 'a'));
    REQUIRE(test_hash);
    for (auto& asset : assets)
        if (!asset.content_hash.valid())
            asset.content_hash = *test_hash;
    auto sequence =
        take(Sequence::create({2}, "root", std::nullopt, std::nullopt, std::move(tracks)));
    ProjectInput input;
    input.id = {1};
    input.name = "audio renderer";
    input.next_item_id = 1'000;
    input.root_sequence_id = {2};
    input.assets = std::move(assets);
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

std::shared_ptr<const DecodedAudioAssetPool> pool(std::vector<DecodedAudioAsset> assets,
                                                  AudioRendererLimits limits = {}) {
    return take(DecodedAudioAssetPool::create(std::move(assets), limits));
}

class InlineExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task, std::chrono::steady_clock::time_point) override {
        do {
            ++slice_count;
        } while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1), 1}) ==
                 CompileTaskStatus::Pending);
        return true;
    }
    std::size_t slice_count = 0;
};

class DeadlineExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> incoming,
                std::chrono::steady_clock::time_point) override {
        task = std::move(incoming);
        return task != nullptr;
    }
    CompileTaskStatus run_one(std::chrono::steady_clock::time_point deadline,
                              std::size_t work = 1) {
        REQUIRE(task);
        const auto status = task->run_slice({deadline, work});
        if (status == CompileTaskStatus::Complete)
            task.reset();
        return status;
    }
    void drain() {
        while (task)
            (void)run_one(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    }
    std::unique_ptr<CompileTask> task;
};

struct CompiledFixture {
    PlaybackProgramStore store;
    InlineExecutor executor;
    std::unique_ptr<PlaybackProgramCompiler> compiler;

    CompiledFixture(std::shared_ptr<const Project> project,
                    std::shared_ptr<const CompiledTempoMap> map,
                    std::shared_ptr<const DecodedAudioAssetPool> assets,
                    AudioRendererLimits limits = {}) {
        compiler = std::make_unique<PlaybackProgramCompiler>(store, executor,
                                                             std::chrono::microseconds(0));
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = std::move(map);
        request.document_revision = 1;
        request.dirty.all = true;
        request.audio_assets = std::move(assets);
        request.audio_limits = limits;
        REQUIRE(compiler->submit(std::move(request)));
        INFO("compile error code " << static_cast<int>(compiler->status().last_error.code)
                                   << " item " << compiler->status().last_error.item.value
                                   << " audio detail "
                                   << static_cast<int>(compiler->status().last_error.audio_detail));
        REQUIRE(store.has_value());
    }
};

struct Output {
    explicit Output(std::size_t channels, std::size_t frames)
        : storage(channels, std::vector<float>(frames, 99.0f)), pointers(channels) {
        for (std::size_t i = 0; i < channels; ++i)
            pointers[i] = storage[i].data();
    }
    audio::BufferView<float> view() {
        return {pointers.data(), pointers.size(), storage.front().size()};
    }
    std::vector<std::vector<float>> storage;
    std::vector<float*> pointers;
};

TransportSnapshot snapshot(const PlaybackProgram& program, std::uint32_t frames,
                           std::int64_t sample_start = 0) {
    TransportSnapshot value;
    value.tempo_map = &program.tempo_map();
    value.sample_rate = program.tempo_map().sample_rate();
    value.frame_count = frames;
    value.is_playing = true;
    value.range_count = 1;
    value.ranges[0].frame_count = frames;
    value.ranges[0].timeline_sample_start = {sample_start};
    return value;
}

std::vector<double> render_resampled_tone(std::uint32_t source_rate, std::uint32_t target_rate,
                                          double frequency_hz, double amplitude,
                                          std::size_t source_frames, std::size_t trim_frames) {
    std::vector<float> source(source_frames);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency_hz * static_cast<double>(frame) /
                                 static_cast<double>(source_rate)));
    const auto data = audio_data({source}, source_rate);
    auto clip = take(Clip::create_absolute({100}, {0}, source_frames, {source_rate, 1},
                                           MediaRef{{3}, {0}, source_frames}));
    auto track = take(Track::create({10}, "sample-rate conversion", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", source_frames, {source_rate, 1}}});
    const std::array points{TempoPoint{{0}, 120.0}};
    auto map = shared_compiled_tempo_map(points, RationalRate{target_rate, 1});
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();
    const auto target_frames = static_cast<std::size_t>(
        std::llround(static_cast<long double>(source_frames) * target_rate / source_rate));
    REQUIRE(target_frames <= std::numeric_limits<std::uint32_t>::max());
    REQUIRE(trim_frames * 2u < target_frames);
    Output output(1, target_frames);
    REQUIRE(ArrangementAudioRenderer::process(
                *program, snapshot(*program, static_cast<std::uint32_t>(target_frames)),
                output.view()) == AudioRenderStatus::Rendered);
    return {output.storage[0].begin() + static_cast<std::ptrdiff_t>(trim_frames),
            output.storage[0].end() - static_cast<std::ptrdiff_t>(trim_frames)};
}

std::vector<std::uint8_t> mono_pcm16_wav(std::span<const std::int16_t> samples) {
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2u);
    std::vector<std::uint8_t> bytes(44u + data_bytes, 0);
    auto put16 = [&](std::size_t offset, std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
    };
    auto put32 = [&](std::size_t offset, std::uint32_t value) {
        put16(offset, static_cast<std::uint16_t>(value));
        put16(offset + 2, static_cast<std::uint16_t>(value >> 16u));
    };
    std::memcpy(bytes.data(), "RIFF", 4);
    put32(4, 36u + data_bytes);
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, 1);
    put32(24, 48'000);
    put32(28, 96'000);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(bytes.data() + 36, "data", 4);
    put32(40, data_bytes);
    for (std::size_t index = 0; index < samples.size(); ++index)
        put16(44 + index * 2, static_cast<std::uint16_t>(samples[index]));
    return bytes;
}

} // namespace

static_assert(ArrangementAudioRenderer::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs);
static_assert(!ArrangementAudioRenderer::carries_mutable_state);
