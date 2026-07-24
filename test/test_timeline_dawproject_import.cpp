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

TEST_CASE("DAWproject import maps the linear subset into the timeline model") {
    auto result = import_dawproject_xml(read_fixture("linear_subset.dawproject.xml"),
                                        fixture_media_resolver());
    REQUIRE(result.has_value());
    const Project& project = result.value();

    // Tempo + meter land at bar one.
    REQUIRE(project.tempo_map().points().size() == 1);
    REQUIRE(project.tempo_map().points()[0].bpm == 140.0);
    REQUIRE(project.meter_map().points().size() == 1);
    REQUIRE(project.meter_map().points()[0].signature.numerator == 3);
    REQUIRE(project.meter_map().points()[0].signature.denominator == 4);

    // One arrangement sequence with the two structure tracks, in structure order.
    REQUIRE(project.sequences().size() == 1);
    const Sequence& sequence = project.sequences()[0];
    REQUIRE(sequence.id() == project.root_sequence_id());
    REQUIRE(sequence.tracks().size() == 2);
    const Track& bass = sequence.tracks()[0];
    const Track& lead = sequence.tracks()[1];
    REQUIRE(bass.name() == "Bass");
    REQUIRE(lead.name() == "Lead");

    // The two bass clips reference one deduplicated audio asset.
    REQUIRE(project.assets().size() == 1);
    const MediaAsset& asset = project.assets()[0];
    REQUIRE(asset.frame_count == 88200); // 2.0s * 44100
    REQUIRE(asset.sample_rate == RationalRate{44100, 1});
    REQUIRE(asset.content_hash.valid());
    REQUIRE(asset.content_hash.to_hex() ==
            runtime::sha256_hex(fixture_media_bytes().data(), fixture_media_bytes().size()));
    REQUIRE(asset.locators.size() == 1);
    REQUIRE(asset.locators[0].kind == AssetLocatorKind::PackageRelative);
    REQUIRE(asset.locators[0].hint == "audio/bass.wav");

    // Bass clip placements, ordered by start.
    auto bass_clips = bass.clips();
    REQUIRE(bass_clips.size() == 2);
    REQUIRE(bass_clips[0].start().value == 0);
    REQUIRE(bass_clips[0].duration().value == 4 * kBeat);
    REQUIRE(bass_clips[1].start().value == 6 * kBeat);
    REQUIRE(bass_clips[1].duration().value == 2 * kBeat);
    const auto& bass_ref = std::get<MediaRef>(bass_clips[0].content());
    REQUIRE(bass_ref.asset_id == asset.id);
    REQUIRE(std::get<MediaRef>(bass_clips[1].content()).asset_id == asset.id);

    // Lead note clip.
    auto lead_clips = lead.clips();
    REQUIRE(lead_clips.size() == 1);
    REQUIRE(lead_clips[0].start().value == 4 * kBeat);
    REQUIRE(lead_clips[0].duration().value == 4 * kBeat);
    const auto& notes = std::get<NoteContent>(lead_clips[0].content()).notes();
    REQUIRE(notes.size() == 2);
    REQUIRE(notes[0].start.value == 0);
    REQUIRE(notes[0].duration.value == 1 * kBeat);
    REQUIRE(notes[0].pitch == 60);
    REQUIRE(notes[0].velocity == 52428); // round(0.8 * 65535)
    REQUIRE(notes[1].start.value == 2 * kBeat);
    REQUIRE(notes[1].pitch == 64);
    REQUIRE(notes[1].velocity == 65535);

    // Sequence duration spans the latest clip end (8 beats on both tracks).
    REQUIRE(sequence.duration().has_value());
    REQUIRE(sequence.duration()->value == 8 * kBeat);
}

TEST_CASE("DAWproject audio import requires media bytes to seal durable identity") {
    auto result = import_dawproject_xml(read_fixture("linear_subset.dawproject.xml"));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == DawProjectImportErrorCode::MissingMediaBytes);
    REQUIRE(result.error().message.find("audio/bass.wav") != std::string::npos);
}

TEST_CASE("DAWproject audio import validates metadata against resolved WAV bytes") {
    auto xml = read_fixture("linear_subset.dawproject.xml");

    SECTION("sample rate mismatch") {
        const auto offset = xml.find(R"(sampleRate="44100")");
        REQUIRE(offset != std::string::npos);
        xml.replace(offset, std::string_view(R"(sampleRate="44100")").size(),
                    R"(sampleRate="48000")");
        auto result = import_dawproject_xml(xml, fixture_media_resolver());
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("sampleRate") != std::string::npos);
    }

    SECTION("duration mismatch") {
        const auto offset = xml.find(R"(<Audio algorithm="stretch" channels="2" duration="2.0")");
        REQUIRE(offset != std::string::npos);
        const auto duration_offset = xml.find(R"(duration="2.0")", offset);
        REQUIRE(duration_offset != std::string::npos);
        xml.replace(duration_offset, std::string_view(R"(duration="2.0")").size(),
                    R"(duration="1.0")");
        auto result = import_dawproject_xml(xml, fixture_media_resolver());
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("duration") != std::string::npos);
    }

    SECTION("huge finite duration") {
        const auto audio = xml.find("<Audio");
        REQUIRE(audio != std::string::npos);
        const auto duration = xml.find(R"(duration="2.0")", audio);
        REQUIRE(duration != std::string::npos);
        xml.replace(duration, std::string_view(R"(duration="2.0")").size(),
                    R"(duration="418446744073709.55")");
        auto result = import_dawproject_xml(xml, fixture_media_resolver());
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("duration") != std::string::npos);
    }

    SECTION("reused path with conflicting metadata") {
        const auto first_audio = xml.find("<Audio");
        REQUIRE(first_audio != std::string::npos);
        const auto second_audio = xml.find("<Audio", first_audio + 1);
        REQUIRE(second_audio != std::string::npos);
        const auto duration = xml.find(R"(duration="2.0")", second_audio);
        REQUIRE(duration != std::string::npos);
        xml.replace(duration, std::string_view(R"(duration="2.0")").size(), R"(duration="1.0")");
        auto result = import_dawproject_xml(xml, fixture_media_resolver());
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("earlier reference") != std::string::npos);
    }

    SECTION("invalid media") {
        DawProjectMediaResolver invalid =
            [](std::string_view) -> std::optional<std::vector<std::uint8_t>> {
            return std::vector<std::uint8_t>{'n', 'o', 't', '-', 'w', 'a', 'v'};
        };
        auto result = import_dawproject_xml(xml, std::move(invalid));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("invalid or unsupported WAV") != std::string::npos);
    }
}

TEST_CASE("DAWproject import fails closed on an out-of-subset clip (no silent drop)") {
    // The clip's real content is a <Warps> timeline. The importer must reject it
    // rather than import an empty clip and lose the content.
    auto result = import_dawproject_xml(read_fixture("out_of_subset.dawproject.xml"));
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
    REQUIRE(result.error().message.find("Warps") != std::string::npos);
}

TEST_CASE("DAWproject import fails closed on unsupported audio sub-range and warp metadata") {
    auto expect_error = [](std::string_view clip_xml, DawProjectImportErrorCode code,
                           std::string_view detail) {
        const auto xml =
            std::string(R"(<Project version="1.0"><Structure>)") +
            R"(<Track id="t1" name="A"/></Structure>)" +
            R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)" +
            std::string(clip_xml) +
            R"(</Clips></Lanes></Lanes></Arrangement></Project>)";
        auto result = import_dawproject_xml(xml);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == code);
        REQUIRE(result.error().message.find(detail) != std::string::npos);
    };
    auto expect_unsupported = [&](std::string_view clip_xml, std::string_view detail) {
        expect_error(clip_xml, DawProjectImportErrorCode::UnsupportedFeature, detail);
    };

    SECTION("standard Clip playStart sub-range") {
        expect_unsupported(
            R"(<Clip time="0" duration="1" playStart="0.25"><Notes/></Clip>)",
            "playStart");
    }
    SECTION("referenced Clip content") {
        expect_unsupported(
            R"(<Clip time="0" duration="1" reference="source-clip"><Notes/></Clip>)",
            "referenced-content");
    }
    SECTION("Clip playStart is not numeric") {
        expect_error(R"(<Clip time="0" duration="1" playStart="junk"><Notes/></Clip>)",
                     DawProjectImportErrorCode::InvalidValue, "playStart");
    }
    SECTION("Clip playStart is non-finite") {
        expect_error(R"(<Clip time="0" duration="1" playStart="inf"><Notes/></Clip>)",
                     DawProjectImportErrorCode::InvalidValue, "playStart");
    }
    SECTION("Clip playStop") {
        expect_unsupported(
            R"(<Clip time="0" duration="1" playStop="1"><Notes/></Clip>)",
            "playStop");
    }
    SECTION("Clip loopStart") {
        expect_unsupported(
            R"(<Clip time="0" duration="1" loopStart="0"><Notes/></Clip>)",
            "loopStart");
    }
    SECTION("Clip loopEnd") {
        expect_unsupported(
            R"(<Clip time="0" duration="1" loopEnd="1"><Notes/></Clip>)",
            "loopEnd");
    }
    SECTION("absolute Clip content time") {
        expect_unsupported(
            R"(<Clip time="0" duration="1" contentTimeUnit="seconds"><Notes/></Clip>)",
            "contentTimeUnit");
    }
    SECTION("Audio playStart variant") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Audio playStart="0.25" duration="1")"
            R"( sampleRate="44100"><File path="audio/bass.wav"/></Audio></Clip>)",
            "playStart");
    }
    SECTION("Audio playStop variant") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Audio playStop="1" duration="1")"
            R"( sampleRate="44100"><File path="audio/bass.wav"/></Audio></Clip>)",
            "playStop");
    }
    SECTION("Audio loopStart variant") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Audio loopStart="0" duration="1")"
            R"( sampleRate="44100"><File path="audio/bass.wav"/></Audio></Clip>)",
            "loopStart");
    }
    SECTION("Audio loopEnd variant") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Audio loopEnd="1" duration="1")"
            R"( sampleRate="44100"><File path="audio/bass.wav"/></Audio></Clip>)",
            "loopEnd");
    }
    SECTION("Warp nested directly under Audio") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Audio duration="1" sampleRate="44100">)"
            R"(<File path="audio/bass.wav"/><Warp time="0" contentTime="0"/>)"
            R"(</Audio></Clip>)",
            "Warp");
    }
    SECTION("Warps nested under Audio") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Audio duration="1" sampleRate="44100">)"
            R"(<File path="audio/bass.wav"/><Warps contentTimeUnit="seconds"/>)"
            R"(</Audio></Clip>)",
            "Warps");
    }
    SECTION("Warp nested under File") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Audio duration="1" sampleRate="44100">)"
            R"(<File path="audio/bass.wav"><Warp time="0" contentTime="0"/></File>)"
            R"(</Audio></Clip>)",
            "Warp");
    }
    SECTION("Warps nested under Note") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Notes>)"
            R"(<Note time="0" duration="1" channel="0" key="60">)"
            R"(<Warps contentTimeUnit="seconds" timeUnit="beats">)"
            R"(<Audio duration="1" sampleRate="44100"><File path="audio/bass.wav"/></Audio>)"
            R"(<Warp time="0" contentTime="0"/></Warps></Note></Notes></Clip>)",
            "Warps");
    }
    SECTION("non-Note timeline nested under Notes") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Notes>)"
            R"(<Warps contentTimeUnit="seconds" timeUnit="beats"/>)"
            R"(</Notes></Clip>)",
            "Warps");
    }
    SECTION("Notes in absolute time") {
        expect_unsupported(
            R"(<Clip time="0" duration="1"><Notes timeUnit="seconds">)"
            R"(<Note time="0" duration="1" channel="0" key="60"/>)"
            R"(</Notes></Clip>)",
            "timeUnit");
    }
}

TEST_CASE("DAWproject import rejects absolute timing on a Clips container") {
    auto result = import_dawproject_xml(
        R"(<Project version="1.0"><Structure><Track id="t1" name="A"/></Structure>)"
        R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips timeUnit="seconds">)"
        R"(<Clip time="0" duration="1"><Notes/></Clip>)"
        R"(</Clips></Lanes></Lanes></Arrangement></Project>)");
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
    REQUIRE(result.error().message.find("timeUnit") != std::string::npos);
}

TEST_CASE("DAWproject import rejects absolute timing on track-scoped Lanes") {
    auto result = import_dawproject_xml(
        R"(<Project version="1.0"><Structure><Track id="t1" name="A"/></Structure>)"
        R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1" timeUnit="seconds"><Clips>)"
        R"(<Clip time="0" duration="1"><Notes/></Clip>)"
        R"(</Clips></Lanes></Lanes></Arrangement></Project>)");
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
    REQUIRE(result.error().message.find("timeUnit") != std::string::npos);
}

TEST_CASE("DAWproject import accepts explicit zero playStart as whole-content playback") {
    auto result = import_dawproject_xml(
        R"(<Project version="1.0"><Structure><Track id="t1" name="A"/></Structure>)"
        R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
        R"(<Clip time="0" duration="1" contentTimeUnit="beats" playStart="0"><Notes/></Clip>)"
        R"(</Clips></Lanes></Lanes></Arrangement></Project>)");
    REQUIRE(result);
    REQUIRE(result->sequences()[0].tracks()[0].clips().size() == 1);
}

TEST_CASE("DAWproject import validates Project-level semantic containers") {
    SECTION("populated Scenes are not silently dropped") {
        auto result = import_dawproject_xml(
            R"(<Project version="1.0"><Application name="Test" version="1"/>)"
            R"(<Scenes><Scene id="scene-1"/></Scenes></Project>)");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
        REQUIRE(result.error().message.find("Scene") != std::string::npos);
    }

    SECTION("unknown root semantic child is not silently dropped") {
        auto result = import_dawproject_xml(
            R"(<Project version="1.0"><Application name="Test" version="1"/>)"
            R"(<SessionData><Clip id="hidden"/></SessionData></Project>)");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
        REQUIRE(result.error().message.find("SessionData") != std::string::npos);
    }

    SECTION("allowlisted application metadata and empty Scenes are harmless") {
        auto result = import_dawproject_xml(
            R"(<Project version="1.0"><Application name="Test DAW" version="9.2"/>)"
            R"(<Scenes/></Project>)");
        REQUIRE(result);
        REQUIRE(result->sequences().size() == 1);
    }

    SECTION("nested application content is not treated as metadata") {
        auto result = import_dawproject_xml(
            R"(<Project version="1.0"><Application name="Test" version="1">)"
            R"(<SessionData/></Application></Project>)");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
        REQUIRE(result.error().message.find("SessionData") != std::string::npos);
    }
}

TEST_CASE("DAWproject import exhaustively validates imported semantic containers") {
    auto expect_unsupported = [](std::string_view xml, std::string_view detail) {
        auto result = import_dawproject_xml(xml);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
        REQUIRE(result.error().message.find(detail) != std::string::npos);
    };

    SECTION("unknown Transport child") {
        expect_unsupported(
            R"(<Project version="1.0"><Transport><Loop start="0" end="4"/></Transport></Project>)",
            "Loop");
    }
    SECTION("duplicate Tempo") {
        expect_unsupported(
            R"(<Project version="1.0"><Transport><Tempo unit="bpm" value="120"/>)"
            R"(<Tempo unit="bpm" value="130"/></Transport></Project>)",
            "multiple");
    }
    SECTION("nested Tempo automation") {
        expect_unsupported(
            R"(<Project version="1.0"><Transport><Tempo unit="bpm" value="120">)"
            R"(<Points/></Tempo></Transport></Project>)",
            "Points");
    }
    SECTION("unknown Structure child") {
        expect_unsupported(
            R"(<Project version="1.0"><Structure><Channel id="master"/></Structure></Project>)",
            "Channel");
    }
    SECTION("schema-valid Channel nested under imported Track") {
        expect_unsupported(
            R"(<Project version="1.0"><Structure><Track id="t1" name="A">)"
            R"(<Channel role="regular" audioChannels="2"><Devices/></Channel>)"
            R"(</Track></Structure></Project>)",
            "Channel");
    }
    SECTION("direct Arrangement automation") {
        expect_unsupported(
            R"(<Project version="1.0"><Arrangement><Points/></Arrangement></Project>)",
            "Points");
    }
    SECTION("duplicate Arrangement Lanes") {
        expect_unsupported(
            R"(<Project version="1.0"><Arrangement><Lanes timeUnit="beats"/>)"
            R"(<Lanes timeUnit="beats"/></Arrangement></Project>)",
            "multiple");
    }
}

TEST_CASE("DAWproject import rejects malformed and out-of-subset input") {
    auto expect = [](std::string_view xml, DawProjectImportErrorCode code) {
        auto result = import_dawproject_xml(xml);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == code);
    };

    SECTION("malformed XML") {
        expect("this is not <<< xml", DawProjectImportErrorCode::ParseError);
    }
    SECTION("wrong root element") {
        expect("<NotAProject/>", DawProjectImportErrorCode::MissingRoot);
    }
    SECTION("unsupported major version") {
        expect(R"(<Project version="2.0"/>)", DawProjectImportErrorCode::UnsupportedVersion);
    }
    SECTION("non-bpm tempo unit") {
        expect(R"(<Project version="1.0"><Transport>)"
               R"(<Tempo unit="linear" value="120"/></Transport></Project>)",
               DawProjectImportErrorCode::UnsupportedFeature);
    }
    SECTION("absolute (seconds) arrangement timing") {
        expect(R"(<Project version="1.0"><Arrangement>)"
               R"(<Lanes timeUnit="seconds"/></Arrangement></Project>)",
               DawProjectImportErrorCode::UnsupportedFeature);
    }
    SECTION("nested group track") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="g" name="Group"><Track id="c" name="Child"/></Track>)"
               R"(</Structure></Project>)",
               DawProjectImportErrorCode::UnsupportedFeature);
    }
    SECTION("duplicate track id") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="dup" name="A"/><Track id="dup" name="B"/>)"
               R"(</Structure></Project>)",
               DawProjectImportErrorCode::DuplicateTrackId);
    }
    SECTION("dangling track reference in arrangement") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats">)"
               R"(<Lanes track="ghost"><Clips/></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::DanglingTrackReference);
    }
    SECTION("clip missing required duration") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0"/></Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::MissingAttribute);
    }
    SECTION("clip non-finite time") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="inf" duration="1.0"/>)"
               R"(</Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("clip time has trailing junk") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="1junk" duration="1.0"/>)"
               R"(</Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("clip huge duration") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1e300"/>)"
               R"(</Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("clip duration rounds to zero ticks") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1e-20"/>)"
               R"(</Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("clip tick range overflows") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="10000000000000" duration="10000000000000"/>)"
               R"(</Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("note tick range overflows") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1.0"><Notes>)"
               R"(<Note time="10000000000000" duration="10000000000000" key="60"/>)"
               R"(</Notes></Clip></Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("note time is not numeric") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1.0"><Notes>)"
               R"(<Note time="garbage" duration="1.0" key="60"/>)"
               R"(</Notes></Clip></Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("note velocity is non-finite") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1.0"><Notes>)"
               R"(<Note time="0.0" duration="1.0" key="60" vel="nan"/>)"
               R"(</Notes></Clip></Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("note velocity has trailing junk") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1.0"><Notes>)"
               R"(<Note time="0.0" duration="1.0" key="60" vel="0.5junk"/>)"
               R"(</Notes></Clip></Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("note channel has trailing junk") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1.0"><Notes>)"
               R"(<Note time="0.0" duration="1.0" key="60" channel="1junk"/>)"
               R"(</Notes></Clip></Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("note pitch out of range") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1.0"><Notes>)"
               R"(<Note time="0.0" duration="1.0" key="200"/></Notes></Clip>)"
               R"(</Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::InvalidValue);
    }
    SECTION("audio clip missing File reference") {
        expect(R"(<Project version="1.0"><Structure>)"
               R"(<Track id="t1" name="A"/></Structure>)"
               R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
               R"(<Clip time="0.0" duration="1.0">)"
               R"(<Audio duration="1.0" sampleRate="44100"/></Clip>)"
               R"(</Clips></Lanes></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::UnsupportedFeature);
    }
    SECTION("unsupported arrangement-level timeline") {
        // A <Markers> timeline at arrangement root must not be silently ignored.
        expect(R"(<Project version="1.0"><Arrangement><Lanes timeUnit="beats">)"
               R"(<Markers/></Lanes></Arrangement></Project>)",
               DawProjectImportErrorCode::UnsupportedFeature);
    }
}

TEST_CASE("DAWproject import accepts an empty project with defaults") {
    // No transport, structure, or arrangement: 120 bpm / 4-4 defaults, no tracks.
    auto result = import_dawproject_xml(R"(<Project version="1.0"/>)");
    REQUIRE(result.has_value());
    const Project& project = result.value();
    REQUIRE(project.sequences().size() == 1);
    REQUIRE(project.sequences()[0].tracks().empty());
    REQUIRE(project.tempo_map().points()[0].bpm == 120.0);
    REQUIRE(project.meter_map().points()[0].signature.numerator == 4);
}

TEST_CASE("DAWproject decimal parsing is independent of the host numeric locale") {
    ScopedCommaNumericLocale locale;
    if (!locale.active())
        SKIP("no comma-decimal locale is installed");

    auto result = import_dawproject_xml(
        R"(<Project version="1.0"><Transport><Tempo unit="bpm" value="120.5"/></Transport>)"
        R"(<Structure><Track id="t1" name="A"/></Structure>)"
        R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t1"><Clips>)"
        R"(<Clip time="0.5" duration="1.5"><Notes>)"
        R"(<Note time="0.25" duration="0.5" key="60" vel="0.5"/>)"
        R"(</Notes></Clip></Clips></Lanes></Lanes></Arrangement></Project>)");
    REQUIRE(result);
    REQUIRE(result.value().tempo_map().points()[0].bpm == 120.5);
    const auto& clip = result.value().sequences()[0].tracks()[0].clips()[0];
    REQUIRE(clip.start().value == kBeat / 2);
    REQUIRE(clip.duration().value == 3 * kBeat / 2);
    REQUIRE(std::get<NoteContent>(clip.content()).notes()[0].velocity == 32768);
}

TEST_CASE("DAWproject imported arrangement plays identically across block schedules") {
    auto imported = import_dawproject_xml(read_fixture("linear_subset.dawproject.xml"),
                                          fixture_media_resolver());
    REQUIRE(imported);
    auto project = std::make_shared<const Project>(std::move(imported).value());

    constexpr std::array regular{512u};
    constexpr std::array varying{17u, 251u, 64u, 509u, 3u, 128u};
    const auto regular_trace = play_imported_project(project, regular);
    const auto varying_trace = play_imported_project(project, varying);

    REQUIRE(regular_trace.audio == varying_trace.audio);
    REQUIRE(regular_trace.midi == varying_trace.midi);
    REQUIRE(regular_trace.audio.size() == 151'200);
    REQUIRE(std::any_of(regular_trace.audio.begin(), regular_trace.audio.end(),
                        [](float sample) { return sample != 0.0f; }));
    REQUIRE(regular_trace.midi.size() == 4);
}
