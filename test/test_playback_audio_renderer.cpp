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

TEST_CASE("audio renderer decodes WAV bytes into the immutable asset pool") {
    const std::array<std::int16_t, 4> samples{0, 16'384, -16'384, 32'767};
    const auto bytes = mono_pcm16_wav(samples);
    auto decoded = DecodedAudioAssetPool::decode_wav({3}, bytes);
    REQUIRE(decoded);
    auto assets = DecodedAudioAssetPool::create({std::move(decoded).value()});
    REQUIRE(assets);
    REQUIRE(assets.value()->find({3}));
    REQUIRE(assets.value()->find({3})->audio->num_frames() == 4);
    REQUIRE_THAT(assets.value()->find({3})->audio->channels[0][1], WithinAbs(0.5f, 1e-5f));
}

TEST_CASE("audio renderer honors source range gain fades and varied block boundaries") {
    std::vector<float> source(2'050, 1.0f);
    source[0] = 99.0f;
    source[1] = 99.0f;
    const auto data = audio_data({source});
    auto clip =
        absolute_media_clip(100, 0, 2'048, 3, 2, 2'048,
                            {.gain_linear = 0.5f, .fade_in_duration = 4, .fade_out_duration = 4});
    auto track = take(Track::create({10}, "one", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 2'050, {48'000, 1}}});
    const auto map = map_120();
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();

    for (const auto frames : {1u, 64u, 257u, 1024u}) {
        Output output(1, frames);
        const auto state = snapshot(*program, frames);
        REQUIRE(ArrangementAudioRenderer::process(*program, state, output.view()) ==
                AudioRenderStatus::Rendered);
        REQUIRE_THAT(output.storage[0][0], WithinAbs(0.0f, 1e-7f));
        if (frames > 4)
            REQUIRE_THAT(output.storage[0][4], WithinAbs(0.5f, 1e-7f));
    }

    Output tail(1, 4);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 4, 2'044),
                                              tail.view()) == AudioRenderStatus::Rendered);
    REQUIRE_THAT(tail.storage[0][0], WithinAbs(0.375f, 1e-7f));
    REQUIRE_THAT(tail.storage[0][3], WithinAbs(0.0f, 1e-7f));
}

TEST_CASE("tempo mapped musical audio placement resolves to exact samples") {
    const auto data = audio_data({std::vector<float>(128, 0.25f)});
    auto clip = musical_media_clip(100, kTicksPerQuarter, kTicksPerQuarter, 3, 128);
    auto track = take(Track::create({10}, "musical", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 128, {48'000, 1}}});
    const auto map = map_120();
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();

    Output before(1, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64, 23'936),
                                              before.view()) == AudioRenderStatus::Rendered);
    REQUIRE(before.storage[0][63] == 0.0f);
    Output at_clip(1, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64, 24'000),
                                              at_clip.view()) == AudioRenderStatus::Rendered);
    REQUIRE_THAT(at_clip.storage[0][0], WithinAbs(0.25f, 1e-7f));
}

TEST_CASE("audio renderer maps host 60 BPM frames into a 120 BPM document") {
    constexpr auto mapped_duration = kTicksPerQuarter / 480;
    constexpr auto mapped_half = mapped_duration / 2;
    std::vector<float> ramp(50);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(frame + 1);
    const auto data = audio_data({ramp});
    auto clip = musical_media_clip(
        100, kTicksPerQuarter, mapped_duration, 3, ramp.size(),
        {.gain_linear = 1.0f, .fade_in_duration = mapped_half, .fade_out_duration = 0});
    auto track = take(Track::create({10}, "host mapped", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", ramp.size(), {48'000, 1}}});
    const auto map = map_120();
    const auto assets = pool({{3, data}});
    CompiledFixture compiled(project, map, assets);
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 100, 48'000);
    mapped.ranges[0].timeline_tick_start = {kTicksPerQuarter};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter + mapped_duration};
    mapped.ranges[0].host_beat_mapping = true;
    Output stretched(1, 100);
    AudioRenderStatus status = AudioRenderStatus::InvalidProgram;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        status = ArrangementAudioRenderer::process(*program, mapped, stretched.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(status == AudioRenderStatus::Rendered);
    REQUIRE(allocations == 0);
    REQUIRE_THAT(stretched.storage[0][0], WithinAbs(0.0f, 1e-7f));
    REQUIRE_THAT(stretched.storage[0][25], WithinAbs(6.75f, 1e-6f));
    REQUIRE_THAT(stretched.storage[0][50], WithinAbs(26.0f, 1e-6f));
    REQUIRE_THAT(stretched.storage[0][99], WithinAbs(50.0f, 1e-6f));

    mapped.range_count = 2;
    mapped.ranges[0].frame_count = 50;
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter + mapped_half};
    mapped.ranges[1] = mapped.ranges[0];
    mapped.ranges[1].sample_offset = 50;
    mapped.ranges[1].discontinuity = true;
    Output looped(1, 100);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, looped.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(looped.storage[0][49], WithinAbs(24.99f, 1e-5f));
    REQUIRE_THAT(looped.storage[0][50], WithinAbs(0.0f, 1e-7f));
    REQUIRE_THAT(looped.storage[0][75], WithinAbs(6.75f, 1e-6f));
}

TEST_CASE("host-mapped audio follows a document tempo ramp instead of endpoint interpolation") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 180.0},
    };
    const auto map = shared_compiled_tempo_map(points, RationalRate{48'000, 1});
    const auto document_frames = static_cast<std::uint64_t>(
        map->ticks_to_samples({2 * kTicksPerQuarter}).value);
    REQUIRE(document_frames > 0);
    std::vector<float> ramp(document_frames);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(
            static_cast<double>(frame) / static_cast<double>(document_frames));

    const auto data = audio_data({ramp});
    auto clip =
        musical_media_clip(100, 0, 2 * kTicksPerQuarter, 3, document_frames);
    auto track = take(Track::create({10}, "tempo ramp", {clip}));
    auto project = project_with_tracks(
        {track}, {{3, "tempo-ramp.wav", document_frames, {48'000, 1}}});
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 4);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {2 * kTicksPerQuarter};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 4);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);

    const auto midpoint_sample = map->ticks_to_samples({kTicksPerQuarter}).value;
    const auto expected =
        static_cast<float>(static_cast<double>(midpoint_sample) /
                           static_cast<double>(document_frames));
    REQUIRE_THAT(output.storage[0][2], WithinAbs(expected, 2e-6f));
    REQUIRE(std::abs(output.storage[0][2] - 0.5f) > 0.05f);
}

TEST_CASE("host beat mapping keeps absolute audio on the host sample clock") {
    std::vector<float> absolute_ramp(64);
    for (std::size_t frame = 0; frame < absolute_ramp.size(); ++frame)
        absolute_ramp[frame] = static_cast<float>(frame);
    auto absolute = absolute_media_clip(100, 0, 50, 3, 0, 50);
    auto absolute_track = take(Track::create({10}, "absolute", {absolute}));
    auto project = project_with_tracks({absolute_track}, {{3, "absolute", 64, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({absolute_ramp})}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 50);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter / 960};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 50);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(output.storage[0][10], WithinAbs(10.0f, 1.0e-6f));
    REQUIRE_THAT(output.storage[0][20], WithinAbs(20.0f, 1.0e-6f));
}

TEST_CASE("host-tempo audio decimation rejects aliases without muting the passband path") {
    std::vector<float> source(256);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = frame % 2 == 0 ? 1.0f : -1.0f;
    auto clip = musical_media_clip(100, 0, kTicksPerQuarter / 120, 3, 200);
    auto track = take(Track::create({10}, "host decimation", {clip}));
    auto project = project_with_tracks({track}, {{3, "nyquist", source.size(), {48'000, 1}}});
    const auto map = map_120();
    const auto assets = pool({{3, audio_data({source})}});
    CompiledFixture compiled(project, map, assets);
    auto program = compiled.store.read();
    REQUIRE(program->find_track({10})->audio_program()->clips()[0].host_rate_converter);

    auto render = [&](std::int64_t tick_end) {
        auto mapped = snapshot(*program, 100);
        mapped.ranges[0].timeline_tick_start = {0};
        mapped.ranges[0].timeline_tick_end = {tick_end};
        mapped.ranges[0].host_beat_mapping = true;
        Output output(1, 100);
        REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
                AudioRenderStatus::Rendered);
        double squared = 0.0;
        for (std::size_t frame = 32; frame < 68; ++frame)
            squared += static_cast<double>(output.storage[0][frame]) *
                       static_cast<double>(output.storage[0][frame]);
        return std::sqrt(squared / 36.0);
    };

    const auto passband_reference_rms = render(kTicksPerQuarter / 240);
    const auto decimated_alias_rms = render(kTicksPerQuarter / 120);
    CAPTURE(passband_reference_rms, decimated_alias_rms);
    REQUIRE(passband_reference_rms >= 0.8);
    REQUIRE(decimated_alias_rms <= 0.05);
}

TEST_CASE("host-tempo reconstruction uses the effective rate when slowed audio stops decimating") {
    std::vector<float> source(256);
    for (std::size_t frame = 0; frame < source.size(); ++frame)
        source[frame] = frame % 2 == 0 ? 1.0f : -1.0f;
    auto clip = musical_media_clip(100, 0, kTicksPerQuarter / 240, 3, 200);
    auto track = take(Track::create({10}, "slowed downsample", {clip}));
    auto project = project_with_tracks({track}, {{3, "96k-nyquist", source.size(), {96'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({source}, 96'000)}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 100);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter / 480};
    mapped.ranges[0].host_beat_mapping = true;
    Output output(1, 100);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);
    double squared = 0.0;
    for (std::size_t frame = 16; frame < 84; ++frame)
        squared += static_cast<double>(output.storage[0][frame]) *
                   static_cast<double>(output.storage[0][frame]);
    REQUIRE(std::sqrt(squared / 68.0) >= 0.8);
}

TEST_CASE("one bounded compiler pass publishes note and audio payloads for a mixed track") {
    const auto map = map_120();
    auto notes = take(NoteContent::create({{{101}, {0}, {kTicksPerQuarter / 4}, 0x8000, 64, 1}}));
    auto note_clip = take(Clip::create({100}, {0}, {kTicksPerQuarter}, std::move(notes)));
    auto audio_clip =
        musical_media_clip(102, 2 * kTicksPerQuarter, kTicksPerQuarter, 3, 128,
                           {.gain_linear = 0.5f, .fade_in_duration = 2, .fade_out_duration = 3});
    auto track = take(Track::create({10}, "mixed", {note_clip, audio_clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 128, {48'000, 1}}});
    CompiledFixture compiled(project, map,
                             pool({{3, audio_data({std::vector<float>(128, 1.0f)})}}));

    const auto program = compiled.store.read();
    const auto* compiled_track = program->find_track({10});
    REQUIRE(compiled_track != nullptr);
    REQUIRE(compiled_track->ordered_clip_ids().size() == 2);
    REQUIRE(compiled_track->arrangement_note_events().size() == 2);
    REQUIRE(compiled_track->arrangement_note_events()[0].note_id == ItemId{101});
    REQUIRE(compiled_track->audio_program() != nullptr);
    REQUIRE(compiled_track->audio_program()->clips().size() == 1);
    REQUIRE(compiled_track->audio_program()->clips()[0].id == ItemId{102});
    REQUIRE(compiled_track->audio_program()->clips()[0].gain_linear == 0.5f);
}

TEST_CASE("active take comp lowers to a derived artifact and renders exact selections") {
    std::vector<float> source(32);
    for (std::size_t index = 0; index < source.size(); ++index)
        source[index] = static_cast<float>(index);
    const auto first =
        take(Take::create({20}, MediaRef{{3}, {0}, 16}, {0}, RationalRate{48'000, 1}));
    const auto second =
        take(Take::create({21}, MediaRef{{3}, {16}, 16}, {0}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({30}, "active", {first, second},
                                      {{.take_id = {20}, .range = {{0}, 2, {48'000, 1}}},
                                       {.take_id = {21}, .range = {{2}, 2, {48'000, 1}}},
                                       {.take_id = {20}, .range = {{4}, 2, {48'000, 1}}}}));
    auto track = take(Track::create(TrackInput{.id = {10},
                                               .name = "comp",
                                               .clips = {absolute_media_clip(100, 0, 6, 3, 0, 6)},
                                               .take_lanes = {lane},
                                               .active_take_lane_id = {30}}));
    auto project = project_with_tracks({track}, {{3, "takes", 32, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({source})}}));
    auto program = compiled.store.read();
    const auto regions = program->find_track({10})->audio_program()->clips();
    REQUIRE(program->find_track({10})->ordered_clip_ids().empty());
    REQUIRE(regions.size() == 3);
    REQUIRE(regions[0].source_kind == AudioClipRendererProgram::SourceKind::TakeCompSegment);
    REQUIRE(regions[0].source_ordinal == 1);
    REQUIRE(regions[1].source_ordinal == 2);
    REQUIRE(regions[2].source_ordinal == 3);
    REQUIRE(regions[0].id == ItemId{20});
    REQUIRE(regions[1].id == ItemId{21});
    REQUIRE(regions[2].id == ItemId{20});

    Output output(1, 6);
    AudioRenderStatus status = AudioRenderStatus::InvalidProgram;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        status = ArrangementAudioRenderer::process(*program, snapshot(*program, 6), output.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(status == AudioRenderStatus::Rendered);
    REQUIRE(allocations == 0);
    REQUIRE(output.storage[0] == std::vector<float>{0, 1, 18, 19, 4, 5});

    auto mapped = snapshot(*program, 6);
    mapped.ranges[0].timeline_tick_start = {0};
    mapped.ranges[0].timeline_tick_end = {kTicksPerQuarter / 8'000};
    mapped.ranges[0].host_beat_mapping = true;
    Output host_tempo_output(1, 6);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, host_tempo_output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(host_tempo_output.storage[0] == std::vector<float>{0, 1, 18, 19, 4, 5});
}

TEST_CASE("inactive take comp is document data but not a playback source") {
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, 4}, {0}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({30}, "inactive", {recorded},
                                      {{.take_id = {20}, .range = {{0}, 4, {48'000, 1}}}}));
    auto track = take(Track::create(TrackInput{.id = {10},
                                               .name = "comp",
                                               .clips = {absolute_media_clip(100, 0, 4, 3, 0, 4)},
                                               .take_lanes = {lane}}));
    auto project = project_with_tracks({track}, {{3, "take", 4, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(),
                             pool({{3, audio_data({{1.0f, 2.0f, 3.0f, 4.0f}})}}));
    const auto* compiled_track = compiled.store.read()->find_track({10});
    REQUIRE(compiled_track->ordered_clip_ids().size() == 1);
    REQUIRE(compiled_track->ordered_clip_ids()[0] == ItemId{100});
    REQUIRE(compiled_track->audio_program()->clips()[0].source_kind ==
            AudioClipRendererProgram::SourceKind::ArrangementClip);
}

TEST_CASE("take comp compilation validates asset rate and whole program capacity") {
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, 4}, {0}, RationalRate{44'100, 1}));
    auto lane = take(TakeLane::create({30}, "rate", {recorded},
                                      {{.take_id = {20}, .range = {{0}, 4, {44'100, 1}}}}));
    auto track = take(Track::create(
        TrackInput{.id = {10}, .name = "comp", .take_lanes = {lane}, .active_take_lane_id = {30}}));
    auto project = project_with_tracks({track}, {{3, "take", 4, {48'000, 1}}});
    auto compiled = compile_take_comp_segment_program(
        lane, 0, *project, *map_120(), *pool({{3, audio_data({{1.0f, 2.0f, 3.0f, 4.0f}})}}), {});
    REQUIRE_FALSE(compiled);
    REQUIRE(compiled.error().code == AudioRendererErrorCode::UnsupportedSampleRate);

    const auto matching =
        take(Take::create({20}, MediaRef{{3}, {0}, 4}, {0}, RationalRate{48'000, 1}));
    lane = take(TakeLane::create({30}, "capacity", {matching},
                                 {{.take_id = {20}, .range = {{0}, 2, {48'000, 1}}},
                                  {.take_id = {20}, .range = {{2}, 2, {48'000, 1}}}}));
    track = take(Track::create(
        TrackInput{.id = {10}, .name = "comp", .take_lanes = {lane}, .active_take_lane_id = {30}}));
    auto earlier = take(Track::create({9}, "earlier", {absolute_media_clip(100, 0, 1, 3, 0, 1)}));
    project = project_with_tracks({earlier, track}, {{3, "take", 4, {48'000, 1}}});
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, audio_data({{1.0f, 2.0f, 3.0f, 4.0f}})}});
    request.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(store.has_value());
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);
}

TEST_CASE("take comp boundary projection is exact beyond floating point integer precision") {
    constexpr std::int64_t start = (std::int64_t{1} << 53u) + 1;
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, 1}, {start}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({30}, "large position", {recorded},
                                      {{.take_id = {20}, .range = {{start}, 1, {48'000, 1}}}}));
    auto track = take(Track::create(
        TrackInput{.id = {10}, .name = "comp", .take_lanes = {lane}, .active_take_lane_id = {30}}));
    auto project = project_with_tracks({track}, {{3, "take", 1, {48'000, 1}}});
    const std::array points{TempoPoint{{0}, 120.0}};
    auto doubled_rate = shared_compiled_tempo_map(points, RationalRate{96'000, 1});
    auto compiled = take(compile_take_comp_segment_program(lane, 0, *project, *doubled_rate,
                                                           *pool({{3, audio_data({{1.0f}})}}), {}));
    REQUIRE(compiled.timeline_start == start * 2);
    REQUIRE(compiled.timeline_frame_count == 2);
}

TEST_CASE("playback property commands dirty and rebuild audible clip gain") {
    auto asset_pool = pool({{3, audio_data({std::vector<float>(8, 1.0f)})}});
    auto map = map_120();
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8, {.gain_linear = 0.5f});
    auto track = take(Track::create({10}, "gain", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 8, {48'000, 1}}});
    CompiledFixture compiled(project, map, asset_pool);

    Output before(1, 1);
    auto first_program = compiled.store.read();
    REQUIRE(ArrangementAudioRenderer::process(*first_program, snapshot(*first_program, 1),
                                              before.view()) == AudioRenderStatus::Rendered);
    REQUIRE(before.storage[0][0] == 0.5f);

    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back(
        {{{1}, 1},
         SetClipPlaybackProperties{
             {2}, {10}, {100}, {.gain_linear = 0.5f}, {.gain_linear = 0.25f}}});
    auto changed = reduce_transaction(*project, transaction);
    REQUIRE(changed);
    REQUIRE(changed->dirty.items().size() == 1);
    REQUIRE(changed->dirty.items()[0].owner_track == ItemId{10});
    REQUIRE(changed->dirty.items()[0].flags == DirtyFlags::Content);

    ProgramCompileRequest request;
    request.project = std::make_shared<const Project>(changed->project);
    request.sequence_id = {2};
    request.tempo_map = map;
    request.document_revision = 2;
    request.dirty = {false, {{10}}};
    request.audio_assets = asset_pool;
    REQUIRE(compiled.compiler->submit(std::move(request)));
    auto second_program = compiled.store.read();
    Output after(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*second_program, snapshot(*second_program, 1),
                                              after.view()) == AudioRenderStatus::Rendered);
    REQUIRE(after.storage[0][0] == 0.25f);
}

TEST_CASE("audio track linking remains incremental under one-work-unit slices") {
    constexpr std::size_t clip_count = 256;
    std::vector<Clip> clips;
    clips.reserve(clip_count);
    for (std::size_t index = 0; index < clip_count; ++index)
        clips.push_back(musical_media_clip(100 + index,
                                           static_cast<std::int64_t>(index * kTicksPerQuarter),
                                           kTicksPerQuarter, 3, 1));
    auto track = take(Track::create({10}, "many", std::move(clips)));
    auto project = project_with_tracks({track}, {{3, "sample", 1, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, audio_data({{1.0f}})}}));
    REQUIRE(compiled.store.read()->find_track({10})->audio_program()->clips().size() == clip_count);
    REQUIRE(compiled.executor.slice_count > clip_count * 4);
}

TEST_CASE("take comp lowering remains incremental under one-work-unit slices") {
    constexpr std::size_t segment_count = 256;
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, segment_count}, {0}, RationalRate{48'000, 1}));
    std::vector<TakeCompSegment> segments;
    segments.reserve(segment_count);
    for (std::size_t index = 0; index < segment_count; ++index)
        segments.push_back(
            {.take_id = {20}, .range = {{static_cast<std::int64_t>(index)}, 1, {48'000, 1}}});
    auto lane = take(TakeLane::create({30}, "many selections", {recorded}, std::move(segments)));
    auto track = take(Track::create(
        TrackInput{.id = {10}, .name = "comp", .take_lanes = {lane}, .active_take_lane_id = {30}}));
    auto project = project_with_tracks({track}, {{3, "take", segment_count, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(),
                             pool({{3, audio_data({std::vector<float>(segment_count, 1.0f)})}}));
    REQUIRE(compiled.store.read()->find_track({10})->audio_program()->clips().size() ==
            segment_count);
    REQUIRE(compiled.executor.slice_count > segment_count);
}

TEST_CASE("active take comp compile cost is independent of the hidden arrangement") {
    constexpr std::size_t hidden_clip_count = 512;
    const auto recorded =
        take(Take::create({20}, MediaRef{{3}, {0}, 1}, {0}, RationalRate{48'000, 1}));
    auto lane = take(TakeLane::create({30}, "active", {recorded},
                                      {{.take_id = {20}, .range = {{0}, 1, {48'000, 1}}}}));
    auto make_project = [&](std::size_t clip_count) {
        std::vector<Clip> clips;
        clips.reserve(clip_count);
        for (std::size_t index = 0; index < clip_count; ++index)
            clips.push_back(
                absolute_media_clip(100 + index, static_cast<std::int64_t>(index), 1, 3, 0, 1));
        auto track = take(Track::create(TrackInput{.id = {10},
                                                   .name = "comp",
                                                   .clips = std::move(clips),
                                                   .take_lanes = {lane},
                                                   .active_take_lane_id = {30}}));
        return project_with_tracks({track}, {{3, "take", 1, {48'000, 1}}});
    };
    const auto asset_pool = pool({{3, audio_data({{1.0f}})}});
    const auto tempo_map = map_120();
    auto compile_bytes = [&](std::shared_ptr<const Project> project) {
        PlaybackProgramStore store;
        InlineExecutor executor;
        PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = tempo_map;
        request.document_revision = 1;
        request.dirty.all = true;
        request.audio_assets = asset_pool;
        test::RtAllocationProbe probe;
        REQUIRE(compiler.submit(std::move(request)));
        REQUIRE(store.has_value());
        return probe.allocated_bytes();
    };
    const auto baseline_bytes = compile_bytes(make_project(0));
    const auto hidden_arrangement_bytes = compile_bytes(make_project(hidden_clip_count));
    REQUIRE(hidden_arrangement_bytes == baseline_bytes);
}

TEST_CASE("expired compile deadlines stop an in-progress audio link pass") {
    constexpr std::size_t clip_count = 64;
    std::vector<Clip> clips;
    for (std::size_t index = 0; index < clip_count; ++index)
        clips.push_back(musical_media_clip(100 + index,
                                           static_cast<std::int64_t>(index * kTicksPerQuarter),
                                           kTicksPerQuarter, 3, 1));
    auto track = take(Track::create({10}, "deadline", std::move(clips)));
    auto project = project_with_tracks({track}, {{3, "sample", 1, {48'000, 1}}});
    PlaybackProgramStore store;
    DeadlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, audio_data({{1.0f}})}});
    REQUIRE(compiler.submit(std::move(request)));
    for (std::size_t index = 0; index <= clip_count; ++index)
        REQUIRE(executor.run_one(std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
                CompileTaskStatus::Pending);
    REQUIRE_FALSE(store.has_value());
    REQUIRE(executor.run_one(std::chrono::steady_clock::now() - std::chrono::milliseconds(1),
                             1'000'000) == CompileTaskStatus::Pending);
    REQUIRE_FALSE(store.has_value());
    executor.drain();
    REQUIRE(store.has_value());
}

TEST_CASE("audio renderer projects absolute anchors and source rates on reprepare") {
    const auto data = audio_data({std::vector<float>(441, 0.75f)}, 44'100);
    auto clip =
        take(Clip::create_absolute({100}, {441}, 441, {44'100, 1}, MediaRef{{3}, {0}, 441}));
    auto track = take(Track::create({10}, "absolute rate", {clip}));
    auto project = project_with_tracks({track}, {{3, "44k", 441, {44'100, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    REQUIRE(program->find_track({10})->audio_program()->clips()[0].timeline_start == 480);
    REQUIRE(program->find_track({10})->audio_program()->clips()[0].timeline_frame_count == 480);

    Output before(1, 1);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 1, 479),
                                              before.view()) == AudioRenderStatus::Rendered);
    REQUIRE(before.storage[0][0] == 0.0f);
    Output converted(1, 480);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 480, 480),
                                              converted.view()) == AudioRenderStatus::Rendered);
    REQUIRE_THAT(converted.storage[0].front(), WithinAbs(0.75f, 1e-7f));
    REQUIRE_THAT(converted.storage[0].back(), WithinAbs(0.75f, 5e-7f));

    const std::array points{TempoPoint{{0}, 120.0}};
    auto map_96 = shared_compiled_tempo_map(points, RationalRate{96'000, 1});
    CompiledFixture reprepared(project, map_96, pool({{3, data}}));
    auto next = reprepared.store.read();
    REQUIRE(next->find_track({10})->audio_program()->clips()[0].timeline_start == 960);
    REQUIRE(next->find_track({10})->audio_program()->clips()[0].timeline_frame_count == 960);
}

TEST_CASE("audio renderer resamples a 44k ramp identically across split blocks") {
    std::vector<float> ramp(441);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<float>(frame);
    const auto data = audio_data({ramp}, 44'100);
    auto clip = take(Clip::create_absolute({100}, {0}, 441, {44'100, 1}, MediaRef{{3}, {0}, 441}));
    auto track = take(Track::create({10}, "ramp", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", 441, {44'100, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();

    Output whole(1, 480);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 480), whole.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(whole.storage[0][0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(whole.storage[0][240], WithinAbs(220.5f, 1e-3f));
    REQUIRE_THAT(whole.storage[0][479], WithinAbs(440.0f, 0.05f));

    Output first(1, 137);
    Output second(1, 343);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 137), first.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 343, 137),
                                              second.view()) == AudioRenderStatus::Rendered);
    std::vector<float> split = first.storage[0];
    split.insert(split.end(), second.storage[0].begin(), second.storage[0].end());
    REQUIRE(split == whole.storage[0]);
}

TEST_CASE("audio renderer sample-rate conversion preserves passband and rejects aliases") {
    constexpr std::uint32_t source_rate = 96'000;
    constexpr std::uint32_t target_rate = 48'000;
    constexpr std::size_t source_frames = source_rate;
    constexpr std::size_t trim_frames = 2'048;
    constexpr double amplitude = 0.5;
    constexpr double passband_hz = 20'000.0;
    constexpr double stopband_hz = 30'000.0;
    constexpr double folded_stopband_hz = 18'000.0;

    const auto passband = render_resampled_tone(source_rate, target_rate, passband_hz, amplitude,
                                                source_frames, trim_frames);
    const auto passband_gain = tone_gain_db(passband, passband_hz / target_rate, amplitude);
    CAPTURE(passband_gain);
    REQUIRE(std::abs(passband_gain) <= kSrcPassbandGainToleranceDb);

    const auto stopband = render_resampled_tone(source_rate, target_rate, stopband_hz, amplitude,
                                                source_frames, trim_frames);
    const auto alias_gain = tone_gain_db(stopband, folded_stopband_hz / target_rate, amplitude);
    CAPTURE(alias_gain);
    REQUIRE(alias_gain <= kSrcStopbandRejectionDb);

    std::vector<double> linear_negative_control;
    linear_negative_control.reserve(stopband.size());
    for (std::size_t output_frame = trim_frames; output_frame < source_frames / 2u - trim_frames;
         ++output_frame) {
        const auto source_frame = output_frame * 2u;
        linear_negative_control.push_back(
            amplitude * std::sin(2.0 * kPi * stopband_hz * source_frame / source_rate));
    }
    const auto linear_alias_gain =
        tone_gain_db(linear_negative_control, folded_stopband_hz / target_rate, amplitude);
    CAPTURE(linear_alias_gain);
    REQUIRE(linear_alias_gain >= kLinearNegativeControlAliasDb);

    constexpr std::uint32_t upsample_source_rate = 44'100;
    constexpr double upsample_passband_hz = 18'000.0;
    const auto upsampled =
        render_resampled_tone(upsample_source_rate, target_rate, upsample_passband_hz, amplitude,
                              upsample_source_rate, trim_frames);
    const auto upsample_gain =
        tone_gain_db(upsampled, upsample_passband_hz / target_rate, amplitude);
    const auto upsample_residual = tone_residual_db(upsampled, upsample_passband_hz / target_rate);
    CAPTURE(upsample_gain, upsample_residual);
    REQUIRE(std::abs(upsample_gain) <= kSrcPassbandGainToleranceDb);
    REQUIRE(upsample_residual <= kSrcPassbandPurityDb);
}

TEST_CASE("absolute sample-rate projection preserves adjacent clip boundaries") {
    const auto data = audio_data({{1.0f, 2.0f}}, 44'100);
    auto first = take(Clip::create_absolute({100}, {0}, 1, {44'100, 1}, MediaRef{{3}, {0}, 1}));
    auto second = take(Clip::create_absolute({101}, {1}, 1, {44'100, 1}, MediaRef{{3}, {1}, 1}));
    auto track = take(Track::create({10}, "adjacent", {first, second}));
    auto project = project_with_tracks({track}, {{3, "44k", 2, {44'100, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    const auto clips = program->find_track({10})->audio_program()->clips();
    REQUIRE(clips.size() == 2);
    REQUIRE(clips[0].sample_rate_converter);
    REQUIRE(clips[0].sample_rate_converter == clips[1].sample_rate_converter);
    REQUIRE(clips[0].timeline_end() == clips[1].timeline_start);

    Output output(1, 2);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 2), output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(output.storage[0] == std::vector<float>{1.0f, 2.0f});
}

TEST_CASE("audio renderer requires exact transport and program tempo map identity") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto track = take(Track::create({10}, "identity", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 8, {48'000, 1}}});
    const auto baseline = map_120();
    const std::array foreign_points{TempoPoint{{0}, 90.0}, TempoPoint{{kTicksPerQuarter}, 140.0}};
    const auto foreign = shared_compiled_tempo_map(foreign_points, RationalRate{48'000, 1});
    CompiledFixture compiled(project, baseline, pool({{3, data}}));
    auto program = compiled.store.read();
    REQUIRE_FALSE(program->find_track({10})->audio_program()->clips()[0].sample_rate_converter);
    auto mismatched = snapshot(*program, 8);
    mismatched.tempo_map = foreign.get();
    Output rejected(1, 8);

    REQUIRE(ArrangementAudioRenderer::process(*program, mismatched, rejected.view()) ==
            AudioRenderStatus::InvalidTransport);
    REQUIRE(rejected.storage[0] == std::vector<float>(8, 0.0f));

    CompiledFixture reprepared(project, foreign, pool({{3, data}}));
    auto paired_program = reprepared.store.read();
    Output accepted(1, 8);
    REQUIRE(ArrangementAudioRenderer::process(*paired_program, snapshot(*paired_program, 8),
                                              accepted.view()) == AudioRenderStatus::Rendered);
    REQUIRE(accepted.storage[0] == std::vector<float>(8, 1.0f));
}

TEST_CASE("audio renderer follows transport loop splits and seeks without stale cursors") {
    std::vector<float> ramp(256);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i);
    const auto data = audio_data({ramp}, 48'000);
    auto clip = take(Clip::create_absolute({100}, {0}, 256, {48'000, 1}, MediaRef{{3}, {0}, 256}));
    auto track = take(Track::create({10}, "loop", {clip}));
    auto project = project_with_tracks({track}, {{3, "ramp", 256, {48'000, 1}}});
    const auto map = map_120();
    CompiledFixture compiled(project, map, pool({{3, data}}));
    auto program = compiled.store.read();

    MasterTransport transport;
    const auto loop_end = map->samples_to_ticks({64});
    REQUIRE(transport.prepare(*map, {.max_buffer_size = 32,
                                     .loop = {true, {0}, loop_end},
                                     .initial_position = map->samples_to_ticks({48}),
                                     .initially_playing = true}) == TransportError::None);
    TransportSnapshot state;
    REQUIRE(transport.begin_block(32, state) == TransportError::None);
    REQUIRE(state.range_count == 2);
    Output looped(1, 32);
    REQUIRE(ArrangementAudioRenderer::process(*program, state, looped.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(looped.storage[0][0] == 48.0f);
    REQUIRE(looped.storage[0][15] == 63.0f);
    REQUIRE(looped.storage[0][16] == 0.0f);
    REQUIRE(looped.storage[0][31] == 15.0f);

    REQUIRE(transport.seek(map->samples_to_ticks({32})) == TransportError::None);
    REQUIRE(transport.begin_block(16, state) == TransportError::None);
    REQUIRE(state.reset_requested);
    Output seeked(1, 16);
    REQUIRE(ArrangementAudioRenderer::process(*program, state, seeked.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(seeked.storage[0][0] == 32.0f);
}

TEST_CASE("audio renderer mixes tracks and maps mono and stereo deterministically") {
    const auto mono = audio_data({std::vector<float>(64, 1.0f)});
    const auto stereo = audio_data({std::vector<float>(64, 2.0f), std::vector<float>(64, 4.0f)});
    auto mono_track =
        take(Track::create({10}, "mono", {absolute_media_clip(100, 0, 64, 3, 0, 64)}));
    auto stereo_track =
        take(Track::create({11}, "stereo", {absolute_media_clip(101, 0, 64, 4, 0, 64)}));
    auto project = project_with_tracks(
        {stereo_track, mono_track}, {{3, "mono", 64, {48'000, 1}}, {4, "stereo", 64, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{4, stereo}, {3, mono}}));
    auto program = compiled.store.read();

    Output first(2, 64);
    Output second(2, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64), first.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64), second.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(first.storage == second.storage);
    REQUIRE(first.storage[0][0] == 3.0f);
    REQUIRE(first.storage[1][0] == 5.0f);

    Output mono_output(1, 64);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64),
                                              mono_output.view()) == AudioRenderStatus::Rendered);
    REQUIRE(mono_output.storage[0][0] == 4.0f);
}

TEST_CASE("audio renderer zero fills after source EOF and while stopped") {
    const auto data = audio_data({{1.0f, 2.0f, 3.0f, 4.0f}});
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 4);
    auto track = take(Track::create({10}, "short", {clip}));
    auto project = project_with_tracks({track}, {{3, "short", 4, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(1, 8);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 8), output.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE(output.storage[0] == std::vector<float>{1, 2, 3, 4, 0, 0, 0, 0});

    auto stopped = snapshot(*program, 8);
    stopped.is_playing = false;
    REQUIRE(ArrangementAudioRenderer::process(*program, stopped, output.view()) ==
            AudioRenderStatus::Silent);
    REQUIRE(output.storage[0] == std::vector<float>(8, 0.0f));
}

TEST_CASE("audio compiler rejects invalid assets sample rates and capacities") {
    const auto good = audio_data({std::vector<float>(8, 1.0f)});
    auto clip = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto track = take(Track::create({10}, "bad", {clip}));
    auto project = project_with_tracks({track}, {{3, "declared", 8, {48'000, 1}}});
    const auto map = map_120();

    auto mismatched = pool({{3, audio_data({std::vector<float>(7, 1.0f)})}});
    auto direct = compile_audio_clip_program(clip, *project, *map, *mismatched, {});
    REQUIRE_FALSE(direct);
    REQUIRE(direct.error().code == AudioRendererErrorCode::AssetMetadataMismatch);

    auto wrong_rate = pool({{3, audio_data({std::vector<float>(8, 1.0f)}, 44'100)}});
    direct = compile_audio_clip_program(clip, *project, *map, *wrong_rate, {});
    REQUIRE_FALSE(direct);
    REQUIRE(direct.error().code == AudioRendererErrorCode::AssetMetadataMismatch);

    auto compiled_clip =
        take(compile_audio_clip_program(clip, *project, *map, *pool({{3, good}}), {}));

    auto malformed_arrangement = compiled_clip;
    malformed_arrangement.source_ordinal = 1;
    auto linked = link_audio_track_program({10}, {malformed_arrangement}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidIdentity);
    auto first_selection = compiled_clip;
    first_selection.source_kind = AudioClipRendererProgram::SourceKind::TakeCompSegment;
    first_selection.source_ordinal = 1;
    auto second_selection = first_selection;
    second_selection.source_ordinal = 2;
    linked = link_audio_track_program({10}, {first_selection, second_selection}, {});
    REQUIRE(linked);
    second_selection.id = {999};
    second_selection.source_ordinal = 1;
    linked = link_audio_track_program({10}, {first_selection, second_selection}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidIdentity);
    first_selection.source_ordinal = 0;
    linked = link_audio_track_program({10}, {first_selection}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidIdentity);

    linked = link_audio_track_program({10}, {compiled_clip}, {.max_clips = 0});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::CapacityExceeded);
    auto duplicate_late = compiled_clip;
    duplicate_late.timeline_start = 100;
    auto interleaved = compiled_clip;
    interleaved.id = {999};
    interleaved.timeline_start = 50;
    linked = link_audio_track_program({10}, {compiled_clip, interleaved, duplicate_late}, {});
    REQUIRE_FALSE(linked);
    REQUIRE(linked.error().code == AudioRendererErrorCode::InvalidIdentity);

    std::vector<std::vector<float>> excessive_channels(65, std::vector<float>(1, 0.0f));
    auto excessive = std::make_shared<audio::AudioFileData>();
    excessive->sample_rate = 48'000;
    excessive->channels = std::move(excessive_channels);
    auto invalid_pool = DecodedAudioAssetPool::create({{{3}, excessive}});
    REQUIRE_FALSE(invalid_pool);
    REQUIRE(invalid_pool.error().code == AudioRendererErrorCode::InvalidAsset);

    REQUIRE_FALSE(Clip::create_absolute({200}, {0}, 8, {48'000, 1}, MediaRef{{3}, {-1}, 1}));
    REQUIRE_FALSE(Clip::create_absolute({200}, {0}, 8, {48'000, 1}, MediaRef{{3}, {0}, 1},
                                        {.fade_in_duration = 9}));
    REQUIRE_FALSE(Clip::create({200}, {0}, {30}, MediaRef{{3}, {0}, 1}, {.fade_in_duration = 31}));
}

TEST_CASE("audio compiler enforces whole program track capacity") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto first = take(Track::create({10}, "first", {absolute_media_clip(100, 0, 8, 3, 0, 8)}));
    auto second = take(Track::create({11}, "second", {absolute_media_clip(101, 0, 8, 3, 0, 8)}));
    auto project = project_with_tracks({first, second}, {{3, "tone", 8, {48'000, 1}}});
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, data}});
    request.audio_limits.max_tracks = 1;
    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(store.has_value());
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::AudioProgramInvalid);
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);
}

TEST_CASE("audio compiler bounds distinct sample-rate conversion kernels") {
    const auto data_44 = audio_data({std::vector<float>(8, 1.0f)}, 44'100);
    const auto data_96 = audio_data({std::vector<float>(8, 1.0f)}, 96'000);
    auto first = absolute_media_clip(100, 0, 8, 3, 0, 8);
    auto second = absolute_media_clip(101, 8, 8, 4, 0, 8);
    auto track = take(Track::create({10}, "rates", {first, second}));
    auto project =
        project_with_tracks({track}, {{3, "44k", 8, {44'100, 1}}, {4, "96k", 8, {96'000, 1}}});
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = {2};
    request.tempo_map = map_120();
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = pool({{3, data_44}, {4, data_96}});
    request.audio_limits.max_sample_rate_converters = 1;

    REQUIRE(compiler.submit(std::move(request)));
    REQUIRE_FALSE(store.has_value());
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::AudioProgramInvalid);
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);
}

TEST_CASE("incremental audio compilation shares and globally bounds conversion kernels") {
    const auto data_44 = audio_data({std::vector<float>(8, 1.0f)}, 44'100);
    const auto data_48 = audio_data({std::vector<float>(8, 1.0f)}, 48'000);
    const auto data_96 = audio_data({std::vector<float>(8, 1.0f)}, 96'000);
    auto assets = pool({{3, data_44}, {4, data_44}, {5, data_48}, {6, data_96}});
    auto map = map_120();
    auto make_project = [](std::uint64_t second_asset) {
        auto clean = take(Track::create({10}, "clean", {absolute_media_clip(100, 0, 8, 3, 0, 8)}));
        auto dirty = take(
            Track::create({20}, "dirty", {absolute_media_clip(200, 8, 8, second_asset, 0, 8)}));
        return project_with_tracks({clean, dirty}, {{3, "44-a", 8, {44'100, 1}},
                                                    {4, "44-b", 8, {44'100, 1}},
                                                    {5, "48", 8, {48'000, 1}},
                                                    {6, "96", 8, {96'000, 1}}});
    };

    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest first;
    first.project = make_project(5);
    first.sequence_id = {2};
    first.tempo_map = map;
    first.document_revision = 1;
    first.dirty.all = true;
    first.audio_assets = assets;
    first.audio_limits.max_sample_rate_converters = 1;
    REQUIRE(compiler.submit(std::move(first)));
    REQUIRE(store.read()->document_revision() == 1);

    SECTION("a second distinct pair exceeds the whole-program limit") {
        ProgramCompileRequest second;
        second.project = make_project(6);
        second.sequence_id = {2};
        second.tempo_map = map;
        second.document_revision = 2;
        second.dirty.tracks = {{20}};
        second.audio_assets = assets;
        second.audio_limits.max_sample_rate_converters = 1;
        REQUIRE(compiler.submit(std::move(second)));
        REQUIRE(compiler.status().has_error);
        REQUIRE(compiler.status().last_error.audio_detail ==
                AudioRendererErrorCode::CapacityExceeded);
        REQUIRE(store.read()->document_revision() == 1);
    }

    SECTION("a reused pair shares the seeded converter") {
        ProgramCompileRequest second;
        second.project = make_project(4);
        second.sequence_id = {2};
        second.tempo_map = map;
        second.document_revision = 2;
        second.dirty.tracks = {{20}};
        second.audio_assets = assets;
        second.audio_limits.max_sample_rate_converters = 1;
        REQUIRE(compiler.submit(std::move(second)));
        auto published = store.read();
        REQUIRE(published->document_revision() == 2);
        const auto& clean = published->find_track({10})->audio_program()->clips().front();
        const auto& dirty = published->find_track({20})->audio_program()->clips().front();
        REQUIRE(clean.sample_rate_converter);
        REQUIRE(clean.sample_rate_converter == dirty.sample_rate_converter);
    }
}

TEST_CASE("compiler invalidation covers global clip counts assets and exact tempo identity") {
    auto make_project = [](bool extra_dirty_clip) {
        std::vector<Clip> dirty{absolute_media_clip(100, 0, 8, 3, 0, 8)};
        if (extra_dirty_clip)
            dirty.push_back(absolute_media_clip(101, 16, 8, 3, 0, 8));
        auto first = take(Track::create({10}, "dirty", std::move(dirty)));
        auto second =
            take(Track::create({20}, "clean", {absolute_media_clip(200, 32, 8, 3, 0, 8)}));
        return project_with_tracks({first, second}, {{3, "tone", 8, {48'000, 1}}});
    };
    auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto first_pool = pool({{3, data}});
    auto first_map = map_120();
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));

    ProgramCompileRequest first;
    first.project = make_project(false);
    first.sequence_id = {2};
    first.tempo_map = first_map;
    first.document_revision = 1;
    first.dirty.all = true;
    first.audio_assets = first_pool;
    first.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(first)));
    auto baseline = store.read();
    const auto* baseline_dirty = baseline->find_track({10});
    const auto* baseline_clean = baseline->find_track({20});

    ProgramCompileRequest over_limit;
    over_limit.project = make_project(true);
    over_limit.sequence_id = {2};
    over_limit.tempo_map = first_map;
    over_limit.document_revision = 2;
    over_limit.dirty.tracks = {{10}};
    over_limit.audio_assets = first_pool;
    over_limit.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(over_limit)));
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.audio_detail == AudioRendererErrorCode::CapacityExceeded);
    REQUIRE(store.read()->document_revision() == 1);

    auto second_pool = pool({{3, data}});
    ProgramCompileRequest new_pool;
    new_pool.project = make_project(false);
    new_pool.sequence_id = {2};
    new_pool.tempo_map = first_map;
    new_pool.document_revision = 3;
    new_pool.dirty.tracks = {{10}};
    new_pool.audio_assets = second_pool;
    new_pool.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(new_pool)));
    auto pool_rebuilt = store.read();
    REQUIRE(pool_rebuilt->find_track({10}) != baseline_dirty);
    REQUIRE(pool_rebuilt->find_track({20}) != baseline_clean);
    const auto* pool_dirty = pool_rebuilt->find_track({10});
    const auto* pool_clean = pool_rebuilt->find_track({20});

    auto same_rate_new_map = map_120();
    ProgramCompileRequest new_map;
    new_map.project = make_project(false);
    new_map.sequence_id = {2};
    new_map.tempo_map = same_rate_new_map;
    new_map.document_revision = 4;
    new_map.dirty.tracks = {{10}};
    new_map.audio_assets = second_pool;
    new_map.audio_limits.max_clips = 2;
    REQUIRE(compiler.submit(std::move(new_map)));
    auto map_rebuilt = store.read();
    REQUIRE(map_rebuilt->find_track({10}) != pool_dirty);
    REQUIRE(map_rebuilt->find_track({20}) != pool_clean);
    REQUIRE(map_rebuilt->tempo_map_owner().get() == same_rate_new_map.get());
}

TEST_CASE("audio renderer enforces runtime clip capacity across the whole program") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto first = take(Track::create({10}, "first", {absolute_media_clip(100, 0, 8, 3, 0, 8)}));
    auto second = take(Track::create({11}, "second", {absolute_media_clip(101, 0, 8, 3, 0, 8)}));
    auto project = project_with_tracks({first, second}, {{3, "tone", 8, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(1, 8);

    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 8), output.view(),
                                              {.max_clips = 1}) ==
            AudioRenderStatus::CapacityExceeded);
}

TEST_CASE("audio renderer capacity preflight prevents partial output from a later track") {
    const auto data = audio_data({std::vector<float>(8, 1.0f)});
    auto first = take(Track::create({10}, "first", {absolute_media_clip(100, 0, 8, 3, 0, 8)}));
    auto later = take(Track::create({11}, "later", {absolute_media_clip(101, 0, 8, 3, 0, 8)}));
    auto project = project_with_tracks({first, later}, {{3, "tone", 8, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(2, 8);

    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 8), output.view(),
                                              {.max_clips = 1}) ==
            AudioRenderStatus::CapacityExceeded);
    REQUIRE(output.storage[0] == std::vector<float>(8, 0.0f));
    REQUIRE(output.storage[1] == std::vector<float>(8, 0.0f));
}

TEST_CASE("audio render entry point is allocation free and fails closed") {
    const auto data = audio_data({std::vector<float>(64, 1.0f)});
    auto clip = absolute_media_clip(100, 0, 64, 3, 0, 64);
    auto track = take(Track::create({10}, "rt", {clip}));
    auto project = project_with_tracks({track}, {{3, "tone", 64, {48'000, 1}}});
    CompiledFixture compiled(project, map_120(), pool({{3, data}}));
    auto program = compiled.store.read();
    Output output(2, 64);
    auto state = snapshot(*program, 64);
    std::size_t allocations = 1;
    AudioRenderStatus status = AudioRenderStatus::InvalidProgram;
    {
        test::ScopedRtProcessProbe probe;
        status = ArrangementAudioRenderer::process(*program, state, output.view());
        allocations = probe.allocation_count();
    }
    REQUIRE(status == AudioRenderStatus::Rendered);
    REQUIRE(allocations == 0);

    state.ranges[0].frame_count = 65;
    REQUIRE(ArrangementAudioRenderer::process(*program, state, output.view()) ==
            AudioRenderStatus::InvalidTransport);
    REQUIRE(output.storage[0] == std::vector<float>(64, 0.0f));
    std::fill(output.storage[0].begin(), output.storage[0].end(), 7.0f);
    std::fill(output.storage[1].begin(), output.storage[1].end(), 7.0f);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 64), output.view(),
                                              {.max_channels = 1}) ==
            AudioRenderStatus::CapacityExceeded);
    REQUIRE(output.storage[0] == std::vector<float>(64, 7.0f));
    REQUIRE(output.storage[1] == std::vector<float>(64, 7.0f));

    CompiledFixture narrow(project, map_120(), pool({{3, data}}), {.max_channels = 1});
    auto narrow_program = narrow.store.read();
    REQUIRE(ArrangementAudioRenderer::process(*narrow_program, snapshot(*narrow_program, 64),
                                              output.view()) ==
            AudioRenderStatus::CapacityExceeded);
}

TEST_CASE("prepared sample-rate conversion rejects invalid source domains") {
    const audio::PreparedSampleRateConversion converter(1.0);
    REQUIRE(converter.read({}, 0.0) == 0.0f);

    constexpr std::array source{0.25f};
    REQUIRE_THAT(converter.read(source, -1.0e300), WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(converter.read(source, 1.0e300), WithinAbs(0.25f, 1.0e-6f));
    REQUIRE(converter.read(source, std::numeric_limits<double>::quiet_NaN()) == 0.0f);
    REQUIRE(converter.read(source, std::numeric_limits<double>::infinity()) == 0.0f);

    const audio::PreparedVariableRateConversion variable;
    REQUIRE(variable.read(source, 0.0,
                          audio::PreparedVariableRateConversion::
                              kMaximumSourceFramesPerOutputFrame *
                              2.0) == 0.0f);
}
