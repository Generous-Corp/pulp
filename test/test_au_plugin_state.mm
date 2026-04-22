#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>

#import "../core/format/src/au_audio_unit.h"

#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

class TestAUEffectProcessor;
class TestAUInstrumentProcessor;

TestAUEffectProcessor* g_last_effect_processor = nullptr;
TestAUInstrumentProcessor* g_last_instrument_processor = nullptr;

class TestAUEffectProcessor : public pulp::format::Processor {
public:
    TestAUEffectProcessor() { g_last_effect_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "AUEffectPluginStateTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au-effect-plugin-state",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
};

class TestAUInstrumentProcessor : public pulp::format::Processor {
public:
    TestAUInstrumentProcessor() { g_last_instrument_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "AUInstrumentPluginStateTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au-instrument-plugin-state",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Cutoff",
            .unit = "Hz",
            .range = {20.0f, 20000.0f, 440.0f, 1.0f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
};

std::unique_ptr<pulp::format::Processor> create_effect_processor() {
    return std::make_unique<TestAUEffectProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_instrument_processor() {
    return std::make_unique<TestAUInstrumentProcessor>();
}

void require_plst_blob(const uint8_t* bytes, std::size_t size) {
    REQUIRE(size >= 4);
    REQUIRE(bytes[0] == 'P');
    REQUIRE(bytes[1] == 'L');
    REQUIRE(bytes[2] == 'S');
    REQUIRE(bytes[3] == 'T');
}

} // namespace

TEST_CASE("AU v2 effect SaveState/RestoreState round-trips plugin-owned payload",
          "[au][auv2][state]") {
    pulp::format::register_plugin(create_effect_processor);

    pulp::format::au::PulpAUEffect saver(nullptr);
    auto* saver_processor = g_last_effect_processor;
    REQUIRE(saver_processor != nullptr);
    saver_processor->state().set_value(1, -10.5f);
    saver_processor->plugin_state = "layout=48";

    CFPropertyListRef saved = nullptr;
    REQUIRE(saver.SaveState(&saved) == noErr);
    REQUIRE(saved != nullptr);
    REQUIRE(CFGetTypeID(saved) == CFDictionaryGetTypeID());
    auto saved_dict = static_cast<CFDictionaryRef>(saved);
    auto payload = static_cast<CFDataRef>(
        CFDictionaryGetValue(saved_dict, CFSTR("pulp-state")));
    REQUIRE(payload != nullptr);
    require_plst_blob(CFDataGetBytePtr(payload),
                      static_cast<std::size_t>(CFDataGetLength(payload)));

    pulp::format::register_plugin(create_effect_processor);
    pulp::format::au::PulpAUEffect loader(nullptr);
    auto* loader_processor = g_last_effect_processor;
    REQUIRE(loader_processor != nullptr);
    loader_processor->state().set_value(1, 6.0f);
    loader_processor->plugin_state = "stale";

    REQUIRE(loader.RestoreState(saved) == noErr);
    REQUIRE_THAT(loader_processor->state().get_value(1), WithinAbs(-10.5, 0.01));
    REQUIRE(loader_processor->plugin_state == "layout=48");

    CFRelease(saved);
}

TEST_CASE("AU v2 instrument SaveState/RestoreState round-trips plugin-owned payload",
          "[au][auv2][instrument][state]") {
    pulp::format::register_plugin(create_instrument_processor);

    pulp::format::au::PulpAUInstrument saver(nullptr);
    auto* saver_processor = g_last_instrument_processor;
    REQUIRE(saver_processor != nullptr);
    saver_processor->state().set_value(1, 880.0f);
    saver_processor->plugin_state = "snapshot=B";

    CFPropertyListRef saved = nullptr;
    REQUIRE(saver.SaveState(&saved) == noErr);
    REQUIRE(saved != nullptr);
    REQUIRE(CFGetTypeID(saved) == CFDictionaryGetTypeID());
    auto saved_dict = static_cast<CFDictionaryRef>(saved);
    auto payload = static_cast<CFDataRef>(
        CFDictionaryGetValue(saved_dict, CFSTR("pulp-state")));
    REQUIRE(payload != nullptr);
    require_plst_blob(CFDataGetBytePtr(payload),
                      static_cast<std::size_t>(CFDataGetLength(payload)));

    pulp::format::register_plugin(create_instrument_processor);
    pulp::format::au::PulpAUInstrument loader(nullptr);
    auto* loader_processor = g_last_instrument_processor;
    REQUIRE(loader_processor != nullptr);
    loader_processor->state().set_value(1, 220.0f);
    loader_processor->plugin_state = "stale";

    REQUIRE(loader.RestoreState(saved) == noErr);
    REQUIRE_THAT(loader_processor->state().get_value(1), WithinAbs(880.0, 0.01));
    REQUIRE(loader_processor->plugin_state == "snapshot=B");

    CFRelease(saved);
}

TEST_CASE("AU v3 fullState round-trips plugin-owned payload",
          "[au][auv3][state]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        pulp::format::register_plugin(create_effect_processor);

        NSError* saver_error = nil;
        PulpAudioUnit* saver =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&saver_error];
        REQUIRE(saver != nil);
        REQUIRE(saver_error == nil);
        auto* saver_processor = g_last_effect_processor;
        REQUIRE(saver_processor != nullptr);
        saver_processor->state().set_value(1, -14.0f);
        saver_processor->plugin_state = "view=60-12000";

        NSDictionary<NSString*, id>* saved = [saver fullState];
        REQUIRE(saved != nil);
        NSData* payload = saved[@"pulpState"];
        REQUIRE(payload != nil);
        require_plst_blob(static_cast<const uint8_t*>(payload.bytes), payload.length);

        pulp::format::register_plugin(create_effect_processor);

        NSError* loader_error = nil;
        PulpAudioUnit* loader =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&loader_error];
        REQUIRE(loader != nil);
        REQUIRE(loader_error == nil);
        auto* loader_processor = g_last_effect_processor;
        REQUIRE(loader_processor != nullptr);
        loader_processor->state().set_value(1, 3.0f);
        loader_processor->plugin_state = "stale";

        [loader setFullState:saved];
        REQUIRE_THAT(loader_processor->state().get_value(1), WithinAbs(-14.0, 0.01));
        REQUIRE(loader_processor->plugin_state == "view=60-12000");

        [loader release];
        [saver release];
    }
}
