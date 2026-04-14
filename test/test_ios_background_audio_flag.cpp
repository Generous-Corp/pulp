// Verifies the PluginDescriptor::ios_requires_background_audio flag
// (workstream 05 slice 5.5). The host-app layer consumes the flag to
// decide whether to request the iOS `audio` UIBackgroundModes
// entitlement and keep AVAudioSession active in background.

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>

using namespace pulp::format;

TEST_CASE("ios_requires_background_audio defaults to false",
          "[ios][descriptor]") {
    PluginDescriptor d;
    REQUIRE(d.ios_requires_background_audio == false);
}

TEST_CASE("ios_requires_background_audio is settable per plugin",
          "[ios][descriptor]") {
    PluginDescriptor effect;
    effect.category = PluginCategory::Effect;
    effect.ios_requires_background_audio = false;
    REQUIRE_FALSE(effect.ios_requires_background_audio);

    PluginDescriptor live_synth;
    live_synth.category = PluginCategory::Instrument;
    live_synth.ios_requires_background_audio = true;
    REQUIRE(live_synth.ios_requires_background_audio);
}
