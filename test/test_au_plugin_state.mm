#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/format/au_v2_adapter.hpp>
#include <pulp/format/au_v2_instrument.hpp>
#include <pulp/format/audio_workgroup_client.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/host_type.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#import "../core/format/src/au_audio_unit.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <cmath>

using Catch::Matchers::WithinAbs;

namespace {

class TestAUEffectProcessor;
class TestAUInstrumentProcessor;
class TestAUMpeInstrumentProcessor;

TestAUEffectProcessor* g_last_effect_processor = nullptr;
TestAUInstrumentProcessor* g_last_instrument_processor = nullptr;
TestAUMpeInstrumentProcessor* g_last_mpe_processor = nullptr;
int g_pending_au_latency_samples = 0;
int g_pending_au_tail_samples = 0;
// Toggles whether TestAUMpeInstrumentProcessor declares supports_mpe. Lets one
// capture processor cover both the opt-in and the non-opt-in (null mpe_input)
// paths.
bool g_au_mpe_declare = true;

class TestAUEffectProcessor : public pulp::format::Processor,
                              public pulp::format::AudioWorkgroupClient {
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
                 const pulp::format::ProcessContext& context) override {
        ++process_count;
        last_context = context;
        gain_seen_in_process = state().get_value(1);
        had_param_events = (param_events() != nullptr);
        last_param_events.clear();
        if (auto* events = param_events()) {
            for (const auto& event : *events) last_param_events.push_back(event);
        }
    }

    void process(pulp::format::ProcessBuffers& audio,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override {
        ++process_buffers_count;
        last_process_buffers_layout_ok = audio.layouts_match_descriptors();
        last_process_buffers_storage_ok = audio.active_buses_have_storage();
        last_process_buffers_input_count = audio.inputs.active_count();
        last_process_buffers_output_count = audio.outputs.active_count();
        last_process_buffers_main_input_channels =
            audio.main_input() ? audio.main_input()->num_channels() : 0;
        last_process_buffers_main_output_channels =
            audio.main_output() ? audio.main_output()->num_channels() : 0;
        last_process_buffers_sidechain_channels =
            audio.sidechain_input() ? audio.sidechain_input()->num_channels() : 0;
        last_process_buffers_sidechain_first_sample =
            (audio.sidechain_input() && audio.sidechain_input()->num_channels() > 0 &&
             audio.sidechain_input()->num_samples() > 0)
                ? audio.sidechain_input()->channel(0)[0]
                : 0.0f;

        auto* output = audio.main_output();
        pulp::audio::BufferView<const float> empty_input;
        auto* input = audio.main_input();
        if (output) {
            process(*output, input ? *input : empty_input, midi_in, midi_out, context);
        }
    }

    std::vector<uint8_t> serialize_plugin_state() const override {
        return std::vector<uint8_t>(plugin_state.begin(), plugin_state.end());
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    void set_audio_workgroup(void* value) noexcept override {
        audio_workgroup.store(value, std::memory_order_release);
        audio_workgroup_updates.fetch_add(1, std::memory_order_relaxed);
    }

    void wait_for_audio_workgroup_update() noexcept override {
        audio_workgroup_waits.fetch_add(1, std::memory_order_relaxed);
    }

    bool prepare_audio_workgroup_for_render() noexcept override {
        audio_workgroup_prepares.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::string plugin_state;
    int process_count = 0;
    int process_buffers_count = 0;
    pulp::format::ProcessContext last_context{};
    float gain_seen_in_process = 0.0f;
    bool had_param_events = false;
    bool last_process_buffers_layout_ok = false;
    bool last_process_buffers_storage_ok = false;
    std::size_t last_process_buffers_input_count = 0;
    std::size_t last_process_buffers_output_count = 0;
    std::size_t last_process_buffers_main_input_channels = 0;
    std::size_t last_process_buffers_main_output_channels = 0;
    std::size_t last_process_buffers_sidechain_channels = 0;
    float last_process_buffers_sidechain_first_sample = 0.0f;
    std::vector<pulp::state::ParameterEvent> last_param_events;
    std::atomic<void*> audio_workgroup{nullptr};
    std::atomic<int> audio_workgroup_updates{0};
    std::atomic<int> audio_workgroup_waits{0};
    std::atomic<int> audio_workgroup_prepares{0};
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
                 const pulp::format::ProcessContext& context) override {
        last_context = context;
    }

    void process(pulp::format::ProcessBuffers& audio,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& context) override {
        ++process_buffers_count;
        last_process_buffers_layout_ok = audio.layouts_match_descriptors();
        last_process_buffers_storage_ok = audio.active_buses_have_storage();
        last_process_buffers_input_count = audio.inputs.active_count();
        last_process_buffers_output_count = audio.outputs.active_count();
        last_process_buffers_main_output_channels =
            audio.main_output() ? audio.main_output()->num_channels() : 0;

        auto* output = audio.main_output();
        pulp::audio::BufferView<const float> empty_input;
        if (output) {
            process(*output, empty_input, midi_in, midi_out, context);
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
    int process_buffers_count = 0;
    pulp::format::ProcessContext last_context{};
    bool last_process_buffers_layout_ok = false;
    bool last_process_buffers_storage_ok = false;
    std::size_t last_process_buffers_input_count = 0;
    std::size_t last_process_buffers_output_count = 0;
    std::size_t last_process_buffers_main_output_channels = 0;
};

// One captured per-note expression event, flattened from MpeExpressionEvent so
// the test can assert routing without holding onto the buffer's storage.
struct CapturedMpeEvent {
    pulp::midi::MpeExpressionEvent::Kind kind;
    uint8_t channel = 0;
    uint8_t note = 0;
    float pitch_bend_semitones = 0.0f;
    float pressure = 0.0f;
    float timbre = 0.0f;
    int32_t sample_offset = 0;
};

// MPE-declaring instrument for the AU v3 MPE-routing test. Snapshots the
// per-note expression buffer the render block hands it via mpe_input() each
// block, so the test can assert the adapter ran the MpeVoiceTracker over the
// channel-wide MIDI it fed and attached the resulting per-note events.
class TestAUMpeInstrumentProcessor : public pulp::format::Processor {
public:
    TestAUMpeInstrumentProcessor() { g_last_mpe_processor = this; }

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d{
            .name = "AUMpeInstrumentTest",
            .manufacturer = "PulpTest",
            .bundle_id = "com.pulp.test.au-mpe-instrument",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Instrument,
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
        };
        d.supports_mpe = g_au_mpe_declare;
        return d;
    }

    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    void process(pulp::format::ProcessBuffers&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        ++process_count;
        captured.clear();
        const pulp::midi::MpeBuffer* mpe = mpe_input();
        mpe_input_present = (mpe != nullptr);
        if (mpe) {
            for (const auto& e : *mpe) {
                captured.push_back({e.kind, e.state.channel, e.state.note,
                                    e.state.pitch_bend_semitones, e.state.pressure,
                                    e.state.timbre, e.sample_offset});
            }
        }
    }

    int process_count = 0;
    bool mpe_input_present = false;
    std::vector<CapturedMpeEvent> captured;
};

class TestAUWideEditorProcessor : public TestAUEffectProcessor {
public:
    pulp::format::ViewSize view_size() const override {
        return pulp::format::view_size_from_design(900, 520);
    }
};

class TestAULayoutProcessor : public TestAUEffectProcessor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        auto d = TestAUEffectProcessor::descriptor();
        d.supported_bus_layouts = {
            {.inputs = {2}, .outputs = {2}, .name = "Stereo"},
            {.inputs = {1}, .outputs = {1}, .name = "Mono"},
        };
        return d;
    }
};

class TestAUSidechainEffectProcessor : public TestAUEffectProcessor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        auto d = TestAUEffectProcessor::descriptor();
        d.input_buses = {{"Audio In", 2}, {"Sidechain", 1, true}};
        return d;
    }
};

class TestAULatencyTailEffectProcessor : public TestAUEffectProcessor {
public:
    TestAULatencyTailEffectProcessor()
        : latency_samples_(g_pending_au_latency_samples)
        , tail_samples_(g_pending_au_tail_samples) {}

    pulp::format::PluginDescriptor descriptor() const override {
        auto d = TestAUEffectProcessor::descriptor();
        d.tail_samples = tail_samples_;
        return d;
    }

    int latency_samples() const override { return latency_samples_; }

private:
    int latency_samples_ = 0;
    int tail_samples_ = 0;
};

class TestAULatencyTailInstrumentProcessor : public TestAUInstrumentProcessor {
public:
    TestAULatencyTailInstrumentProcessor()
        : latency_samples_(g_pending_au_latency_samples)
        , tail_samples_(g_pending_au_tail_samples) {}

    pulp::format::PluginDescriptor descriptor() const override {
        auto d = TestAUInstrumentProcessor::descriptor();
        d.tail_samples = tail_samples_;
        return d;
    }

    int latency_samples() const override { return latency_samples_; }

private:
    int latency_samples_ = 0;
    int tail_samples_ = 0;
};

std::unique_ptr<pulp::format::Processor> create_effect_processor() {
    return std::make_unique<TestAUEffectProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_layout_processor() {
    return std::make_unique<TestAULayoutProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_instrument_processor() {
    return std::make_unique<TestAUInstrumentProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_wide_editor_processor() {
    return std::make_unique<TestAUWideEditorProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_mpe_instrument_processor() {
    return std::make_unique<TestAUMpeInstrumentProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_sidechain_effect_processor() {
    return std::make_unique<TestAUSidechainEffectProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_latency_tail_effect_processor() {
    return std::make_unique<TestAULatencyTailEffectProcessor>();
}

std::unique_ptr<pulp::format::Processor> create_latency_tail_instrument_processor() {
    return std::make_unique<TestAULatencyTailInstrumentProcessor>();
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

struct AUv2TransportCallbackState {
    double beat = 0.0;
    double tempo = 120.0;
    UInt32 time_sig_denominator = 4;
    double measure_downbeat = 0.0;
    Boolean is_playing = true;
    Float64 sample_position = 0.0;
};

OSStatus auv2_test_beat_and_tempo(void* user_data,
                                  Float64* out_beat,
                                  Float64* out_tempo) {
    auto* state = static_cast<AUv2TransportCallbackState*>(user_data);
    if (out_beat) *out_beat = state->beat;
    if (out_tempo) *out_tempo = state->tempo;
    return noErr;
}

OSStatus auv2_test_musical_time(void* user_data,
                                UInt32* out_delta_samples,
                                Float32* out_time_sig_numerator,
                                UInt32* out_time_sig_denominator,
                                Float64* out_measure_downbeat) {
    auto* state = static_cast<AUv2TransportCallbackState*>(user_data);
    if (out_delta_samples) *out_delta_samples = 0;
    if (out_time_sig_numerator) *out_time_sig_numerator = 4.0f;
    if (out_time_sig_denominator) {
        *out_time_sig_denominator = state->time_sig_denominator;
    }
    if (out_measure_downbeat) *out_measure_downbeat = state->measure_downbeat;
    return noErr;
}

OSStatus auv2_test_transport_state(void* user_data,
                                   Boolean* out_is_playing,
                                   Boolean* out_transport_state_changed,
                                   Float64* out_current_sample,
                                   Boolean* out_is_cycling,
                                   Float64* out_cycle_start,
                                   Float64* out_cycle_end) {
    auto* state = static_cast<AUv2TransportCallbackState*>(user_data);
    if (out_is_playing) *out_is_playing = state->is_playing;
    if (out_transport_state_changed) *out_transport_state_changed = false;
    if (out_current_sample) *out_current_sample = state->sample_position;
    if (out_is_cycling) *out_is_cycling = false;
    if (out_cycle_start) *out_cycle_start = 0.0;
    if (out_cycle_end) *out_cycle_end = 0.0;
    return noErr;
}

} // namespace

TEST_CASE("AU adapters retire editor owner tokens before processor/store teardown",
          "[au][editor][crash][owner-lifetime][lifecycle]") {
    ScopedFactoryRegistration registration(create_effect_processor);

    SECTION("AU v2 Cocoa editor context") {
        pulp::runtime::AliveToken::Handle owner_alive;
        {
            pulp::format::au::PulpAUEffect effect(nullptr);
            pulp::format::au::PulpEditorContext ctx{};
            REQUIRE(effect.GetProperty(pulp::format::au::kPulpEditorContextProperty,
                                       kAudioUnitScope_Global, 0, &ctx) == noErr);
            REQUIRE(ctx.processor != nullptr);
            REQUIRE(ctx.store != nullptr);
            owner_alive = ctx.owner_alive;
            REQUIRE(pulp::runtime::AliveToken::is_alive(owner_alive));
        }
        REQUIRE_FALSE(pulp::runtime::AliveToken::is_alive(owner_alive));
    }

    SECTION("AU v3 retained view-controller handle") {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';
        NSError* error = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&error];
        REQUIRE(unit != nil);
        REQUIRE(error == nil);
        auto owner_alive = [unit pulpOwnerAlive];
        REQUIRE(pulp::runtime::AliveToken::is_alive(owner_alive));
        [unit release];
        REQUIRE_FALSE(pulp::runtime::AliveToken::is_alive(owner_alive));
    }
}

TEST_CASE("AU adapters survive repeated allocate render deallocate destroy cycles",
          "[au][lifecycle][churn]") {
    ScopedFactoryRegistration registration(create_effect_processor);

    SECTION("AU v2 initialize and cleanup") {
        constexpr UInt32 kFrames = 64;
        AudioStreamBasicDescription format{};
        format.mSampleRate = 48000.0;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                              kAudioFormatFlagIsNonInterleaved;
        format.mBytesPerPacket = sizeof(float);
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = sizeof(float);
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 32;

        struct StereoBufferList {
            AudioBufferList list;
            AudioBuffer extra[1];
        };
        for (int cycle = 0; cycle < 200; ++cycle) {
            pulp::format::au::PulpAUEffect effect(nullptr);
            // Direct construction bypasses AUBase's component dispatch, so
            // mirror the SDK host sequence explicitly before rendering.
            effect.CreateElements();
            REQUIRE(effect.Input(0).SetStreamFormat(format) == noErr);
            REQUIRE(effect.Output(0).SetStreamFormat(format) == noErr);
            UInt32 max_frames = kFrames;
            REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice,
                                               kAudioUnitScope_Global, 0,
                                               &max_frames, sizeof(max_frames)) == noErr);
            REQUIRE(effect.DoInitialize() == noErr);

            float in_l[kFrames] = {};
            float in_r[kFrames] = {};
            float out_l[kFrames] = {};
            float out_r[kFrames] = {};
            StereoBufferList input{};
            input.list.mNumberBuffers = 2;
            input.list.mBuffers[0] = {1, kFrames * sizeof(float), in_l};
            input.list.mBuffers[1] = {1, kFrames * sizeof(float), in_r};
            StereoBufferList output{};
            output.list.mNumberBuffers = 2;
            output.list.mBuffers[0] = {1, kFrames * sizeof(float), out_l};
            output.list.mBuffers[1] = {1, kFrames * sizeof(float), out_r};
            AudioUnitRenderActionFlags flags = 0;
            const UInt32 frames = cycle % 2 == 0 ? 32 : kFrames;
            REQUIRE(effect.ProcessBufferLists(
                        flags, input.list, output.list, frames) == noErr);
            effect.DoCleanup();
        }
    }

    SECTION("AU v3 allocate, render, deallocate, release") {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';
        constexpr UInt32 kFrames = 64;

        for (int cycle = 0; cycle < 200; ++cycle) {
            @autoreleasepool {
                NSError* error = nil;
                PulpAudioUnit* unit =
                    [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                               options:0
                                                                 error:&error];
                REQUIRE(unit != nil);
                REQUIRE(error == nil);
                unit.maximumFramesToRender = kFrames;
                NSError* allocate_error = nil;
                REQUIRE([unit allocateRenderResourcesAndReturnError:&allocate_error]);
                REQUIRE(allocate_error == nil);

                float left[kFrames] = {};
                float right[kFrames] = {};
                struct StereoBufferList {
                    AudioBufferList list;
                    AudioBuffer extra[1];
                } output{};
                output.list.mNumberBuffers = 2;
                const UInt32 frames = cycle % 2 == 0 ? 32 : kFrames;
                const auto bytes = static_cast<UInt32>(frames * sizeof(float));
                output.list.mBuffers[0] = {1, bytes, left};
                output.list.mBuffers[1] = {1, bytes, right};

                AudioUnitRenderActionFlags flags = 0;
                AudioTimeStamp timestamp{};
                timestamp.mFlags = kAudioTimeStampSampleTimeValid;
                timestamp.mSampleTime = cycle * kFrames;
                AUInternalRenderBlock block = [unit internalRenderBlock];
                REQUIRE(block != nil);
                REQUIRE(block(&flags, &timestamp, frames, 0, &output.list, nil, nil) == noErr);

                [unit deallocateRenderResources];
                [unit release];
            }
        }
    }
}

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

TEST_CASE("AU v2 lifecycle churn preserves byte-equivalent plugin state",
          "[au][auv2][lifecycle][churn][state]") {
    ScopedFactoryRegistration registration(create_effect_processor);

    for (int cycle = 0; cycle < 200; ++cycle) {
        const float saved_gain = -24.0f + static_cast<float>(cycle % 48);
        const std::string saved_payload = "cycle=" + std::to_string(cycle);
        CFPropertyListRef saved = nullptr;

        {
            pulp::format::au::PulpAUEffect saver(nullptr);
            auto* saver_processor = g_last_effect_processor;
            REQUIRE(saver_processor != nullptr);
            saver_processor->state().set_value(1, saved_gain);
            saver_processor->plugin_state = saved_payload;
            REQUIRE(saver.SaveState(&saved) == noErr);
            REQUIRE(saved != nullptr);

            // Positive control for the byte-equivalence instrument: repeated
            // saves from the same live instance must compare equal.
            CFPropertyListRef control_saved = nullptr;
            REQUIRE(saver.SaveState(&control_saved) == noErr);
            REQUIRE(control_saved != nullptr);
            REQUIRE(CFEqual(saved, control_saved));
            CFRelease(control_saved);
        }

        {
            pulp::format::au::PulpAUEffect loader(nullptr);
            auto* loader_processor = g_last_effect_processor;
            REQUIRE(loader_processor != nullptr);
            REQUIRE_THAT(loader_processor->state().get_value(1), WithinAbs(0.0, 0.01));
            REQUIRE(loader_processor->plugin_state.empty());
            REQUIRE(loader.RestoreState(saved) == noErr);
            REQUIRE_THAT(loader_processor->state().get_value(1),
                         WithinAbs(saved_gain, 0.01));
            REQUIRE(loader_processor->plugin_state == saved_payload);

            CFPropertyListRef resaved = nullptr;
            REQUIRE(loader.SaveState(&resaved) == noErr);
            REQUIRE(resaved != nullptr);
            const auto saved_dict = static_cast<CFDictionaryRef>(saved);
            const auto resaved_dict = static_cast<CFDictionaryRef>(resaved);
            const auto saved_pulp = static_cast<CFDataRef>(
                CFDictionaryGetValue(saved_dict, CFSTR("pulp-state")));
            const auto resaved_pulp = static_cast<CFDataRef>(
                CFDictionaryGetValue(resaved_dict, CFSTR("pulp-state")));
            REQUIRE(saved_pulp != nullptr);
            REQUIRE(resaved_pulp != nullptr);
            // AUBase contributes instance metadata that can legitimately
            // differ after destroy/recreate. The Pulp-owned PLST blob is the
            // byte-equivalence contract this adapter controls.
            REQUIRE(CFEqual(saved_pulp, resaved_pulp));
            CFRelease(resaved);
        }

        CFRelease(saved);
    }
}

TEST_CASE("AU v2 latency and tail report processor runtime contract",
          "[au][auv2][latency][tail]") {
    constexpr int kTailSamples = 24000;

    REQUIRE_THAT(pulp::format::au::tail_samples_to_seconds(kTailSamples, 48000.0),
                 WithinAbs(0.5, 1e-9));
    REQUIRE(pulp::format::au::tail_samples_to_seconds(0, 48000.0) == 0.0);
    REQUIRE(pulp::format::au::tail_samples_to_seconds(kTailSamples, 0.0) == 0.0);

    SECTION("effect and instrument map infinite tail to infinity") {
        g_pending_au_latency_samples = 0;
        g_pending_au_tail_samples = -1;
        {
            ScopedFactoryRegistration registration(create_latency_tail_effect_processor);
            pulp::format::au::PulpAUEffect effect(nullptr);
            REQUIRE(effect.SupportsTail());
            REQUIRE(std::isinf(effect.GetTailTime()));
        }
        {
            ScopedFactoryRegistration registration(create_latency_tail_instrument_processor);
            pulp::format::au::PulpAUInstrument instrument(nullptr);
            REQUIRE(instrument.SupportsTail());
            REQUIRE(std::isinf(instrument.GetTailTime()));
        }
    }
}

TEST_CASE("AU v2 host callbacks mark transport jumps for processor reset",
          "[au][auv2][transport][reset]") {
    ScopedFactoryRegistration registration(create_effect_processor);

    pulp::format::au::PulpAUEffect effect(nullptr);

    AUv2TransportCallbackState transport;
    HostCallbackInfo callbacks{};
    callbacks.hostUserData = &transport;
    callbacks.beatAndTempoProc = auv2_test_beat_and_tempo;
    callbacks.musicalTimeLocationProc = auv2_test_musical_time;
    callbacks.transportStateProc = auv2_test_transport_state;
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_HostCallbacks,
                                       kAudioUnitScope_Global,
                                       0,
                                       &callbacks,
                                       sizeof(callbacks)) == noErr);

    constexpr UInt32 kFrames = 8;
    pulp::format::detail::PlayheadSnapshot previous;
    auto first = pulp::format::au::make_render_process_context(48000.0, kFrames);
    pulp::format::au::apply_host_callbacks_to_process_context(
        first, effect, previous);
    REQUIRE_FALSE(first.transport_jump);
    REQUIRE_FALSE(first.should_reset_dsp_state());
    REQUIRE(first.position_samples == 0);
    REQUIRE(first.is_playing);
    REQUIRE(first.process_mode == pulp::format::ProcessMode::Realtime);
    REQUIRE(first.render_speed_hint == pulp::format::RenderSpeedHint::Realtime);

    transport.sample_position = kFrames;
    transport.beat = 1.0;
    auto continuous =
        pulp::format::au::make_render_process_context(48000.0, kFrames);
    pulp::format::au::apply_host_callbacks_to_process_context(
        continuous, effect, previous);
    REQUIRE_FALSE(continuous.transport_jump);
    REQUIRE_FALSE(continuous.should_reset_dsp_state());
    REQUIRE(continuous.position_samples == kFrames);

    transport.sample_position = 4096.0;
    transport.beat = 64.0;
    auto jumped = pulp::format::au::make_render_process_context(48000.0, kFrames);
    pulp::format::au::apply_host_callbacks_to_process_context(
        jumped, effect, previous);
    REQUIRE(jumped.transport_jump);
    REQUIRE(jumped.should_reset_dsp_state());
    REQUIRE(jumped.position_samples == 4096);
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

TEST_CASE("AU v3 routes channel-wide MIDI into per-note MPE expression",
          "[au][auv3][mpe]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_MusicDevice;
        desc.componentSubType = 'TstM';
        desc.componentManufacturer = 'Plup';

        // Snapshot of one render-block invocation, copied out of the processor
        // BEFORE the owning unit is released (the processor is owned by the
        // unit's bridge, so it dangles after -release).
        struct DriveResult {
            bool mpe_input_present = false;
            int process_count = 0;
            std::vector<CapturedMpeEvent> captured;
        };

        auto drive_block = [&](bool declare_mpe) -> DriveResult {
            g_au_mpe_declare = declare_mpe;
            ScopedFactoryRegistration registration(create_mpe_instrument_processor);

            NSError* error = nil;
            PulpAudioUnit* unit =
                [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                           options:0
                                                             error:&error];
            REQUIRE(unit != nil);
            REQUIRE(error == nil);

            auto* processor = g_last_mpe_processor;
            REQUIRE(processor != nullptr);

            NSError* allocate_error = nil;
            REQUIRE([unit allocateRenderResourcesAndReturnError:&allocate_error]);
            REQUIRE(allocate_error == nil);

            constexpr UInt32 kFrames = 16;
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

            // Channel-wide MIDI on MPE lower-zone member channel 1 (0-based):
            // a note-on plus the three expression axes the MpeVoiceTracker
            // narrows into per-note pitch bend / timbre / pressure. Linked in
            // host delivery order (the AU render path does not sort midi_in).
            std::array<AURenderEvent, 4> events{};
            auto set_midi = [](AURenderEvent& e, uint8_t s, uint8_t d1, uint8_t d2,
                               uint16_t len) {
                e.MIDI.eventType = AURenderEventMIDI;
                e.MIDI.eventSampleTime = 0;
                e.MIDI.length = len;
                e.MIDI.cable = 0;
                e.MIDI.data[0] = s;
                e.MIDI.data[1] = d1;
                e.MIDI.data[2] = d2;
            };
            set_midi(events[0], 0x91, 60, 100, 3);    // note-on, member ch, note 60
            set_midi(events[1], 0xE1, 0x00, 0x60, 3); // pitch bend up (0x3000)
            set_midi(events[2], 0xB1, 74, 100, 3);    // CC74 → timbre
            set_midi(events[3], 0xD1, 80, 0, 2);      // channel pressure
            events[0].MIDI.next = &events[1];
            events[1].MIDI.next = &events[2];
            events[2].MIDI.next = &events[3];
            events[3].MIDI.next = nullptr;

            AudioUnitRenderActionFlags flags = 0;
            AudioTimeStamp timestamp{};
            timestamp.mFlags = kAudioTimeStampSampleTimeValid;
            timestamp.mSampleTime = 0;

            AUInternalRenderBlock block = [unit internalRenderBlock];
            REQUIRE(block != nil);
            auto status = block(&flags, &timestamp, kFrames, 0, &output.list,
                                &events[0], nil);
            REQUIRE(status == noErr);

            DriveResult result;
            result.mpe_input_present = processor->mpe_input_present;
            result.process_count = processor->process_count;
            result.captured = processor->captured;

            [unit deallocateRenderResources];
            [unit release];
            return result;
        };

        SECTION("opted-in instrument receives routed per-note expression") {
            const DriveResult r = drive_block(/*declare_mpe=*/true);
            REQUIRE(r.process_count == 1);
            REQUIRE(r.mpe_input_present);

            using K = pulp::midi::MpeExpressionEvent::Kind;
            const auto& evs = r.captured;
            auto has = [&](K k) {
                return std::any_of(evs.begin(), evs.end(),
                    [&](const CapturedMpeEvent& e) { return e.kind == k; });
            };
            REQUIRE(has(K::NoteOn));
            REQUIRE(has(K::PitchBend));
            REQUIRE(has(K::Timbre));
            REQUIRE(has(K::Pressure));

            // Every emitted event carries the note-on's identity (member
            // channel 1, note 60): the channel-wide MIDI was narrowed onto the
            // one active note.
            for (const auto& e : evs) {
                REQUIRE(e.channel == 1);
                REQUIRE(e.note == 60);
            }

            auto first = [&](K k) -> const CapturedMpeEvent& {
                auto it = std::find_if(evs.begin(), evs.end(),
                    [&](const CapturedMpeEvent& e) { return e.kind == k; });
                REQUIRE(it != evs.end());
                return *it;
            };
            REQUIRE(first(K::PitchBend).pitch_bend_semitones > 0.0f);
            REQUIRE_THAT(first(K::Timbre).timbre,
                         WithinAbs(100.0f / 127.0f, 1e-3f));
            REQUIRE_THAT(first(K::Pressure).pressure,
                         WithinAbs(80.0f / 127.0f, 1e-3f));
        }

        SECTION("non-opted-in instrument gets a null mpe_input") {
            const DriveResult r = drive_block(/*declare_mpe=*/false);
            REQUIRE(r.process_count == 1);
            REQUIRE_FALSE(r.mpe_input_present);
            REQUIRE(r.captured.empty());
        }
    }
    g_au_mpe_declare = true;
}

TEST_CASE("AU v3 channel capabilities expose only descriptor-declared layouts",
          "[au][auv3][bus-layout][negotiation]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstL';
        desc.componentManufacturer = 'Plup';
        ScopedFactoryRegistration registration(create_layout_processor);

        NSError* error = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&error];
        REQUIRE(unit != nil);
        REQUIRE(error == nil);
        NSArray<NSNumber*>* capabilities = [unit channelCapabilities];
        REQUIRE(capabilities != nil);
        REQUIRE(capabilities.count == 4);
        REQUIRE([capabilities[0] intValue] == 2);
        REQUIRE([capabilities[1] intValue] == 2);
        REQUIRE([capabilities[2] intValue] == 1);
        REQUIRE([capabilities[3] intValue] == 1);
        [unit release];
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
        REQUIRE(processor->audio_workgroup_prepares.load(std::memory_order_relaxed) == 1);

        // Context adoption precedes every render exit, not only the normal
        // process path. A host may retire the prior borrowed context as soon as
        // either of these early-return renders completes.
        REQUIRE(block(&flags, &timestamp, kFrames, 0, nullptr, nullptr, nil) ==
                noErr);
        REQUIRE(processor->audio_workgroup_prepares.load(
                    std::memory_order_relaxed) == 2);
        REQUIRE(block(&flags, &timestamp, unit.maximumFramesToRender + 1, 0,
                      &output.list, nullptr, nil) ==
                kAudioUnitErr_TooManyFramesToProcess);
        REQUIRE(processor->audio_workgroup_prepares.load(
                    std::memory_order_relaxed) == 3);

        REQUIRE(processor->process_buffers_count == 1);
        REQUIRE(processor->process_count == 1);
        REQUIRE(processor->last_context.process_mode ==
                pulp::format::ProcessMode::Realtime);
        REQUIRE(processor->last_context.render_speed_hint ==
                pulp::format::RenderSpeedHint::Realtime);
        REQUIRE_FALSE(processor->last_context.is_offline());
        REQUIRE_FALSE(processor->last_context.allows_offline_quality_work());
        REQUIRE_FALSE(processor->last_context.is_maintenance_render());
        REQUIRE(processor->last_process_buffers_layout_ok);
        REQUIRE(processor->last_process_buffers_storage_ok);
        REQUIRE(processor->last_process_buffers_input_count == 0);
        REQUIRE(processor->last_process_buffers_output_count == 1);
        REQUIRE(processor->last_process_buffers_main_input_channels == 0);
        REQUIRE(processor->last_process_buffers_main_output_channels == 2);
        REQUIRE(processor->last_process_buffers_sidechain_channels == 0);
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

TEST_CASE("AU v3 exposes and routes descriptor-declared sidechain input",
          "[au][auv3][audio][sidechain]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstS';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_sidechain_effect_processor);

        NSError* error = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&error];
        REQUIRE(unit != nil);
        REQUIRE(error == nil);
        REQUIRE([unit inputBusses] != nil);
        REQUIRE([unit inputBusses].count == 2u);
        REQUIRE([unit outputBusses] != nil);
        REQUIRE([unit outputBusses].count == 1u);

        auto* processor = g_last_effect_processor;
        REQUIRE(processor != nullptr);

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

        __block NSInteger main_pull_count = 0;
        __block NSInteger sidechain_pull_count = 0;
        AURenderPullInputBlock pull = ^AUAudioUnitStatus(
            AudioUnitRenderActionFlags*,
            const AudioTimeStamp*,
            AUAudioFrameCount frameCount,
            NSInteger inputBusNumber,
            AudioBufferList* inputData) {
            if (inputBusNumber == 0) {
                ++main_pull_count;
                for (UInt32 ch = 0; ch < inputData->mNumberBuffers; ++ch) {
                    auto* samples = static_cast<float*>(inputData->mBuffers[ch].mData);
                    for (AUAudioFrameCount frame = 0; frame < frameCount; ++frame) {
                        samples[frame] = ch == 0 ? 0.125f : -0.125f;
                    }
                }
                return noErr;
            }
            if (inputBusNumber == 1) {
                ++sidechain_pull_count;
                REQUIRE(inputData->mNumberBuffers == 1u);
                auto* samples = static_cast<float*>(inputData->mBuffers[0].mData);
                for (AUAudioFrameCount frame = 0; frame < frameCount; ++frame) {
                    samples[frame] = 0.75f;
                }
                return noErr;
            }
            return kAudioUnitErr_InvalidElement;
        };

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = 400;

        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block != nil);
        auto status = block(&flags,
                            &timestamp,
                            kFrames,
                            0,
                            &output.list,
                            nil,
                            pull);
        REQUIRE(status == noErr);

        REQUIRE(main_pull_count == 1);
        REQUIRE(sidechain_pull_count == 1);
        REQUIRE(processor->process_buffers_count == 1);
        REQUIRE(processor->process_count == 1);
        REQUIRE(processor->last_process_buffers_layout_ok);
        REQUIRE(processor->last_process_buffers_storage_ok);
        REQUIRE(processor->last_process_buffers_input_count == 2);
        REQUIRE(processor->last_process_buffers_output_count == 1);
        REQUIRE(processor->last_process_buffers_main_input_channels == 2);
        REQUIRE(processor->last_process_buffers_main_output_channels == 2);
        REQUIRE(processor->last_process_buffers_sidechain_channels == 1);
        REQUIRE_THAT(processor->last_process_buffers_sidechain_first_sample,
                     WithinAbs(0.75f, 1e-6f));

        [unit deallocateRenderResources];
        [unit release];
    }
}

TEST_CASE("AU v3 render events drop past realtime parameter capacity without growing",
          "[au][auv3][params][realtime][capacity]") {
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

        constexpr std::size_t kCapacity =
            pulp::state::ParameterEventQueue::kCapacity;
        constexpr UInt32 kFrames = static_cast<UInt32>(kCapacity + 1);
        unit.maximumFramesToRender = kFrames;

        NSError* allocate_error = nil;
        REQUIRE([unit allocateRenderResourcesAndReturnError:&allocate_error]);
        REQUIRE(allocate_error == nil);

        std::array<float, kFrames> left{};
        std::array<float, kFrames> right{};
        struct StereoBufferList {
            AudioBufferList list;
            AudioBuffer extra[1];
        } output{};
        output.list.mNumberBuffers = 2;
        output.list.mBuffers[0].mNumberChannels = 1;
        output.list.mBuffers[0].mDataByteSize = kFrames * sizeof(float);
        output.list.mBuffers[0].mData = left.data();
        output.list.mBuffers[1].mNumberChannels = 1;
        output.list.mBuffers[1].mDataByteSize = kFrames * sizeof(float);
        output.list.mBuffers[1].mData = right.data();

        std::array<AURenderEvent, kCapacity + 1> events{};
        for (std::size_t i = 0; i < events.size(); ++i) {
            auto& event = events[i];
            event.parameter.eventSampleTime = static_cast<AUEventSampleTime>(200 + i);
            event.parameter.eventType = AURenderEventParameter;
            event.parameter.rampDurationSampleFrames = 0;
            event.parameter.parameterAddress = 1;
            event.parameter.value = -30.0f;
            event.head.next = (i + 1 < events.size()) ? &events[i + 1] : nullptr;
        }
        events[0].parameter.value = -6.0f;
        events[kCapacity - 1].parameter.value = -18.0f;
        events[kCapacity].parameter.value = 24.0f;

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = 200;

        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block != nil);
        auto status = block(&flags,
                            &timestamp,
                            kFrames,
                            0,
                            &output.list,
                            &events[0],
                            nil);
        REQUIRE(status == noErr);

        REQUIRE(processor->process_count == 1);
        REQUIRE(processor->had_param_events);
        REQUIRE(processor->last_param_events.size() == kCapacity);
        REQUIRE(processor->last_param_events.front().sample_offset == 0);
        REQUIRE_THAT(processor->last_param_events.front().value, WithinAbs(-6.0f, 1e-6f));
        REQUIRE(processor->last_param_events.back().sample_offset ==
                static_cast<int32_t>(kCapacity - 1));
        REQUIRE_THAT(processor->last_param_events.back().value, WithinAbs(-18.0f, 1e-6f));
        REQUIRE_THAT(processor->gain_seen_in_process, WithinAbs(24.0f, 1e-6f));
        REQUIRE_THAT(processor->state().get_value(1), WithinAbs(24.0f, 1e-6f));

        REQUIRE([unit pulpLastParameterEventCount] == kCapacity);
        REQUIRE([unit pulpLastParameterEventCapacity] == kCapacity);
        REQUIRE([unit pulpLastParameterEventsOverflowed] == YES);
        REQUIRE([unit pulpLastParameterEventDropCount] == 1);
        REQUIRE([unit pulpLastParameterEventSampleOffsetAtIndex:0] == 0);
        REQUIRE_THAT([unit pulpLastParameterEventValueAtIndex:0], WithinAbs(-6.0f, 1e-6f));
        REQUIRE([unit pulpLastParameterEventSampleOffsetAtIndex:kCapacity - 1] ==
                static_cast<int32_t>(kCapacity - 1));
        REQUIRE_THAT([unit pulpLastParameterEventValueAtIndex:kCapacity - 1],
                     WithinAbs(-18.0f, 1e-6f));

        [unit deallocateRenderResources];
        [unit release];
    }
}

TEST_CASE("AU v3 transport jumps request processor reset through ProcessContext",
          "[au][auv3][transport][reset]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TTrJ';
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

        __block double current_sample_position = 1000.0;
        unit.transportStateBlock = ^BOOL(
            AUHostTransportStateFlags* transport_flags,
            double* sample_position,
            double* cycle_start,
            double* cycle_end) {
            if (transport_flags) *transport_flags = AUHostTransportStateMoving;
            if (sample_position) *sample_position = current_sample_position;
            if (cycle_start) *cycle_start = 0.0;
            if (cycle_end) *cycle_end = 0.0;
            return YES;
        };

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block != nil);

        current_sample_position = 1000.0;
        auto status = block(&flags,
                            &timestamp,
                            kFrames,
                            0,
                            &output.list,
                            nullptr,
                            nil);
        REQUIRE(status == noErr);
        const auto first = processor->last_context;
        REQUIRE_FALSE(first.transport_jump);
        REQUIRE_FALSE(first.should_reset_dsp_state());

        current_sample_position = 1008.0;
        status = block(&flags,
                       &timestamp,
                       kFrames,
                       0,
                       &output.list,
                       nullptr,
                       nil);
        REQUIRE(status == noErr);
        const auto continuous = processor->last_context;
        REQUIRE_FALSE(continuous.transport_jump);
        REQUIRE_FALSE(continuous.should_reset_dsp_state());

        current_sample_position = 4096.0;
        status = block(&flags,
                       &timestamp,
                       kFrames,
                       0,
                       &output.list,
                       nullptr,
                       nil);
        REQUIRE(status == noErr);
        const auto jumped = processor->last_context;
        REQUIRE(jumped.transport_jump);
        REQUIRE(jumped.should_reset_dsp_state());

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

        // The render block's scratch output buffers are pre-allocated in
        // allocateRenderResources — the audio-thread render path never
        // allocates (see au_adapter.mm). Real AU hosts always allocate render
        // resources before rendering; this test exercises the null/undersized
        // host-buffer substitution on that supported path.
        NSError* allocate_error = nil;
        REQUIRE([unit allocateRenderResourcesAndReturnError:&allocate_error]);
        REQUIRE(allocate_error == nil);

        constexpr UInt32 kFrames = 8;
        float undersized = 0.0f;
        struct StereoBufferList {
            AudioBufferList list;
            AudioBuffer extra[1];
        } output{};
        output.list.mNumberBuffers = 2;
        output.list.mBuffers[0].mNumberChannels = 1;
        output.list.mBuffers[0].mDataByteSize = 0;
        output.list.mBuffers[0].mData = nullptr;
        output.list.mBuffers[1].mNumberChannels = 1;
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

        [unit deallocateRenderResources];
        [unit release];
    }
}

TEST_CASE("AU v3 fails closed for malformed render buffer topology",
          "[au][auv3][render][malformed-layout]") {
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

        constexpr UInt32 kFrames = 8;
        std::array<std::array<float, kFrames>, 3> samples{};
        struct ThreeBufferList {
            AudioBufferList list;
            AudioBuffer extra[2];
        } output{};
        output.list.mNumberBuffers = 2;
        for (UInt32 i = 0; i < 3; ++i) {
            samples[i].fill(9.0f);
            output.list.mBuffers[i] = {
                1, static_cast<UInt32>(sizeof(samples[i])), samples[i].data()};
        }

        AURenderPullInputBlock pull = nil;
        SECTION("fewer channel buffers") {
            output.list.mNumberBuffers = 1;
        }
        SECTION("more channel buffers") {
            output.list.mNumberBuffers = 3;
        }
        SECTION("wrong channel count") {
            output.list.mBuffers[1].mNumberChannels = 2;
        }
        SECTION("null active main-input storage") {
            pull = ^AUAudioUnitStatus(
                AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                AUAudioFrameCount, NSInteger bus, AudioBufferList* input) {
                if (bus == 0 && input && input->mNumberBuffers > 1)
                    input->mBuffers[1].mData = nullptr;
                return noErr;
            };
        }

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block != nil);
        REQUIRE(block(&flags, &timestamp, kFrames, 0,
                      &output.list, nullptr, pull) == noErr);
        REQUIRE(processor->process_buffers_count == 0);
        REQUIRE((flags & kAudioUnitRenderAction_OutputIsSilence) != 0);
        for (UInt32 b = 0; b < output.list.mNumberBuffers; ++b) {
            REQUIRE(std::all_of(samples[b].begin(), samples[b].end(),
                                [](float value) { return value == 0.0f; }));
        }

        [unit deallocateRenderResources];
        [unit release];
    }
}

TEST_CASE("AU v2 fails closed for malformed ProcessBufferLists topology",
          "[au][auv2][render][malformed-layout]") {
    ScopedFactoryRegistration registration(create_effect_processor);
    pulp::format::au::PulpAUEffect effect(nullptr);
    effect.CreateElements();

    AudioStreamBasicDescription format{};
    format.mSampleRate = 48000.0;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                          kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    REQUIRE(effect.Input(0).SetStreamFormat(format) == noErr);
    REQUIRE(effect.Output(0).SetStreamFormat(format) == noErr);
    constexpr UInt32 kFrames = 8;
    UInt32 max_frames = kFrames;
    REQUIRE(effect.DispatchSetProperty(kAudioUnitProperty_MaximumFramesPerSlice,
                                       kAudioUnitScope_Global, 0,
                                       &max_frames, sizeof(max_frames)) == noErr);
    REQUIRE(effect.DoInitialize() == noErr);
    auto* processor = g_last_effect_processor;
    REQUIRE(processor != nullptr);

    std::array<float, kFrames> in_l{};
    std::array<float, kFrames> in_r{};
    struct StereoBufferList {
        AudioBufferList list;
        AudioBuffer extra[1];
    } input{};
    input.list.mNumberBuffers = 2;
    input.list.mBuffers[0] = {1, sizeof(in_l), in_l.data()};
    input.list.mBuffers[1] = {1, sizeof(in_r), in_r.data()};

    std::array<std::array<float, kFrames>, 3> samples{};
    struct ThreeBufferList {
        AudioBufferList list;
        AudioBuffer extra[2];
    } output{};
    output.list.mNumberBuffers = 2;
    for (UInt32 i = 0; i < 3; ++i) {
        samples[i].fill(9.0f);
        output.list.mBuffers[i] = {
            1, static_cast<UInt32>(sizeof(samples[i])), samples[i].data()};
    }

    SECTION("fewer channel buffers") {
        output.list.mNumberBuffers = 1;
    }
    SECTION("more channel buffers") {
        output.list.mNumberBuffers = 3;
    }
    SECTION("wrong channel count") {
        output.list.mBuffers[1].mNumberChannels = 2;
    }
    SECTION("null active-bus pointer") {
        output.list.mBuffers[0].mData = nullptr;
    }

    AudioUnitRenderActionFlags flags = 0;
    REQUIRE(effect.ProcessBufferLists(
                flags, input.list, output.list, kFrames) == noErr);
    REQUIRE(processor->process_buffers_count == 0);
    REQUIRE((flags & kAudioUnitRenderAction_OutputIsSilence) != 0);
    for (UInt32 b = 0; b < output.list.mNumberBuffers; ++b) {
        if (!output.list.mBuffers[b].mData) continue;
        REQUIRE(std::all_of(samples[b].begin(), samples[b].end(),
                            [](float value) { return value == 0.0f; }));
    }
    effect.DoCleanup();
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

    std::vector<uint8_t> serialize_plugin_state() const override {
        return {plugin_state.begin(), plugin_state.end()};
    }

    bool deserialize_plugin_state(std::span<const uint8_t> data) override {
        plugin_state.assign(data.begin(), data.end());
        return true;
    }

    int process_count = 0;
    std::string plugin_state;
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

TEST_CASE("AU v3 lifecycle churn preserves state and tolerates bypass toggles",
          "[au][auv3][lifecycle][churn][state][bypass]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TBpE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_bypass_processor);
        for (int cycle = 0; cycle < 200; ++cycle) {
            const float saved_gain = -24.0f + static_cast<float>(cycle % 48);
            const std::string saved_payload = "cycle=" + std::to_string(cycle);

            NSError* saver_error = nil;
            PulpAudioUnit* saver =
                [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                           options:0
                                                             error:&saver_error];
            REQUIRE(saver != nil);
            REQUIRE(saver_error == nil);
            auto* saver_processor = TestAUBypassProcessor::g_last;
            REQUIRE(saver_processor != nullptr);
            saver_processor->state().set_value(7, saved_gain);
            saver_processor->plugin_state = saved_payload;
            [saver setShouldBypassEffect:(cycle % 2 == 0 ? YES : NO)];

            NSDictionary<NSString*, id>* saved = [[saver fullState] retain];
            REQUIRE(saved != nil);
            NSData* saved_payload_data = saved[@"pulpState"];
            REQUIRE(saved_payload_data != nil);
            [saver release];

            NSError* loader_error = nil;
            PulpAudioUnit* loader =
                [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                           options:0
                                                             error:&loader_error];
            REQUIRE(loader != nil);
            REQUIRE(loader_error == nil);
            auto* loader_processor = TestAUBypassProcessor::g_last;
            REQUIRE(loader_processor != nullptr);
            [loader setFullState:saved];
            REQUIRE_THAT(loader_processor->state().get_value(7),
                         WithinAbs(saved_gain, 0.01));
            REQUIRE(loader_processor->plugin_state == saved_payload);
            REQUIRE([loader shouldBypassEffect] == (cycle % 2 == 0 ? YES : NO));

            NSDictionary<NSString*, id>* resaved = [loader fullState];
            REQUIRE(resaved != nil);
            NSData* resaved_payload_data = resaved[@"pulpState"];
            REQUIRE(resaved_payload_data != nil);
            REQUIRE([resaved_payload_data isEqualToData:saved_payload_data]);

            NSError* allocate_error = nil;
            REQUIRE([loader allocateRenderResourcesAndReturnError:&allocate_error]);
            REQUIRE(allocate_error == nil);
            constexpr UInt32 kFrames = 4;
            float left[kFrames]{};
            float right[kFrames]{};
            struct StereoBufferList {
                AudioBufferList list;
                AudioBuffer extra[1];
            } output{};
            output.list.mNumberBuffers = 2;
            output.list.mBuffers[0] = {1, kFrames * sizeof(float), left};
            output.list.mBuffers[1] = {1, kFrames * sizeof(float), right};
            AURenderPullInputBlock pull = ^AUAudioUnitStatus(
                AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                AUAudioFrameCount, NSInteger, AudioBufferList* input) {
                for (UInt32 b = 0; b < input->mNumberBuffers; ++b) {
                    auto* samples = static_cast<float*>(input->mBuffers[b].mData);
                    const UInt32 count = input->mBuffers[b].mDataByteSize / sizeof(float);
                    for (UInt32 i = 0; i < count; ++i) samples[i] = 0.25f;
                }
                return noErr;
            };
            AudioUnitRenderActionFlags flags = 0;
            AudioTimeStamp timestamp{};
            timestamp.mFlags = kAudioTimeStampSampleTimeValid;
            timestamp.mSampleTime = cycle * kFrames;
            AUInternalRenderBlock render = [loader internalRenderBlock];
            REQUIRE(render != nil);
            REQUIRE(render(&flags, &timestamp, kFrames, 0,
                           &output.list, nullptr, pull) == noErr);

            // Toggle mid-lifecycle and render the opposite path too. The next
            // fresh instance must still begin from its own defaults until the
            // saved state is explicitly restored.
            [loader setShouldBypassEffect:(cycle % 2 == 0 ? NO : YES)];
            REQUIRE([loader shouldBypassEffect] == (cycle % 2 == 0 ? NO : YES));
            REQUIRE(render(&flags, &timestamp, kFrames, 0,
                           &output.list, nullptr, pull) == noErr);
            [loader deallocateRenderResources];
            [loader release];
            [saved release];
        }
    }
}

TEST_CASE("AU v3 without a Bypass parameter still honours setShouldBypassEffect",
          "[au][auv3][bypass][item-3.1]") {
    // The atomic-fallback path this test pins only exists when no Bypass
    // param is present — since host-quirks P3b one is synthesized by
    // default, so disable synthesis here.
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);
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
    pulp::format::set_host_quirk_policy(std::nullopt);
}

TEST_CASE("AU v3 synthesizes a Bypass param when the plugin declares none (P3b)",
          "[au][auv3][host-quirks][p3][bypass]") {
    pulp::format::set_host_quirk_policy(pulp::format::QuirkFilter{});  // quirk on
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_effect_processor);  // no Bypass param

        NSError* err = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&err];
        REQUIRE(unit != nil);
        REQUIRE(err == nil);

        // The adapter synthesized a Bypass param (reserved ID) and the
        // detection pass adopted it onto the AU bypass surface; it now
        // appears in the parameter tree and drives shouldBypassEffect.
        REQUIRE([unit pulpBypassParameterId] ==
                static_cast<uint32_t>(pulp::format::kSynthesizedBypassParamId));
        AUParameterTree* tree = unit.parameterTree;
        REQUIRE(tree != nil);
        REQUIRE(tree.allParameters.count == 2u);  // processor's Gain + synthesized Bypass

        [unit release];
    }
    pulp::format::set_host_quirk_policy(std::nullopt);
}

// ─────────────────────────────────────────────────────────────────────────
// Per-method audit invariants (macOS plan item 3.1)
//
// Pins the no-op-by-design surface of `PulpAudioUnit` so regressions to
// the table in the `au_adapter.mm` header don't slip through silently.
// Update both this test AND the header-comment table when you change the
// audit row for an override.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("AU v3 render context observer forwards workgroup changes and removal",
          "[au][auv3][workgroup][rt-safety]") {
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);
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
        auto* processor = g_last_effect_processor;
        REQUIRE(processor != nullptr);

        AURenderContextObserver observer = unit.renderContextObserver;
        REQUIRE(observer != nil);

        AudioUnitRenderContext context{};
        // Deliberately not a real os_object. If the RT observer ever regresses
        // to os_retain/os_release, this test crashes instead of silently
        // blessing an undocumented reference-count operation on the audio path.
        context.workgroup = reinterpret_cast<os_workgroup_t>(std::uintptr_t{0x7070});
        observer(&context);
        REQUIRE(processor->audio_workgroup.load(std::memory_order_acquire) ==
                reinterpret_cast<void*>(context.workgroup));
        REQUIRE(processor->audio_workgroup_updates.load(std::memory_order_relaxed) == 1);
        REQUIRE(processor->audio_workgroup_prepares.load(
                    std::memory_order_relaxed) == 1);

        [unit deallocateRenderResources];
        REQUIRE(processor->audio_workgroup.load(std::memory_order_acquire) == nullptr);
        REQUIRE(processor->audio_workgroup_updates.load(std::memory_order_relaxed) == 2);
        REQUIRE(processor->audio_workgroup_waits.load(
                    std::memory_order_relaxed) == 1);
        [unit release];
    }
    pulp::format::set_host_quirk_policy(std::nullopt);
}

TEST_CASE("AU v3 per-method audit invariants",
          "[au][auv3][audit][item-3.1]") {
    // Pin the test processor's own 1-parameter tree — disable bypass
    // synthesis (host-quirks P3b) so the count reflects only the plugin's
    // declared params, not the synthesized Bypass.
    pulp::format::set_host_quirk_policy(pulp::format::kQuirkFilterOff);
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
        REQUIRE(unit.renderContextObserver != nil);

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
    pulp::format::set_host_quirk_policy(std::nullopt);
}

// Regression guard for the AUv3-in-Logic sizing fix.
//
// Pulp deliberately does NOT implement `supportedViewConfigurations:` /
// `selectViewConfiguration:`. Logic Pro sizes AU v3 editors through the
// view-configuration path and offers only oversized ~4:3 configs (measured
// 1024x768 / 1366x1024). The moment an AU returns ANY supported config, Logic
// locks the editor window to that config's aspect ratio at every size, so a
// wide fixed-design editor (e.g. 900x520 ≈ 16:9.4) letterboxes with top/bottom
// bars that can't be resized away. Apple's empty-set semantics ("use the
// largest available view configuration") make returning empty strictly worse.
//
// By NOT overriding these selectors, `respondsToSelector:` returns NO, Logic
// skips config negotiation entirely, and the editor free-resizes to the
// design's own aspect — matching the tight, proportional fit Pulp gets in
// REAPER / CLAP / VST3 / standalone (verified in Logic 2026-05-29). If anyone
// reintroduces these overrides, this test fails to flag the regression.
TEST_CASE("AU v3 does not opt into host view configurations (Logic 4:3-lock fix)",
          "[au][auv3][view-config][resize]") {
    @autoreleasepool {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'TstE';
        desc.componentManufacturer = 'Plup';

        ScopedFactoryRegistration registration(create_wide_editor_processor);

        NSError* err = nil;
        PulpAudioUnit* unit =
            [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                       options:0
                                                         error:&err];
        REQUIRE(unit != nil);
        REQUIRE(err == nil);

        // Base AUAudioUnit *does* implement these selectors, so respondsToSelector
        // is always YES. The regression guard that matters is that PulpAudioUnit
        // does NOT OVERRIDE them — i.e. its method implementation is the inherited
        // base one. If someone re-adds an override (reintroducing the Logic
        // 4:3-lock), the IMP diverges and this fails.
        IMP pulpSupported = class_getMethodImplementation(
            [PulpAudioUnit class], @selector(supportedViewConfigurations:));
        IMP baseSupported = class_getMethodImplementation(
            [AUAudioUnit class], @selector(supportedViewConfigurations:));
        REQUIRE(pulpSupported == baseSupported);

        IMP pulpSelect = class_getMethodImplementation(
            [PulpAudioUnit class], @selector(selectViewConfiguration:));
        IMP baseSelect = class_getMethodImplementation(
            [AUAudioUnit class], @selector(selectViewConfiguration:));
        REQUIRE(pulpSelect == baseSelect);

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

// Regression coverage: main_is_extension used to test
// `[bundleIdentifier hasSuffix:@".appex"]`, but bundle IDENTIFIERS are
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
TEST_CASE("current_auv3_wrapper_identifier — AU_HOST_BUNDLE_ID env wins",
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

TEST_CASE("detect_host_type — wrapper id wins when wrapper is recognized",
          "[au][auv3][host-detection][item-3.1]") {
    // `detect_host_type()` consults `current_auv3_wrapper_identifier()`
    // first on Apple platforms; when that returns a recognized id, the
    // wrapper-side wins. When unrecognised (or empty), it falls back to
    // the executable-path heuristic. The classifier-level pieces are
    // pinned above; this test only checks that `detect_host_type()` is
    // callable and returns a value in the enum (i.e. the wrapper path
    // doesn't crash when the Apple host services are absent).
    auto type = pulp::format::detect_host_type();
    REQUIRE(static_cast<int>(type) >= static_cast<int>(pulp::format::HostType::Unknown));
    REQUIRE(static_cast<int>(type) <= static_cast<int>(pulp::format::HostType::Other));
}

TEST_CASE("AU v3 editor parameter edit + gesture reach the host for automation",
          "[au][auv3][params][automation]") {
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

        // The parameter tree must be RETAINED across calls: the host observes
        // these exact AUParameter objects, so a per-call rebuild would push UI
        // edits into throwaway objects and break automation recording.
        AUParameterTree* tree = unit.parameterTree;
        REQUIRE(tree != nil);
        REQUIRE(tree == unit.parameterTree);

        AUParameter* p = [tree parameterWithAddress:1];
        REQUIRE(p != nil);

        // Capture the host-visible automation events — what Logic's recorder sees.
        auto events = std::make_shared<std::vector<std::pair<int, float>>>();
        AUParameterObserverToken obs = [tree tokenByAddingParameterAutomationObserver:
            ^(NSInteger count, const AUParameterAutomationEvent* evs) {
                for (NSInteger i = 0; i < count; ++i)
                    if (evs[i].address == 1)
                        events->push_back({(int)evs[i].eventType, evs[i].value});
            }];

        // Drive exactly what parameter_binding.hpp emits from a widget drag:
        // gesture begin → value change → gesture end, via the shared StateStore.
        auto& store = processor->state();
        const float before = p.value;
        store.begin_gesture(1);
        store.set_value(1, before + 3.0f);
        store.end_gesture(1);

        // UI → host write-back: the host AUParameter now reflects the edit. This
        // is the load-bearing assertion — delivered synchronously, and what lets
        // Logic read/record the moved value.
        REQUIRE_THAT(p.value, Catch::Matchers::WithinAbs(before + 3.0, 0.01));

        // The automation observer is delivered asynchronously (Apple batches the
        // events onto the runloop), so pump it before asserting the recorder saw
        // a Touch (begin), a Value, and a Release (end).
        for (int i = 0; i < 25 && events->size() < 3; ++i)
            [[NSRunLoop currentRunLoop]
                runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];

        bool sawTouch = false, sawValue = false, sawRelease = false;
        for (auto& e : *events) {
            if (e.first == AUParameterAutomationEventTypeTouch) sawTouch = true;
            if (e.first == AUParameterAutomationEventTypeValue) sawValue = true;
            if (e.first == AUParameterAutomationEventTypeRelease) sawRelease = true;
        }
        CHECK(sawTouch);
        CHECK(sawValue);
        CHECK(sawRelease);

        [tree removeParameterObserver:obs];
        [unit release];
    }
}
