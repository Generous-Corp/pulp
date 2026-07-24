#include "timeline_dawproject_import_test_support.hpp"

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

TEST_CASE("DAWproject audio import reuses content identity across package locators") {
    auto xml = read_fixture("linear_subset.dawproject.xml");
    const auto first_path = xml.find("audio/bass.wav");
    REQUIRE(first_path != std::string::npos);
    const auto second_path = xml.find("audio/bass.wav", first_path + 1);
    REQUIRE(second_path != std::string::npos);
    xml.replace(second_path, std::string_view("audio/bass.wav").size(), "audio/bass-copy.wav");

    std::vector<std::string> resolved_paths;
    auto result = import_dawproject_xml(
        xml, [&](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
            resolved_paths.emplace_back(path);
            if (path != "audio/bass.wav" && path != "audio/bass-copy.wav")
                return std::nullopt;
            return fixture_media_bytes();
        });

    REQUIRE(result.has_value());
    const auto& project = result.value();
    REQUIRE(resolved_paths == std::vector<std::string>{"audio/bass.wav", "audio/bass-copy.wav"});
    REQUIRE(project.assets().size() == 1);
    const auto& asset = project.assets()[0];
    REQUIRE(asset.locators ==
            std::vector<AssetLocator>{{AssetLocatorKind::PackageRelative, "audio/bass.wav"},
                                      {AssetLocatorKind::PackageRelative, "audio/bass-copy.wav"}});

    const auto& clips = project.sequences()[0].tracks()[0].clips();
    REQUIRE(clips.size() == 2);
    REQUIRE(std::get<MediaRef>(clips[0].content()).asset_id == asset.id);
    REQUIRE(std::get<MediaRef>(clips[1].content()).asset_id == asset.id);
}

TEST_CASE("DAWproject rejects unsafe package paths before resolving media") {
    for (const auto unsafe :
         std::array<std::string_view, 5>{"/tmp/outside.wav", R"(C:\outside.wav)", "C:outside.wav",
                                         "../outside.wav", R"(audio\..\outside.wav)"}) {
        auto xml = read_fixture("linear_subset.dawproject.xml");
        const auto path = xml.find("audio/bass.wav");
        REQUIRE(path != std::string::npos);
        xml.replace(path, std::string_view("audio/bass.wav").size(), unsafe);
        bool resolver_called = false;
        auto result = import_dawproject_xml(
            xml, [&](std::string_view) -> std::optional<std::vector<std::uint8_t>> {
                resolver_called = true;
                return fixture_media_bytes();
            });
        CAPTURE(unsafe);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("package-relative") != std::string::npos);
        REQUIRE_FALSE(resolver_called);
    }
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

    SECTION("channel mismatch") {
        const auto offset = xml.find(R"(channels="2")");
        REQUIRE(offset != std::string::npos);
        xml.replace(offset, std::string_view(R"(channels="2")").size(), R"(channels="1")");
        auto result = import_dawproject_xml(xml, fixture_media_resolver());
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("channels") != std::string::npos);
    }

    SECTION("invalid channel declaration") {
        const auto offset = xml.find(R"(channels="2")");
        REQUIRE(offset != std::string::npos);
        xml.replace(offset, std::string_view(R"(channels="2")").size(), R"(channels="0")");
        auto result = import_dawproject_xml(xml, fixture_media_resolver());
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("channels") != std::string::npos);
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
        REQUIRE(result.error().message.find("resolved media") != std::string::npos);
    }

    SECTION("content-identical locator with conflicting declared metadata") {
        const auto first_audio = xml.find("<Audio");
        REQUIRE(first_audio != std::string::npos);
        const auto second_audio = xml.find("<Audio", first_audio + 1);
        REQUIRE(second_audio != std::string::npos);
        const auto path = xml.find("audio/bass.wav", second_audio);
        REQUIRE(path != std::string::npos);
        xml.replace(path, std::string_view("audio/bass.wav").size(), "audio/bass-copy.wav");
        const auto sample_rate = xml.find(R"(sampleRate="44100")", second_audio);
        REQUIRE(sample_rate != std::string::npos);
        xml.replace(sample_rate, std::string_view(R"(sampleRate="44100")").size(),
                    R"(sampleRate="48000")");

        auto result = import_dawproject_xml(
            xml, [](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
                if (path != "audio/bass.wav" && path != "audio/bass-copy.wav")
                    return std::nullopt;
                return fixture_media_bytes();
            });
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == DawProjectImportErrorCode::InvalidValue);
        REQUIRE(result.error().message.find("audio/bass-copy.wav") != std::string::npos);
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
        const auto xml = std::string(R"(<Project version="1.0"><Structure>)") +
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
        expect_unsupported(R"(<Clip time="0" duration="1" playStart="0.25"><Notes/></Clip>)",
                           "playStart");
    }
    SECTION("referenced Clip content") {
        expect_unsupported(R"(<Clip time="0" duration="1" reference="source-clip"><Notes/></Clip>)",
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
        expect_unsupported(R"(<Clip time="0" duration="1" playStop="1"><Notes/></Clip>)",
                           "playStop");
    }
    SECTION("Clip loopStart") {
        expect_unsupported(R"(<Clip time="0" duration="1" loopStart="0"><Notes/></Clip>)",
                           "loopStart");
    }
    SECTION("Clip loopEnd") {
        expect_unsupported(R"(<Clip time="0" duration="1" loopEnd="1"><Notes/></Clip>)", "loopEnd");
    }
    SECTION("absolute Clip content time") {
        expect_unsupported(
            R"(<Clip time="0" duration="1" contentTimeUnit="seconds"><Notes/></Clip>)",
            "contentTimeUnit");
    }
    SECTION("Audio playStart variant") {
        expect_unsupported(R"(<Clip time="0" duration="1"><Audio playStart="0.25" duration="1")"
                           R"( sampleRate="44100"><File path="audio/bass.wav"/></Audio></Clip>)",
                           "playStart");
    }
    SECTION("Audio playStop variant") {
        expect_unsupported(R"(<Clip time="0" duration="1"><Audio playStop="1" duration="1")"
                           R"( sampleRate="44100"><File path="audio/bass.wav"/></Audio></Clip>)",
                           "playStop");
    }
    SECTION("Audio loopStart variant") {
        expect_unsupported(R"(<Clip time="0" duration="1"><Audio loopStart="0" duration="1")"
                           R"( sampleRate="44100"><File path="audio/bass.wav"/></Audio></Clip>)",
                           "loopStart");
    }
    SECTION("Audio loopEnd variant") {
        expect_unsupported(R"(<Clip time="0" duration="1"><Audio loopEnd="1" duration="1")"
                           R"( sampleRate="44100"><File path="audio/bass.wav"/></Audio></Clip>)",
                           "loopEnd");
    }
    SECTION("Warp nested directly under Audio") {
        expect_unsupported(R"(<Clip time="0" duration="1"><Audio duration="1" sampleRate="44100">)"
                           R"(<File path="audio/bass.wav"/><Warp time="0" contentTime="0"/>)"
                           R"(</Audio></Clip>)",
                           "Warp");
    }
    SECTION("Warps nested under Audio") {
        expect_unsupported(R"(<Clip time="0" duration="1"><Audio duration="1" sampleRate="44100">)"
                           R"(<File path="audio/bass.wav"/><Warps contentTimeUnit="seconds"/>)"
                           R"(</Audio></Clip>)",
                           "Warps");
    }
    SECTION("Warp nested under File") {
        expect_unsupported(R"(<Clip time="0" duration="1"><Audio duration="1" sampleRate="44100">)"
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
        expect_unsupported(R"(<Clip time="0" duration="1"><Notes>)"
                           R"(<Warps contentTimeUnit="seconds" timeUnit="beats"/>)"
                           R"(</Notes></Clip>)",
                           "Warps");
    }
    SECTION("Notes in absolute time") {
        expect_unsupported(R"(<Clip time="0" duration="1"><Notes timeUnit="seconds">)"
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
        auto result =
            import_dawproject_xml(R"(<Project version="1.0"><Application name="Test" version="1"/>)"
                                  R"(<Scenes><Scene id="scene-1"/></Scenes></Project>)");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == DawProjectImportErrorCode::UnsupportedFeature);
        REQUIRE(result.error().message.find("Scene") != std::string::npos);
    }

    SECTION("unknown root semantic child is not silently dropped") {
        auto result =
            import_dawproject_xml(R"(<Project version="1.0"><Application name="Test" version="1"/>)"
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
        auto result =
            import_dawproject_xml(R"(<Project version="1.0"><Application name="Test" version="1">)"
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
        expect_unsupported(R"(<Project version="1.0"><Transport><Tempo unit="bpm" value="120"/>)"
                           R"(<Tempo unit="bpm" value="130"/></Transport></Project>)",
                           "multiple");
    }
    SECTION("nested Tempo automation") {
        expect_unsupported(R"(<Project version="1.0"><Transport><Tempo unit="bpm" value="120">)"
                           R"(<Points/></Tempo></Transport></Project>)",
                           "Points");
    }
    SECTION("unknown Structure child") {
        expect_unsupported(
            R"(<Project version="1.0"><Structure><Channel id="master"/></Structure></Project>)",
            "Channel");
    }
    SECTION("schema-valid Channel nested under imported Track") {
        expect_unsupported(R"(<Project version="1.0"><Structure><Track id="t1" name="A">)"
                           R"(<Channel role="regular" audioChannels="2"><Devices/></Channel>)"
                           R"(</Track></Structure></Project>)",
                           "Channel");
    }
    SECTION("direct Arrangement automation") {
        expect_unsupported(
            R"(<Project version="1.0"><Arrangement><Points/></Arrangement></Project>)", "Points");
    }
    SECTION("duplicate Arrangement Lanes") {
        expect_unsupported(R"(<Project version="1.0"><Arrangement><Lanes timeUnit="beats"/>)"
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

TEST_CASE("DAWproject import enforces caller-configurable resource limits") {
    const auto expect_limit = [](std::string_view xml, DawProjectMediaResolver resolver,
                                 const DawProjectImportLimits& limits,
                                 std::string_view limit_name) {
        auto result = import_dawproject_xml(xml, std::move(resolver), limits);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == DawProjectImportErrorCode::LimitExceeded);
        REQUIRE(result.error().message.find(limit_name) != std::string::npos);
    };

    SECTION("XML bytes are rejected before parsing") {
        const std::string xml = R"(<Project version="1.0"/>)";
        DawProjectImportLimits limits;
        limits.max_xml_bytes = xml.size() - 1;
        expect_limit(xml, {}, limits, "max_xml_bytes");
    }

    SECTION("track growth") {
        DawProjectImportLimits limits;
        limits.max_tracks = 1;
        expect_limit(R"(<Project version="1.0"><Structure>)"
                     R"(<Track id="a"/><Track id="b"/>)"
                     R"(</Structure></Project>)",
                     {}, limits, "max_tracks");
    }

    SECTION("clip growth across containers") {
        DawProjectImportLimits limits;
        limits.max_clips = 1;
        expect_limit(R"(<Project version="1.0"><Structure><Track id="t"/></Structure>)"
                     R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t">)"
                     R"(<Clips><Clip time="0" duration="1"/></Clips>)"
                     R"(<Clips><Clip time="1" duration="1"/></Clips>)"
                     R"(</Lanes></Lanes></Arrangement></Project>)",
                     {}, limits, "max_clips");
    }

    SECTION("note growth across clips") {
        DawProjectImportLimits limits;
        limits.max_notes = 1;
        expect_limit(R"(<Project version="1.0"><Structure><Track id="t"/></Structure>)"
                     R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t"><Clips>)"
                     R"(<Clip time="0" duration="1"><Notes>)"
                     R"(<Note time="0" duration="1" key="60"/>)"
                     R"(<Note time="0" duration="1" key="61"/>)"
                     R"(</Notes></Clip></Clips></Lanes></Lanes></Arrangement></Project>)",
                     {}, limits, "max_notes");
    }

    const std::string two_audio_clips =
        R"(<Project version="1.0"><Structure><Track id="t"/></Structure>)"
        R"(<Arrangement><Lanes timeUnit="beats"><Lanes track="t"><Clips>)"
        R"(<Clip time="0" duration="4"><Audio sampleRate="44100" duration="2">)"
        R"(<File path="audio/a.wav"/></Audio></Clip>)"
        R"(<Clip time="4" duration="4"><Audio sampleRate="44100" duration="2">)"
        R"(<File path="audio/b.wav"/></Audio></Clip>)"
        R"(</Clips></Lanes></Lanes></Arrangement></Project>)";

    SECTION("package path bytes are rejected before resolver invocation") {
        bool called = false;
        DawProjectImportLimits limits;
        limits.max_package_path_bytes = 4;
        expect_limit(
            two_audio_clips,
            [&called](std::string_view) -> std::optional<std::vector<std::uint8_t>> {
                called = true;
                return fixture_media_bytes();
            },
            limits, "max_package_path_bytes");
        REQUIRE_FALSE(called);
    }

    SECTION("resolver calls are bounded before invocation") {
        std::size_t calls = 0;
        DawProjectImportLimits limits;
        limits.max_media_resolver_calls = 1;
        expect_limit(
            two_audio_clips,
            [&calls](std::string_view) -> std::optional<std::vector<std::uint8_t>> {
                ++calls;
                return fixture_media_bytes();
            },
            limits, "max_media_resolver_calls");
        REQUIRE(calls == 1);
    }

    SECTION("resolved bytes per call are rejected before media inspection") {
        DawProjectImportLimits limits;
        limits.max_media_bytes_per_resolver_call = fixture_media_bytes().size() - 1;
        expect_limit(
            two_audio_clips,
            [](std::string_view) -> std::optional<std::vector<std::uint8_t>> {
                return fixture_media_bytes();
            },
            limits, "max_media_bytes_per_resolver_call");
    }

    SECTION("cumulative resolved bytes include repeated resolver results") {
        DawProjectImportLimits limits;
        limits.max_total_media_bytes = fixture_media_bytes().size();
        expect_limit(
            two_audio_clips,
            [](std::string_view) -> std::optional<std::vector<std::uint8_t>> {
                return fixture_media_bytes();
            },
            limits, "max_total_media_bytes");
    }

    SECTION("unique media asset growth") {
        DawProjectImportLimits limits;
        limits.max_media_assets = 1;
        expect_limit(
            two_audio_clips,
            [](std::string_view path) -> std::optional<std::vector<std::uint8_t>> {
                auto bytes = fixture_media_bytes();
                if (path == "audio/b.wav")
                    bytes.back() = 1;
                return bytes;
            },
            limits, "max_media_assets");
    }
}
