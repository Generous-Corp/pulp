#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/host_type.hpp>
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
                 const pulp::format::ProcessContext&) override {
        ++process_count;
        gain_seen_in_process = state().get_value(1);
        had_param_events = (param_events() != nullptr);
        last_param_events.clear();
        if (auto* events = param_events()) {
            for (const auto& event : *events) last_param_events.push_back(event);
        }
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    std::string plugin_state;
    int process_count = 0;
    float gain_seen_in_process = 0.0f;
    bool had_param_events = false;
    std::vector<pulp::state::ParameterEvent> last_param_events;
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

struct ScopedFactoryRegistration {
    explicit ScopedFactoryRegistration(pulp::format::ProcessorFactory factory)
        : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(factory);
    }

    ~ScopedFactoryRegistration() {
        pulp::format::register_plugin(previous);
    }

    pulp::format::ProcessorFactory previous;
};

} // namespace

TEST_CASE("AU v2 effect SaveState/RestoreState round-trips plugin-owned payload",
          "[au][auv2][state]") {
    ScopedFactoryRegistration registration(create_effect_processor);

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
    ScopedFactoryRegistration registration(create_instrument_processor);

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

        ScopedFactoryRegistration registration(create_effect_processor);

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

TEST_CASE("AU v3 render events preserve parameter sample offsets and update StateStore",
          "[au][auv3][params][rate-model]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_effect_processor);

        NSError* error = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&error];
        REQUIRE(unit != nil);
        REQUIRE(error == nil);

        auto* processor = g_last_effect_processor;
        REQUIRE(processor != nullptr);
        processor->state().set_value(1, 0.0f);

        NSError* allocate_error = nil;
        REQUIRE([unit allocateRenderResourcesAndReturnError:&allocate_error]);
        REQUIRE(allocate_error == nil);

        constexpr UInt32 kFrames = 8;
        float left[kFrames] = {};
        float right[kFrames] = {};
        struct StereoBufferList {
            AudioBufferList list;
            AudioBuffer extra[1];
        } output{};
        output.list.mNumberBuffers = 2;
        output.list.mBuffers[0].mNumberChannels = 1;
        output.list.mBuffers[0].mDataByteSize = kFrames * sizeof(float);
        output.list.mBuffers[0].mData = left;
        output.list.mBuffers[1].mNumberChannels = 1;
        output.list.mBuffers[1].mDataByteSize = kFrames * sizeof(float);
        output.list.mBuffers[1].mData = right;

        AURenderEvent first{};
        first.parameter.eventSampleTime = 101;
        first.parameter.eventType = AURenderEventParameter;
        first.parameter.rampDurationSampleFrames = 0;
        first.parameter.parameterAddress = 1;
        first.parameter.value = -6.0f;

        AURenderEvent second{};
        second.parameter.eventSampleTime = 105;
        second.parameter.eventType = AURenderEventParameterRamp;
        second.parameter.rampDurationSampleFrames = 2;
        second.parameter.parameterAddress = 1;
        second.parameter.value = -12.0f;
        first.head.next = &second;

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = 100;

        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block != nil);
        auto status = block(&flags,
                            &timestamp,
                            kFrames,
                            0,
                            &output.list,
                            &first,
                            nil);
        REQUIRE(status == noErr);

        REQUIRE(processor->process_count == 1);
        REQUIRE_THAT(processor->gain_seen_in_process, WithinAbs(-12.0f, 1e-6f));
        REQUIRE(processor->had_param_events);
        REQUIRE(processor->last_param_events.size() == 2);
        REQUIRE(processor->last_param_events[0].param_id == 1);
        REQUIRE(processor->last_param_events[0].sample_offset == 1);
        REQUIRE(processor->last_param_events[0].ramp_duration_sample_frames == 0);
        REQUIRE_THAT(processor->last_param_events[0].value, WithinAbs(-6.0f, 1e-6f));
        REQUIRE(processor->last_param_events[1].param_id == 1);
        REQUIRE(processor->last_param_events[1].sample_offset == 5);
        REQUIRE(processor->last_param_events[1].ramp_duration_sample_frames == 2);
        REQUIRE_THAT(processor->last_param_events[1].value, WithinAbs(-12.0f, 1e-6f));
        REQUIRE_THAT(processor->state().get_value(1), WithinAbs(-12.0f, 1e-6f));

        REQUIRE([unit pulpLastParameterEventCount] == 2);
        REQUIRE([unit pulpLastParameterEventParamIDAtIndex:0] == 1);
        REQUIRE([unit pulpLastParameterEventSampleOffsetAtIndex:0] == 1);
        REQUIRE([unit pulpLastParameterEventRampDurationAtIndex:0] == 0);
        REQUIRE_THAT([unit pulpLastParameterEventValueAtIndex:0], WithinAbs(-6.0f, 1e-6f));
        REQUIRE([unit pulpLastParameterEventParamIDAtIndex:1] == 1);
        REQUIRE([unit pulpLastParameterEventSampleOffsetAtIndex:1] == 5);
        REQUIRE([unit pulpLastParameterEventRampDurationAtIndex:1] == 2);
        REQUIRE_THAT([unit pulpLastParameterEventValueAtIndex:1], WithinAbs(-12.0f, 1e-6f));

        [unit deallocateRenderResources];
        [unit release];
    }
}

TEST_CASE("AU v3 render block rejects frame counts above maximumFramesToRender",
          "[au][auv3][render][bounds]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_effect_processor);

        NSError* error = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&error];
        REQUIRE(unit != nil);
        REQUIRE(error == nil);

        auto* processor = g_last_effect_processor;
        REQUIRE(processor != nullptr);

        NSError* allocate_error = nil;
        REQUIRE([unit allocateRenderResourcesAndReturnError:&allocate_error]);
        REQUIRE(allocate_error == nil);

        AudioBufferList output{};
        output.mNumberBuffers = 1;
        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block != nil);

        const AUAudioFrameCount too_many = unit.maximumFramesToRender + 1;
        auto status = block(&flags,
                            &timestamp,
                            too_many,
                            0,
                            &output,
                            nullptr,
                            nil);
        REQUIRE(status == kAudioUnitErr_TooManyFramesToProcess);
        REQUIRE(processor->process_count == 0);

        [unit deallocateRenderResources];
        [unit release];
    }
}

TEST_CASE("AU v3 render block substitutes scratch output buffers when host buffers are absent",
          "[au][auv3][render][scratch]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_effect_processor);

        NSError* error = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&error];
        REQUIRE(unit != nil);
        REQUIRE(error == nil);

        auto* processor = g_last_effect_processor;
        REQUIRE(processor != nullptr);

        constexpr UInt32 kFrames = 8;
        float undersized = 0.0f;
        struct StereoBufferList {
            AudioBufferList list;
            AudioBuffer extra[1];
        } output{};
        output.list.mNumberBuffers = 2;
        output.list.mBuffers[0].mNumberChannels = 2;
        output.list.mBuffers[0].mDataByteSize = 0;
        output.list.mBuffers[0].mData = nullptr;
        output.list.mBuffers[1].mNumberChannels = 2;
        output.list.mBuffers[1].mDataByteSize = sizeof(float);
        output.list.mBuffers[1].mData = &undersized;

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block != nil);

        auto status = block(&flags,
                            &timestamp,
                            kFrames,
                            0,
                            &output.list,
                            nullptr,
                            nil);
        REQUIRE(status == noErr);
        REQUIRE(processor->process_count == 1);

        for (UInt32 i = 0; i < output.list.mNumberBuffers; ++i) {
            REQUIRE(output.list.mBuffers[i].mNumberChannels == 1);
            REQUIRE(output.list.mBuffers[i].mDataByteSize == kFrames * sizeof(float));
            REQUIRE(output.list.mBuffers[i].mData != nullptr);
        }
        REQUIRE(output.list.mBuffers[1].mData != &undersized);

        [unit release];
    }
}

TEST_CASE("AU v2 SaveState accepts pre-existing property lists",
          "[au][auv2][state]") {
    SECTION("effect") {
        ScopedFactoryRegistration registration(create_effect_processor);
        pulp::format::au::PulpAUEffect saver(nullptr);
        auto* processor = g_last_effect_processor;
        REQUIRE(processor != nullptr);
        processor->plugin_state = "layout=48";

        CFPropertyListRef saved = CFRetain(CFSTR("stale"));

        REQUIRE(saver.SaveState(&saved) == noErr);
        REQUIRE(saved != nullptr);
        auto saved_dict = static_cast<CFDictionaryRef>(saved);
        auto payload = static_cast<CFDataRef>(
            CFDictionaryGetValue(saved_dict, CFSTR("pulp-state")));
        REQUIRE(payload != nullptr);
        require_plst_blob(CFDataGetBytePtr(payload),
                          static_cast<std::size_t>(CFDataGetLength(payload)));
        CFRelease(saved);
    }

    SECTION("instrument") {
        ScopedFactoryRegistration registration(create_instrument_processor);
        pulp::format::au::PulpAUInstrument saver(nullptr);
        auto* processor = g_last_instrument_processor;
        REQUIRE(processor != nullptr);
        processor->plugin_state = "snapshot=B";

        CFPropertyListRef saved = CFRetain(CFSTR("stale"));

        REQUIRE(saver.SaveState(&saved) == noErr);
        REQUIRE(saved != nullptr);
        auto saved_dict = static_cast<CFDictionaryRef>(saved);
        auto payload = static_cast<CFDataRef>(
            CFDictionaryGetValue(saved_dict, CFSTR("pulp-state")));
        REQUIRE(payload != nullptr);
        require_plst_blob(CFDataGetBytePtr(payload),
                          static_cast<std::size_t>(CFDataGetLength(payload)));
        CFRelease(saved);
    }
}

TEST_CASE("AU v2 state persistence rejects invalid payloads and uninitialized processors",
          "[au][auv2][state]") {
    SECTION("effect rejects invalid payload") {
        ScopedFactoryRegistration registration(create_effect_processor);
        pulp::format::au::PulpAUEffect effect(nullptr);
        auto* processor = g_last_effect_processor;
        REQUIRE(processor != nullptr);
        processor->state().set_value(1, 2.0f);
        processor->plugin_state = "keep";

        const uint8_t bad_bytes[] = {'N', 'O', 'P', 'E'};
        CFDataRef payload = CFDataCreate(kCFAllocatorDefault, bad_bytes, 4);
        CFMutableDictionaryRef state = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(state, CFSTR("pulp-state"), payload);

        REQUIRE(effect.RestoreState(state) == kAudioUnitErr_InvalidPropertyValue);
        REQUIRE_THAT(processor->state().get_value(1), WithinAbs(2.0, 0.01));
        REQUIRE(processor->plugin_state == "keep");

        CFRelease(state);
        CFRelease(payload);
    }

    SECTION("instrument rejects invalid payload") {
        ScopedFactoryRegistration registration(create_instrument_processor);
        pulp::format::au::PulpAUInstrument instrument(nullptr);
        auto* processor = g_last_instrument_processor;
        REQUIRE(processor != nullptr);
        processor->state().set_value(1, 330.0f);
        processor->plugin_state = "keep";

        const uint8_t bad_bytes[] = {'N', 'O', 'P', 'E'};
        CFDataRef payload = CFDataCreate(kCFAllocatorDefault, bad_bytes, 4);
        CFMutableDictionaryRef state = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(state, CFSTR("pulp-state"), payload);

        REQUIRE(instrument.RestoreState(state) == kAudioUnitErr_InvalidPropertyValue);
        REQUIRE_THAT(processor->state().get_value(1), WithinAbs(330.0, 0.01));
        REQUIRE(processor->plugin_state == "keep");

        CFRelease(state);
        CFRelease(payload);
    }

    SECTION("effect reports uninitialized when no factory is registered") {
        CFPropertyListRef saved = nullptr;
        {
            ScopedFactoryRegistration registration(create_effect_processor);
            pulp::format::au::PulpAUEffect saver(nullptr);
            REQUIRE(saver.SaveState(&saved) == noErr);
            REQUIRE(saved != nullptr);
        }

        ScopedFactoryRegistration registration(nullptr);
        pulp::format::au::PulpAUEffect effect(nullptr);

        CFPropertyListRef empty = nullptr;
        REQUIRE(effect.SaveState(&empty) == kAudioUnitErr_Uninitialized);
        if (empty) CFRelease(empty);

        REQUIRE(effect.RestoreState(saved) == kAudioUnitErr_Uninitialized);
        CFRelease(saved);
    }

    SECTION("instrument reports uninitialized when no factory is registered") {
        CFPropertyListRef saved = nullptr;
        {
            ScopedFactoryRegistration registration(create_instrument_processor);
            pulp::format::au::PulpAUInstrument saver(nullptr);
            REQUIRE(saver.SaveState(&saved) == noErr);
            REQUIRE(saved != nullptr);
        }

        ScopedFactoryRegistration registration(nullptr);
        pulp::format::au::PulpAUInstrument instrument(nullptr);

        CFPropertyListRef empty = nullptr;
        REQUIRE(instrument.SaveState(&empty) == kAudioUnitErr_Uninitialized);
        if (empty) CFRelease(empty);

        REQUIRE(instrument.RestoreState(saved) == kAudioUnitErr_Uninitialized);
        CFRelease(saved);
    }
}

TEST_CASE("AU v3 setFullState ignores invalid plugin payload",
          "[au][auv3][state]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_effect_processor);

        NSError* error = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&error];
        REQUIRE(unit != nil);
        REQUIRE(error == nil);

        auto* processor = g_last_effect_processor;
        REQUIRE(processor != nullptr);
        processor->state().set_value(1, 5.0f);
        processor->plugin_state = "keep";

        NSData* payload = [NSData dataWithBytes:"NOPE" length:4];
        NSDictionary<NSString*, id>* saved = @{@"pulpState": payload};
        [unit setFullState:saved];

        REQUIRE_THAT(processor->state().get_value(1), WithinAbs(5.0, 0.01));
        REQUIRE(processor->plugin_state == "keep");

        [unit release];
    }
}

// ── Item 3.1 — AU v3 dual-tracked bypass ───────────────────────────────────
//
// Pins three contract points:
//   * AUv3 init auto-detects a "Bypass" parameter and routes
//     `shouldBypassEffect` / `setShouldBypassEffect:` to it.
//   * `setShouldBypassEffect:` writes the plugin's StateStore param.
//   * The render block short-circuits to pass-through audio when
//     bypassed (in→out for effects), and never calls Processor::process.
namespace {

class TestAUBypassProcessor : public pulp::format::Processor {
public:
    TestAUBypassProcessor() { g_last = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "AUBypassEffectTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au-bypass",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 7,
            .name = "Gain",
            .unit = "dB",
            .range = {-60.0f, 24.0f, 0.0f, 0.1f},
        });
        store.add_parameter({
            .id = 9,
            .name = "Bypass",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        ++process_count;
        // Fill output with a sentinel value so a buggy bypass path
        // (one that called process() anyway) would be observable.
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            float* dst = out.channel_ptr(ch);
            for (std::size_t i = 0; i < out.num_samples(); ++i) {
                dst[i] = -99.0f;
            }
        }
        (void)in;
    }

    int process_count = 0;
    static TestAUBypassProcessor* g_last;
};

TestAUBypassProcessor* TestAUBypassProcessor::g_last = nullptr;

std::unique_ptr<pulp::format::Processor> create_bypass_processor() {
    return std::make_unique<TestAUBypassProcessor>();
}

} // namespace

TEST_CASE("AU v3 auto-detects plugin-declared Bypass parameter",
          "[au][auv3][bypass][item-3.1]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TBpE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_bypass_processor);

        NSError* err = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&err];
        REQUIRE(unit != nil);
        REQUIRE(err == nil);

        // The Bypass parameter was registered with id 9 — assert the
        // adapter routed shouldBypassEffect to it instead of falling
        // back to the bridge-local atomic.
        REQUIRE([unit pulpBypassParameterId] == 9u);
        REQUIRE([unit shouldBypassEffect] == NO);

        // Host writes through the AU surface — the StateStore parameter
        // must update so plugin-side UI bindings stay coherent.
        [unit setShouldBypassEffect:YES];
        auto* processor = TestAUBypassProcessor::g_last;
        REQUIRE(processor != nullptr);
        REQUIRE_THAT(processor->state().get_value(9), WithinAbs(1.0f, 1e-6f));
        REQUIRE([unit shouldBypassEffect] == YES);

        // Plugin-side write reflects back to the AU surface.
        processor->state().set_value(9, 0.0f);
        REQUIRE([unit shouldBypassEffect] == NO);

        [unit release];
    }
}

TEST_CASE("AU v3 render block short-circuits to pass-through when bypassed",
          "[au][auv3][bypass][item-3.1]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TBpE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_bypass_processor);

        NSError* err = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&err];
        REQUIRE(unit != nil);
        REQUIRE(err == nil);

        auto* processor = TestAUBypassProcessor::g_last;
        REQUIRE(processor != nullptr);

        NSError* allocate_error = nil;
        REQUIRE([unit allocateRenderResourcesAndReturnError:&allocate_error]);
        REQUIRE(allocate_error == nil);

        constexpr UInt32 kFrames = 4;
        float left[kFrames] = {1.0f, 2.0f, 3.0f, 4.0f};
        float right[kFrames] = {5.0f, 6.0f, 7.0f, 8.0f};
        struct StereoBufferList {
            AudioBufferList list;
            AudioBuffer extra[1];
        } output{};
        output.list.mNumberBuffers = 2;
        output.list.mBuffers[0].mNumberChannels = 1;
        output.list.mBuffers[0].mDataByteSize = kFrames * sizeof(float);
        output.list.mBuffers[0].mData = left;
        output.list.mBuffers[1].mNumberChannels = 1;
        output.list.mBuffers[1].mDataByteSize = kFrames * sizeof(float);
        output.list.mBuffers[1].mData = right;

        // Pull block returns simple input so the bypass copy has
        // something concrete to assert against.
        AURenderPullInputBlock pull = ^AUAudioUnitStatus(
            AudioUnitRenderActionFlags* /*pullFlags*/,
            const AudioTimeStamp* /*ts*/,
            AUAudioFrameCount /*nframes*/,
            NSInteger /*busNumber*/,
            AudioBufferList* inputData) {
            // Fill each pulled channel with a per-channel sentinel.
            for (UInt32 b = 0; b < inputData->mNumberBuffers; ++b) {
                float* ptr = static_cast<float*>(inputData->mBuffers[b].mData);
                const UInt32 n = inputData->mBuffers[b].mDataByteSize / sizeof(float);
                for (UInt32 i = 0; i < n; ++i) {
                    ptr[i] = static_cast<float>(10 + b);
                }
            }
            return noErr;
        };

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = 0;

        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block != nil);

        // Bypass on.
        [unit setShouldBypassEffect:YES];
        const int before = processor->process_count;
        auto status = block(&flags, &timestamp, kFrames, 0,
                            &output.list, nullptr, pull);
        REQUIRE(status == noErr);
        // Processor::process must NOT have been called when bypassed.
        REQUIRE(processor->process_count == before);
        // Output channels must hold the pulled input verbatim (10.0 on
        // channel 0, 11.0 on channel 1) — confirms in→out pass-through,
        // not the -99.0 sentinel the processor would have written.
        for (UInt32 i = 0; i < kFrames; ++i) {
            REQUIRE_THAT(left[i],  WithinAbs(10.0f, 1e-6f));
            REQUIRE_THAT(right[i], WithinAbs(11.0f, 1e-6f));
        }

        // Bypass off — the processor must run and stamp its sentinel.
        [unit setShouldBypassEffect:NO];
        status = block(&flags, &timestamp, kFrames, 0,
                       &output.list, nullptr, pull);
        REQUIRE(status == noErr);
        REQUIRE(processor->process_count == before + 1);
        REQUIRE_THAT(left[0],  WithinAbs(-99.0f, 1e-6f));
        REQUIRE_THAT(right[0], WithinAbs(-99.0f, 1e-6f));

        [unit deallocateRenderResources];
        [unit release];
    }
}

TEST_CASE("AU v3 without a Bypass parameter still honours setShouldBypassEffect",
          "[au][auv3][bypass][item-3.1]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        // Use the existing effect processor — it has no Bypass param.
        ScopedFactoryRegistration registration(create_effect_processor);

        NSError* err = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&err];
        REQUIRE(unit != nil);
        REQUIRE(err == nil);

        // No Bypass parameter — the bridge falls back to its atomic.
        REQUIRE([unit pulpBypassParameterId] == 0u);
        REQUIRE([unit shouldBypassEffect] == NO);

        [unit setShouldBypassEffect:YES];
        REQUIRE([unit shouldBypassEffect] == YES);
        [unit setShouldBypassEffect:NO];
        REQUIRE([unit shouldBypassEffect] == NO);

        [unit release];
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Per-method audit invariants (macOS plan item 3.1)
//
// Pins the no-op-by-design surface of `PulpAudioUnit` so regressions to
// the table in the `au_adapter.mm` header don't slip through silently.
// Update both this test AND the header-comment table when you change the
// audit row for an override.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("AU v3 per-method audit invariants",
          "[au][auv3][audit][item-3.1]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_effect_processor);

        NSError* err = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&err];
        REQUIRE(unit != nil);
        REQUIRE(err == nil);

        // No-op invariants — these must NOT change without an audit-table
        // update. Pinning them prevents a future edit from silently
        // flipping the bus model or preset support, which would change
        // what hosts assume about Pulp plugins at scan time.
        REQUIRE([unit supportsUserPresets] == NO);
        REQUIRE([unit canProcessInPlace] == YES);

        // Bus topology — effect processor declares 1 input bus + 1 output
        // bus, no sidechain. The adapter must mirror that 1:1 so the host
        // doesn't see a phantom sidechain it can't connect.
        REQUIRE([unit outputBusses] != nil);
        REQUIRE([unit outputBusses].count == 1u);
        REQUIRE([unit inputBusses] != nil);
        REQUIRE([unit inputBusses].count == 1u);

        // No latency / tail by default for the test processor. The
        // adapter must NOT report a host-meaningful latency for a
        // zero-latency plugin — Logic stacks reported latencies as
        // PDC budget and will under-report a track that lies.
        REQUIRE(unit.latency == 0.0);
        REQUIRE(unit.tailTime == 0.0);

        // ParameterTree must be cached and non-empty — the test processor
        // exposes one parameter. Recreating the tree on every observer
        // call would allocate on every host parameter scan.
        AUParameterTree* tree1 = unit.parameterTree;
        REQUIRE(tree1 != nil);
        REQUIRE(tree1.allParameters.count == 1u);

        // pulpProcessor + pulpStore — same Processor + Store the audio
        // path will see. Tests rely on this to avoid the dual-Processor
        // drift bug the AU v2 path used to hit.
        REQUIRE([unit pulpProcessor] != nullptr);
        REQUIRE([unit pulpStore] != nullptr);

        [unit release];
    }
}

// ─────────────────────────────────────────────────────────────────────────
// AUHostingService host detection (DAW-quirks row 22, macOS plan item 3.1)
//
// Pins the wrapper-identifier → HostType classifier. The real-host path
// is exercised by running Pulp inside Logic / GarageBand / AUM and is
// validated separately (deferred to the in-DAW bench rows for those
// hosts). These cases pin the deterministic classifier output that the
// adapter consumes when `current_auv3_wrapper_identifier()` resolves.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("AUHostingService wrapper-id classifier — known wrappers",
          "[au][auv3][host-detection][item-3.1]") {
    using pulp::format::host_type_from_auv3_wrapper;
    using pulp::format::HostType;

    SECTION("Logic Pro bundle id") {
        REQUIRE(host_type_from_auv3_wrapper("com.apple.logic10") == HostType::LogicPro);
        REQUIRE(host_type_from_auv3_wrapper("com.apple.logic.pro") == HostType::LogicPro);
        // Case-insensitive.
        REQUIRE(host_type_from_auv3_wrapper("COM.APPLE.LOGIC10") == HostType::LogicPro);
    }

    SECTION("MainStage folds into Logic family") {
        REQUIRE(host_type_from_auv3_wrapper("com.apple.mainstage") == HostType::LogicPro);
    }

    SECTION("Logic 10.5+ AUHostingServiceXPC wrapper name") {
        REQUIRE(host_type_from_auv3_wrapper("AUHostingServiceXPC_arrow") == HostType::LogicPro);
    }

    SECTION("GarageBand bundle id") {
        REQUIRE(host_type_from_auv3_wrapper("com.apple.garageband10") == HostType::GarageBand);
    }

    SECTION("Reuses executable-path classifier for non-Apple hosts") {
        // Reaper, Cubase, etc. when the wrapper advertises a bundle id
        // we already classify via the executable-name heuristic.
        REQUIRE(host_type_from_auv3_wrapper("com.cockos.reaper") == HostType::Reaper);
        REQUIRE(host_type_from_auv3_wrapper("com.steinberg.cubase13") == HostType::Cubase);
        REQUIRE(host_type_from_auv3_wrapper("com.ableton.live") == HostType::AbletonLive);
    }
}

TEST_CASE("AUHostingService wrapper-id classifier — unknown / fallback",
          "[au][auv3][host-detection][item-3.1]") {
    using pulp::format::host_type_from_auv3_wrapper;
    using pulp::format::HostType;

    SECTION("Empty input returns Unknown") {
        REQUIRE(host_type_from_auv3_wrapper("") == HostType::Unknown);
    }

    SECTION("Generic AUHostingService — caller must fall back") {
        // Bare `AUHostingService` is Apple's generic wrapper used by
        // many hosts. The classifier returns Unknown so the caller
        // falls back to the executable-path heuristic instead of
        // mis-classifying the host.
        REQUIRE(host_type_from_auv3_wrapper("AUHostingService") == HostType::Unknown);
    }

    SECTION("Unknown bundle id returns Unknown") {
        REQUIRE(host_type_from_auv3_wrapper("com.example.unknown") == HostType::Unknown);
    }
}

TEST_CASE("current_auv3_wrapper_identifier — Apple bundle inspection",
          "[au][auv3][host-detection][item-3.1]") {
    // The wrapper-id lookup must not crash and must return a string
    // (possibly empty) on Apple platforms. When the test binary runs
    // standalone (no `.appex` bundle), the result is typically the test
    // binary's own bundle id or empty — both are valid outcomes. We pin
    // the no-crash contract here; the real-host classification path is
    // exercised inside Logic / GarageBand / AUM (deferred to per-host
    // bench rows).
    auto id = pulp::format::current_auv3_wrapper_identifier();
    // The std::string contract: callable, returns by value, no throw.
    // The actual content depends on how the test binary is bundled —
    // standalone Catch2 binaries usually return an empty string.
    REQUIRE((id.empty() || !id.empty()));  // tautology — pins no-throw.
}

// Regression: #2967 / Codex comment 3305508749 — main_is_extension used to
// test `[bundleIdentifier hasSuffix:@".appex"]`, but bundle IDENTIFIERS are
// reverse-DNS strings and never carry `.appex`. The check was always false
// inside real AUv3 extension processes, so detect_host_type() leaked the
// extension's own bundle id as the host id, breaking host classification.
// The fix moved the check to the bundle PATH (`bundlePath`, the on-disk
// path that DOES end in `.appex` for an extension bundle).
//
// We can't fake NSBundle in a unit test, but we can pin the preferred
// AU_HOST_BUNDLE_ID env-var channel always wins regardless of bundle
// state — that protects the documented override contract that downstream
// hosts rely on.
TEST_CASE("current_auv3_wrapper_identifier — AU_HOST_BUNDLE_ID env wins (#2967)",
          "[au][auv3][host-detection][issue-2967]") {
    const char* prior = std::getenv("AU_HOST_BUNDLE_ID");
    setenv("AU_HOST_BUNDLE_ID", "com.test.fake-host-2967", /*overwrite=*/1);
    REQUIRE(pulp::format::current_auv3_wrapper_identifier() ==
            "com.test.fake-host-2967");
    if (prior) {
        setenv("AU_HOST_BUNDLE_ID", prior, /*overwrite=*/1);
    } else {
        unsetenv("AU_HOST_BUNDLE_ID");
    }

    // When no env var is set and the test binary is NOT an .appex
    // bundle, the function may legitimately return the binary's own
    // bundle id (in-process load path). The previous bug was that an
    // ACTUAL .appex extension would also wrongly fall into this branch.
    // The bundle-path-based fix can be verified by inspection; this
    // test confirms the env-var override the fix preserves.
}

TEST_CASE("detect_host_type — wrapper id wins when wrapper is recognised",
          "[au][auv3][host-detection][item-3.1]") {
    // `detect_host_type()` consults `current_auv3_wrapper_identifier()`
    // first on Apple platforms; when that returns a recognised id, the
    // wrapper-side wins. When unrecognised (or empty), it falls back to
    // the executable-path heuristic. The classifier-level pieces are
    // pinned above; this test only checks that `detect_host_type()` is
    // callable and returns a value in the enum (i.e. the wrapper path
    // doesn't crash when the Apple host services are absent).
    auto type = pulp::format::detect_host_type();
    REQUIRE(static_cast<int>(type) >= static_cast<int>(pulp::format::HostType::Unknown));
    REQUIRE(static_cast<int>(type) <= static_cast<int>(pulp::format::HostType::Other));
}
