#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/note_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/dawproject_import.hpp>
#include <pulp/timeline/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
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
    auto result = import_dawproject_xml(read_fixture("linear_subset.dawproject.xml"));
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

TEST_CASE("DAWproject import fails closed on an out-of-subset clip (no silent drop)") {
    // The clip's real content is a <Warps> timeline. The importer must reject it
    // rather than import an empty clip and lose the content.
    auto result = import_dawproject_xml(read_fixture("out_of_subset.dawproject.xml"));
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
    REQUIRE(result.error().message.find("Warps") != std::string::npos);
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

TEST_CASE("DAWproject imported arrangement plays identically across block schedules") {
    auto imported = import_dawproject_xml(read_fixture("linear_subset.dawproject.xml"));
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
