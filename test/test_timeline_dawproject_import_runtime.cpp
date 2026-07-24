#include "timeline_dawproject_import_test_support.hpp"

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
