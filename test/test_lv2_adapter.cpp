#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <pulp/format/lv2_adapter.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

using namespace pulp;
using namespace pulp::format;
using namespace pulp::format::lv2_adapter;
using Catch::Matchers::ContainsSubstring;

// Helper: build a test descriptor and store without needing a Processor subclass
static PluginDescriptor make_effect_desc() {
    PluginDescriptor desc;
    desc.name = "TestLv2";
    desc.manufacturer = "Pulp";
    desc.bundle_id = "com.pulp.test-lv2";
    desc.version = "1.0.0";
    desc.category = PluginCategory::Effect;
    desc.input_buses = {{"Audio In", 2}};
    desc.output_buses = {{"Audio Out", 2}};
    desc.accepts_midi = false;
    desc.produces_midi = false;
    return desc;
}

static void add_test_params(state::StateStore& store) {
    store.add_parameter({
        .id = 1,
        .name = "Gain",
        .unit = "dB",
        .range = {-60.0f, 24.0f, 0.0f, 0.1f},
    });
    store.add_parameter({
        .id = 2,
        .name = "Mix",
        .unit = "%",
        .range = {0.0f, 100.0f, 100.0f},
    });
}

TEST_CASE("LV2 TTL generation produces valid plugin.ttl", "[format][lv2]") {
    auto desc = make_effect_desc();
    state::StateStore store;
    add_test_params(store);

    auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/test-lv2");

    // Check prefixes
    REQUIRE_THAT(ttl, ContainsSubstring("@prefix lv2:"));
    REQUIRE_THAT(ttl, ContainsSubstring("@prefix doap:"));

    // Check plugin URI
    REQUIRE_THAT(ttl, ContainsSubstring("<http://pulp.audio/plugins/test-lv2>"));

    // Check plugin metadata
    REQUIRE_THAT(ttl, ContainsSubstring("doap:name \"TestLv2\""));
    REQUIRE_THAT(ttl, ContainsSubstring("doap:name \"Pulp\""));

    // Check audio ports (2 in + 2 out = 4 audio ports)
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:AudioPort"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:InputPort"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:OutputPort"));

    // Check control ports for parameters
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:ControlPort"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:name \"Gain\""));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:name \"Mix\""));

    // Check parameter ranges
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:minimum -60"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:maximum 24"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:default 0"));
}

TEST_CASE("LV2 TTL includes MIDI ports for instruments", "[format][lv2]") {
    PluginDescriptor desc;
    desc.name = "TestSynth";
    desc.manufacturer = "Pulp";
    desc.category = PluginCategory::Instrument;
    desc.input_buses = {};
    desc.output_buses = {{"Audio Out", 2}};
    desc.accepts_midi = true;
    desc.produces_midi = false;

    state::StateStore store;
    auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/test-synth");

    // Should have InstrumentPlugin class
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:InstrumentPlugin"));

    // Should have MIDI input atom port
    REQUIRE_THAT(ttl, ContainsSubstring("atom:AtomPort"));
    REQUIRE_THAT(ttl, ContainsSubstring("midi:MidiEvent"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:symbol \"midi_in\""));
}

TEST_CASE("LV2 manifest.ttl generation", "[format][lv2]") {
    auto ttl = generate_manifest_ttl(
        "http://pulp.audio/plugins/test-lv2", "TestLv2.so");

    REQUIRE_THAT(ttl, ContainsSubstring("<http://pulp.audio/plugins/test-lv2>"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:binary <TestLv2.so>"));
    REQUIRE_THAT(ttl, ContainsSubstring("rdfs:seeAlso <TestLv2.ttl>"));
}

TEST_CASE("LV2 TTL port indices are sequential", "[format][lv2]") {
    auto desc = make_effect_desc();
    state::StateStore store;
    add_test_params(store);

    auto ttl = generate_plugin_ttl(desc, store, "http://pulp.audio/plugins/test");

    // 2 audio in + 2 audio out + 2 control = indices 0-5
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 0"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 1"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 2"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 3"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 4"));
    REQUIRE_THAT(ttl, ContainsSubstring("lv2:index 5"));
}
