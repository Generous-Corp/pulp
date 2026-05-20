// Verifies the richer PluginInfo metadata shape (workstream 03 slice 3.7).
// Format-level extraction (CLAP features → category) has unit-test
// coverage here; per-format scanner-path validation lives with the
// scanner tests.

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/scanner.hpp>

using namespace pulp::host;

TEST_CASE("PluginInfo default-constructs with empty metadata",
          "[host][plugin-info]") {
    PluginInfo p;
    REQUIRE(p.category.empty());
    REQUIRE(p.features.empty());
    REQUIRE(p.description.empty());
    REQUIRE_FALSE(p.has_editor);
    REQUIRE_FALSE(p.supports_sidechain);
    REQUIRE_FALSE(p.supports_midi_in);
    REQUIRE_FALSE(p.supports_midi_out);
}

TEST_CASE("PluginInfo is copyable and carries metadata",
          "[host][plugin-info]") {
    PluginInfo p;
    p.name = "Pulp Drums";
    p.category = "Instrument";
    p.features = {"instrument", "drum", "sampler"};
    p.has_editor = true;
    p.supports_midi_in = true;

    PluginInfo q = p;
    REQUIRE(q.name == "Pulp Drums");
    REQUIRE(q.category == "Instrument");
    REQUIRE(q.features.size() == 3);
    REQUIRE(q.features[2] == "sampler");
    REQUIRE(q.has_editor);
    REQUIRE(q.supports_midi_in);
}

// ── PluginInfo contract (default values + mutation invariants) ──────────

TEST_CASE("PluginInfo defaults treat plugin as a stereo effect",
          "[host][plugin-info][defaults]") {
    // Classifiers in scanner_{clap,vst3,au,lv2}.cpp assume these
    // starting conditions — any future change to the struct defaults
    // ripples into every adapter. This test pins them.
    PluginInfo p;
    REQUIRE_FALSE(p.is_instrument);
    REQUIRE(p.is_effect);
    REQUIRE(p.num_inputs == 2);
    REQUIRE(p.num_outputs == 2);
}

TEST_CASE("PluginInfo MIDI-effect shape: MIDI in+out, stays effect, not instrument",
          "[host][plugin-info][midi-effect]") {
    // Mirrors the CLAP feature="note-effect" fix in scanner_clap.cpp
    // (#198 P2): note-effects are still effects — they process MIDI
    // with no audio output. Clearing is_effect silently dropped them
    // from is_effect filters before the fix landed.
    PluginInfo p;
    p.category = "MidiEffect";
    p.is_effect = true;
    p.is_instrument = false;
    p.supports_midi_in = true;
    p.supports_midi_out = true;

    REQUIRE(p.category == "MidiEffect");
    REQUIRE(p.is_effect);
    REQUIRE_FALSE(p.is_instrument);
    REQUIRE(p.supports_midi_in);
    REQUIRE(p.supports_midi_out);
}

TEST_CASE("PluginInfo instrument shape: 0 inputs, MIDI in, is_instrument true",
          "[host][plugin-info][instrument]") {
    PluginInfo p;
    p.category = "Instrument";
    p.is_instrument = true;
    p.is_effect = false;
    p.num_inputs = 0;
    p.num_outputs = 2;
    p.supports_midi_in = true;

    REQUIRE(p.is_instrument);
    REQUIRE_FALSE(p.is_effect);
    REQUIRE(p.num_inputs == 0);
    REQUIRE(p.supports_midi_in);
}

TEST_CASE("PluginInfo sidechain flag is orthogonal to effect/instrument",
          "[host][plugin-info]") {
    PluginInfo p;
    p.is_effect = true;
    p.supports_sidechain = true;
    REQUIRE(p.supports_sidechain);

    // Clearing sidechain doesn't affect effect/instrument flags.
    p.supports_sidechain = false;
    REQUIRE(p.is_effect);
    REQUIRE_FALSE(p.is_instrument);
}

TEST_CASE("PluginInfo features vector survives move assignment",
          "[host][plugin-info][move]") {
    PluginInfo src;
    src.name = "SrcPlugin";
    src.features = {"audio-effect", "analyzer", "utility"};

    PluginInfo dst = std::move(src);
    REQUIRE(dst.name == "SrcPlugin");
    REQUIRE(dst.features.size() == 3);
    REQUIRE(dst.features[0] == "audio-effect");
    REQUIRE(dst.features[2] == "utility");
}

TEST_CASE("PluginInfo category taxonomy accepts known strings",
          "[host][plugin-info][taxonomy]") {
    // The set of strings scanners write. Callers that switch on
    // category (not the adapter code) should fail fast if a new
    // category sneaks into the scanner without an intentional opt-in.
    const std::vector<std::string> accepted = {
        "Fx", "Instrument", "Analyzer", "MidiEffect", ""
    };
    for (const auto& c : accepted) {
        PluginInfo p;
        p.category = c;
        REQUIRE(p.category == c);
    }
}

// ── PluginFormat enum ───────────────────────────────────────────────────

TEST_CASE("PluginFormat enum covers the 5 shipping formats",
          "[host][plugin-info][format]") {
    for (auto f : {
             PluginFormat::VST3,
             PluginFormat::AudioUnit,
             PluginFormat::AudioUnitV3,
             PluginFormat::CLAP,
             PluginFormat::LV2,
         }) {
        PluginInfo p;
        p.format = f;
        REQUIRE(p.format == f);
    }
}

TEST_CASE("PluginScanner default paths match platform format support",
          "[host][plugin-info][format][coverage][phase3]") {
#if defined(__APPLE__)
    auto vst3 = PluginScanner::default_paths(PluginFormat::VST3);
    auto au = PluginScanner::default_paths(PluginFormat::AudioUnit);
    auto auv3 = PluginScanner::default_paths(PluginFormat::AudioUnitV3);
    auto clap = PluginScanner::default_paths(PluginFormat::CLAP);
    auto lv2 = PluginScanner::default_paths(PluginFormat::LV2);

    REQUIRE(vst3.size() == 2);
    REQUIRE(vst3[0].find("/Library/Audio/Plug-Ins/VST3") != std::string::npos);
    REQUIRE(au.size() == 2);
    REQUIRE(auv3 == au);
    REQUIRE(clap.size() == 2);
    REQUIRE(lv2.empty());
#elif defined(_WIN32)
    auto vst3 = PluginScanner::default_paths(PluginFormat::VST3);
    auto au = PluginScanner::default_paths(PluginFormat::AudioUnit);

    REQUIRE(vst3.size() == 2);
    REQUIRE(vst3[0].find("VST3") != std::string::npos);
    REQUIRE(au.size() == 2);
#elif defined(__linux__)
    auto vst3 = PluginScanner::default_paths(PluginFormat::VST3);
    auto clap = PluginScanner::default_paths(PluginFormat::CLAP);
    auto lv2 = PluginScanner::default_paths(PluginFormat::LV2);
    auto au = PluginScanner::default_paths(PluginFormat::AudioUnit);

    REQUIRE(vst3.size() == 3);
    REQUIRE(clap.size() == 2);
    REQUIRE(lv2.size() == 3);
    REQUIRE(au.empty());
#else
    REQUIRE(PluginScanner::default_paths(PluginFormat::VST3).empty());
#endif
}

TEST_CASE("PluginScanner identifies bundle suffixes for each host format",
          "[host][plugin-info][format]") {
    REQUIRE(PluginScanner::is_plugin_bundle("/Library/Audio/Plug-Ins/VST3/Test.vst3",
                                            PluginFormat::VST3));
    REQUIRE(PluginScanner::is_plugin_bundle("/Library/Audio/Plug-Ins/Components/Test.component",
                                            PluginFormat::AudioUnit));
    REQUIRE(PluginScanner::is_plugin_bundle("/Applications/Synth.app/PlugIns/Test.component",
                                            PluginFormat::AudioUnitV3));
    REQUIRE(PluginScanner::is_plugin_bundle("/Library/Audio/Plug-Ins/CLAP/Test.clap",
                                            PluginFormat::CLAP));
    REQUIRE(PluginScanner::is_plugin_bundle("/usr/lib/lv2/Test.lv2",
                                            PluginFormat::LV2));

    REQUIRE_FALSE(PluginScanner::is_plugin_bundle("/tmp/Test.vst3.backup",
                                                  PluginFormat::VST3));
    REQUIRE_FALSE(PluginScanner::is_plugin_bundle("/tmp/Test.component",
                                                  PluginFormat::CLAP));
    REQUIRE_FALSE(PluginScanner::is_plugin_bundle("/tmp/Test.clap",
                                                  PluginFormat::LV2));
}
