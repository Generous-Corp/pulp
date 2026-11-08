// AUv3 bus topology, format negotiation, and MIDI output declaration.
//
// Drives the real PulpAudioUnit the way a host does: reads inputBusses and
// outputBusses, proposes formats through the bus objects, then renders each
// output bus with its own render call.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/midi/message.hpp>

#import "../core/format/src/au_audio_unit.h"

#include <array>
#include <memory>
#include <vector>

namespace {

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

struct SeenBus {
    std::size_t index = 0;
    pulp::format::BusRole role = pulp::format::BusRole::Main;
    int declared_channels = 0;
    bool active = false;
    std::size_t channels = 0;
};

// What the processor was handed on its last block. Copied out because the
// processor is owned by the unit.
struct SeenBlock {
    std::vector<SeenBus> inputs;
    std::vector<SeenBus> outputs;
    int blocks = 0;
};
SeenBlock g_seen;

// Main in 2, sidechain 1, aux in 2; main out 2, stem A 2, stem B 1. Each stem
// carries a value that proves which input and which bus it came from.
class MultiBusProcessor : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "AuMultiBus";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.au.multibus";
        d.version = "1.0.0";
        d.category = pulp::format::PluginCategory::Effect;
        d.input_buses = {
            {"Main In", 2, false, pulp::format::BusRole::Main},
            {"Sidechain", 1, true, pulp::format::BusRole::Sidechain},
            {"Aux In", 2, true, pulp::format::BusRole::Aux},
        };
        d.output_buses = {
            {"Main Out", 2, false, pulp::format::BusRole::Main},
            {"Stem A", 2, true, pulp::format::BusRole::Aux},
            {"Stem B", 1, true, pulp::format::BusRole::Aux},
        };
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    void process(pulp::format::ProcessBuffers& audio, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        ++g_seen.blocks;
        g_seen.inputs.clear();
        g_seen.outputs.clear();
        for (std::size_t i = 0; i < audio.inputs.size(); ++i) {
            const auto& bus = audio.inputs[i];
            g_seen.inputs.push_back({bus.info.index, bus.info.role, bus.info.declared_channels,
                                     bus.active(), bus.num_channels()});
        }
        for (std::size_t i = 0; i < audio.outputs.size(); ++i) {
            const auto& bus = audio.outputs[i];
            g_seen.outputs.push_back({bus.info.index, bus.info.role, bus.info.declared_channels,
                                      bus.active(), bus.num_channels()});
        }

        const auto fill = [](pulp::format::ProcessBusBufferView<float>& out, float value) {
            for (std::size_t ch = 0; ch < out.num_channels(); ++ch)
                for (auto& sample : out.buffer.channel(ch))
                    sample = value;
        };
        // Main out: 1000 + the first main-input sample.
        // Stem A: 2000 + the aux input. Stem B: 3000 + the sidechain.
        const auto first = [](const pulp::format::ProcessBusBufferView<const float>& in) {
            return in.active() && in.num_channels() > 0 && in.num_samples() > 0
                       ? in.buffer.channel(0)[0]
                       : -1.0f;
        };
        fill(audio.outputs[0], 1000.0f + first(audio.inputs[0]));
        if (audio.outputs.size() > 1)
            fill(audio.outputs[1], 2000.0f + first(audio.inputs[2]));
        if (audio.outputs.size() > 2)
            fill(audio.outputs[2], 3000.0f + first(audio.inputs[1]));
    }
};

std::unique_ptr<pulp::format::Processor> create_multibus() {
    return std::make_unique<MultiBusProcessor>();
}

PulpAudioUnit* make_unit(OSType subtype) {
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Effect;
    desc.componentSubType = subtype;
    desc.componentManufacturer = 'Plup';
    NSError* error = nil;
    PulpAudioUnit* unit = [[PulpAudioUnit alloc] initWithComponentDescription:desc
                                                                      options:0
                                                                        error:&error];
    REQUIRE(unit != nil);
    REQUIRE(error == nil);
    return unit;
}

AVAudioFormat* standard(double rate, AVAudioChannelCount channels) {
    return [[[AVAudioFormat alloc] initStandardFormatWithSampleRate:rate
                                                           channels:channels] autorelease];
}

constexpr UInt32 kFrames = 32;

// A stereo or mono output buffer list, with real storage.
struct OutputList {
    std::array<std::array<float, kFrames>, 2> storage{};
    struct {
        AudioBufferList list;
        AudioBuffer extra[1];
    } abl{};
    explicit OutputList(UInt32 channels) {
        abl.list.mNumberBuffers = channels;
        for (UInt32 i = 0; i < channels; ++i) {
            abl.list.mBuffers[i].mNumberChannels = 1;
            abl.list.mBuffers[i].mDataByteSize = kFrames * sizeof(float);
            abl.list.mBuffers[i].mData = storage[i].data();
        }
    }
    AudioBufferList* list() {
        return &abl.list;
    }
};

} // namespace

TEST_CASE("AU v3 exposes one host bus per declared bus", "[au][auv3][bus]") {
    @autoreleasepool {
        ScopedFactoryRegistration registration(create_multibus);
        PulpAudioUnit* unit = make_unit('TstB');

        REQUIRE(unit.inputBusses.count == 3u);
        REQUIRE(unit.inputBusses[0].format.channelCount == 2u);
        REQUIRE(unit.inputBusses[1].format.channelCount == 1u);
        REQUIRE(unit.inputBusses[2].format.channelCount == 2u);
        REQUIRE([unit.inputBusses[2].name isEqualToString:@"Aux In"]);

        REQUIRE(unit.outputBusses.count == 3u);
        REQUIRE(unit.outputBusses[0].format.channelCount == 2u);
        REQUIRE(unit.outputBusses[1].format.channelCount == 2u);
        REQUIRE(unit.outputBusses[2].format.channelCount == 1u);
        REQUIRE([unit.outputBusses[1].name isEqualToString:@"Stem A"]);

        [unit release];
    }
}

TEST_CASE("AU v3 honors mono and stereo and refuses everything else", "[au][auv3][bus][format]") {
    @autoreleasepool {
        ScopedFactoryRegistration registration(create_multibus);
        PulpAudioUnit* unit = make_unit('TstB');
        NSError* error = nil;

        SECTION("mono and stereo are both accepted on a bus") {
            REQUIRE([unit.outputBusses[1] setFormat:standard(44100.0, 1) error:&error]);
            REQUIRE(unit.outputBusses[1].format.channelCount == 1u);
            REQUIRE(unit.outputBusses[1].format.sampleRate == 44100.0);
            REQUIRE([unit.inputBusses[2] setFormat:standard(48000.0, 1) error:&error]);
            REQUIRE([unit.inputBusses[2] setFormat:standard(48000.0, 2) error:&error]);
        }
        SECTION("a width the processor does not support is refused") {
            error = nil;
            REQUIRE_FALSE([unit.outputBusses[0] setFormat:standard(48000.0, 6) error:&error]);
            REQUIRE(unit.outputBusses[0].format.channelCount == 2u);
            REQUIRE_FALSE([unit.inputBusses[1] setFormat:standard(48000.0, 4) error:&error]);
            REQUIRE(unit.inputBusses[1].format.channelCount == 1u);
        }
        SECTION("interleaved and double-precision formats are refused") {
            AVAudioFormat* interleaved =
                [[[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                                  sampleRate:48000.0
                                                    channels:2
                                                 interleaved:YES] autorelease];
            AVAudioFormat* doubles =
                [[[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat64
                                                  sampleRate:48000.0
                                                    channels:2
                                                 interleaved:NO] autorelease];
            REQUIRE_FALSE([unit.outputBusses[0] setFormat:interleaved error:&error]);
            REQUIRE_FALSE([unit.outputBusses[0] setFormat:doubles error:&error]);
        }

        [unit release];
    }
}

TEST_CASE("AU v3 renders every output bus and routes every input bus", "[au][auv3][bus]") {
    @autoreleasepool {
        ScopedFactoryRegistration registration(create_multibus);
        PulpAudioUnit* unit = make_unit('TstB');
        g_seen = {};

        NSError* error = nil;
        REQUIRE([unit allocateRenderResourcesAndReturnError:&error]);

        // Each input bus supplies a distinct constant so a swapped route shows.
        __block std::vector<NSInteger> pulled;
        AURenderPullInputBlock pull =
            ^AUAudioUnitStatus(AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                               AUAudioFrameCount frames, NSInteger bus, AudioBufferList* list) {
              pulled.push_back(bus);
              const float value = bus == 0 ? 1.0f : bus == 1 ? 2.0f : 3.0f;
              for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
                  auto* data = static_cast<float*>(list->mBuffers[i].mData);
                  for (AUAudioFrameCount n = 0; n < frames; ++n)
                      data[n] = value;
              }
              return noErr;
            };

        AUInternalRenderBlock block = [unit internalRenderBlock];
        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = 0;

        OutputList main(2), stem_a(2), stem_b(1);
        // Ask for the aux bus first: the block is rendered once, whichever bus a
        // host asks for first.
        REQUIRE(block(&flags, &timestamp, kFrames, 1, stem_a.list(), nullptr, pull) == noErr);
        REQUIRE(block(&flags, &timestamp, kFrames, 0, main.list(), nullptr, pull) == noErr);
        REQUIRE(block(&flags, &timestamp, kFrames, 2, stem_b.list(), nullptr, pull) == noErr);

        REQUIRE(g_seen.blocks == 1);
        REQUIRE(main.storage[0][0] == 1001.0f);
        REQUIRE(main.storage[1][kFrames - 1] == 1001.0f);
        REQUIRE(stem_a.storage[0][0] == 2003.0f);
        REQUIRE(stem_a.storage[1][kFrames - 1] == 2003.0f);
        REQUIRE(stem_b.storage[0][0] == 3002.0f);
        // Each input bus was pulled once, by its host bus number.
        REQUIRE(pulled.size() == 3);

        // The processor saw one entry per declared bus, indexed as declared.
        REQUIRE(g_seen.inputs.size() == 3);
        REQUIRE(g_seen.outputs.size() == 3);
        REQUIRE(g_seen.inputs[2].index == 2);
        REQUIRE(g_seen.inputs[2].role == pulp::format::BusRole::Aux);
        REQUIRE(g_seen.inputs[2].channels == 2);
        REQUIRE(g_seen.inputs[2].active);
        REQUIRE(g_seen.outputs[1].role == pulp::format::BusRole::Aux);
        REQUIRE(g_seen.outputs[1].channels == 2);
        REQUIRE(g_seen.outputs[2].channels == 1);
        REQUIRE(g_seen.outputs[2].declared_channels == 1);

        // The next time stamp renders again.
        timestamp.mSampleTime = kFrames;
        REQUIRE(block(&flags, &timestamp, kFrames, 0, main.list(), nullptr, pull) == noErr);
        REQUIRE(g_seen.blocks == 2);

        // A bus the descriptor does not declare is silent, not an error.
        OutputList extra(2);
        extra.storage[0].fill(9.0f);
        REQUIRE(block(&flags, &timestamp, kFrames, 3, extra.list(), nullptr, pull) == noErr);
        REQUIRE(extra.storage[0][0] == 0.0f);

        [unit deallocateRenderResources];
        [unit release];
    }
}

TEST_CASE("AU v3 renders at the width the host negotiated", "[au][auv3][bus][format]") {
    @autoreleasepool {
        ScopedFactoryRegistration registration(create_multibus);
        PulpAudioUnit* unit = make_unit('TstB');
        g_seen = {};
        NSError* error = nil;

        REQUIRE([unit.outputBusses[1] setFormat:standard(48000.0, 1) error:&error]);
        REQUIRE([unit allocateRenderResourcesAndReturnError:&error]);

        AUInternalRenderBlock block = [unit internalRenderBlock];
        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        OutputList stem_a(1);
        REQUIRE(block(&flags, &timestamp, kFrames, 1, stem_a.list(), nullptr, nil) == noErr);

        REQUIRE(g_seen.outputs.size() == 3);
        REQUIRE(g_seen.outputs[1].channels == 1);

        [unit deallocateRenderResources];
        [unit release];
    }
}
