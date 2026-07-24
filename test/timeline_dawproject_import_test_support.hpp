#pragma once

#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/dawproject_import.hpp>
#include <pulp/timeline/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timeline;
using namespace pulp::timebase;

namespace {

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32u; shift += 8u)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_id(std::vector<std::uint8_t>& bytes, std::string_view id) {
    bytes.insert(bytes.end(), id.begin(), id.end());
}

const std::vector<std::uint8_t>& fixture_media_bytes() {
    static const std::vector<std::uint8_t> bytes = [] {
        constexpr std::uint32_t sample_rate = 44'100;
        constexpr std::uint32_t channels = 2;
        constexpr std::uint32_t frames = 88'200;
        constexpr std::uint32_t bytes_per_sample = 2;
        constexpr std::uint32_t data_bytes = frames * channels * bytes_per_sample;
        std::vector<std::uint8_t> wav;
        wav.reserve(44u + data_bytes);
        append_id(wav, "RIFF");
        append_u32(wav, 36u + data_bytes);
        append_id(wav, "WAVE");
        append_id(wav, "fmt ");
        append_u32(wav, 16u);
        append_u16(wav, 1u);
        append_u16(wav, channels);
        append_u32(wav, sample_rate);
        append_u32(wav, sample_rate * channels * bytes_per_sample);
        append_u16(wav, channels * bytes_per_sample);
        append_u16(wav, bytes_per_sample * 8u);
        append_id(wav, "data");
        append_u32(wav, data_bytes);
        wav.resize(wav.size() + data_bytes);
        return wav;
    }();
    return bytes;
}

DawProjectMediaResolver fixture_media_resolver() {
    return [](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
        if (path != "audio/bass.wav")
            return std::nullopt;
        return fixture_media_bytes();
    };
}

// Beats are quarter notes; positions convert through the canonical PPQ.
constexpr std::int64_t kBeat = kTicksPerQuarter;

std::string read_fixture(const std::string& relative) {
    std::string path = std::string(PULP_TIMELINE_FIXTURE_DIR) + "/dawproject/" + relative;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

class ScopedCommaNumericLocale {
  public:
    ScopedCommaNumericLocale() {
        if (const char* previous = std::setlocale(LC_NUMERIC, nullptr))
            previous_ = previous;
        for (const char* name :
             {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR", "nl_NL.UTF-8", "nl_NL"}) {
            if (std::setlocale(LC_NUMERIC, name) != nullptr &&
                std::localeconv()->decimal_point[0] == ',') {
                active_ = true;
                break;
            }
        }
    }

    ~ScopedCommaNumericLocale() {
        if (!previous_.empty())
            std::setlocale(LC_NUMERIC, previous_.c_str());
    }

    bool active() const noexcept {
        return active_;
    }

  private:
    std::string previous_;
    bool active_ = false;
};

struct PlaybackTrace {
    std::vector<float> audio;
    std::vector<std::tuple<std::uint64_t, std::uint8_t, std::uint8_t, std::uint8_t>> midi;
};

PlaybackTrace play_imported_project(std::shared_ptr<const Project> project,
                                    std::span<const std::uint32_t> block_schedule) {
    auto tempo_map = std::make_shared<const CompiledTempoMap>(
        take(CompiledTempoMap::compile(project->tempo_map().points(), {44'100, 1})));

    auto decoded = std::make_shared<audio::AudioFileData>();
    decoded->sample_rate = 44'100;
    decoded->channels.emplace_back(88'200, 0.25f);
    REQUIRE(project->assets().size() == 1);
    auto assets =
        take(DecodedAudioAssetPool::create({{project->assets()[0].id, std::move(decoded)}}));

    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = project;
    request.sequence_id = project->root_sequence_id();
    request.tempo_map = tempo_map;
    request.document_revision = 1;
    request.dirty.all = true;
    request.audio_assets = std::move(assets);
    REQUIRE(compiler.submit(std::move(request)));
    while (compiler.status().busy)
        executor.run_for(std::chrono::seconds(1), 64);
    REQUIRE_FALSE(compiler.status().has_error);

    MasterTransport transport;
    REQUIRE(transport.prepare(*tempo_map, {.max_buffer_size = 512, .initially_playing = true}) ==
            TransportError::None);
    const auto* sequence = project->find_sequence(project->root_sequence_id());
    REQUIRE(sequence != nullptr);
    const auto note_track =
        std::find_if(sequence->tracks().begin(), sequence->tracks().end(), [](const Track& track) {
            const auto clips = track.clips();
            return !clips.empty() && std::holds_alternative<NoteContent>(clips[0].content());
        });
    REQUIRE(note_track != sequence->tracks().end());
    ArrangementNoteRenderer notes(note_track->id());
    REQUIRE(notes.prepare(8));
    PlaybackProgramBlockLatch latch;

    REQUIRE(sequence->duration().has_value());
    const auto total_frames = static_cast<std::uint64_t>(
        tempo_map->ticks_to_samples({sequence->duration()->value}).value);

    PlaybackTrace trace;
    trace.audio.reserve(total_frames);
    std::uint64_t rendered = 0;
    std::size_t schedule_index = 0;
    while (rendered < total_frames) {
        const auto requested = block_schedule[schedule_index++ % block_schedule.size()];
        const auto frames =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(requested, total_frames - rendered));
        TransportSnapshot snapshot;
        REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
        auto block = latch.begin_block(store);
        REQUIRE(block);

        std::vector<float> block_audio(frames);
        float* channel = block_audio.data();
        audio::BufferView<float> output(&channel, 1, frames);
        REQUIRE(ArrangementAudioRenderer::process(*block.program(), snapshot, output) ==
                AudioRenderStatus::Rendered);
        const auto note_result = notes.process(block, snapshot);
        REQUIRE(note_result.code == NoteRenderCode::Ok);
        trace.audio.insert(trace.audio.end(), block_audio.begin(), block_audio.end());
        for (const auto& event : notes.events()) {
            REQUIRE(event.size() == 3);
            trace.midi.emplace_back(rendered + event.sample_offset, event.data()[0],
                                    event.data()[1], event.data()[2]);
        }
        rendered += frames;
    }
    return trace;
}

} // namespace
