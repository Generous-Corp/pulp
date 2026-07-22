#include <pulp/timeline/dawproject_import.hpp>
#include <pulp/timeline/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>

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
