// AUv3 MIDI output declaration.
//
// A host installs MIDIOutputEventBlock only for a unit that reports a MIDI
// output through MIDIOutputNames. These tests drive the real PulpAudioUnit the
// way a host does.
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

// Plain stereo effect that declares MIDI output and emits one note per block.
class MidiOutProcessor : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "AuMidiOut";
        d.manufacturer = "PulpTest";
        d.bundle_id = "com.pulp.test.au.midiout";
        d.version = "1.0.0";
        d.category = pulp::format::PluginCategory::Effect;
        d.input_buses = {{"In", 2}};
        d.output_buses = {{"Out", 2}};
        d.produces_midi = produces;
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext&) override {
        if (produces)
            midi_out.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    }
    static inline bool produces = true;
};

std::unique_ptr<pulp::format::Processor> create_midi_out() {
    return std::make_unique<MidiOutProcessor>();
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

TEST_CASE("AU v3 MIDIOutputNames reports MIDI output only for a processor that produces it",
          "[au][auv3][midi]") {
    @autoreleasepool {
        ScopedFactoryRegistration registration(create_midi_out);

        MidiOutProcessor::produces = true;
        PulpAudioUnit* producer = make_unit('TstO');
        REQUIRE(producer.MIDIOutputNames.count == 1u);
        REQUIRE([producer.MIDIOutputNames[0] isEqualToString:@"MIDI Out"]);
        [producer release];

        MidiOutProcessor::produces = false;
        PulpAudioUnit* silent = make_unit('TstP');
        REQUIRE(silent.MIDIOutputNames.count == 0u);
        [silent release];
        MidiOutProcessor::produces = true;
    }
}

TEST_CASE("AU v3 delivers processor MIDI output through the host's output block",
          "[au][auv3][midi]") {
    @autoreleasepool {
        ScopedFactoryRegistration registration(create_midi_out);
        MidiOutProcessor::produces = true;
        PulpAudioUnit* unit = make_unit('TstO');

        __block std::vector<std::array<uint8_t, 4>> received; // cable, b0, b1, b2
        unit.MIDIOutputEventBlock = ^AUAudioUnitStatus(AUEventSampleTime, uint8_t cable,
                                                       NSInteger length, const uint8_t* bytes) {
          REQUIRE(length == 3);
          received.push_back({cable, bytes[0], bytes[1], bytes[2]});
          return noErr;
        };

        NSError* error = nil;
        REQUIRE([unit allocateRenderResourcesAndReturnError:&error]);

        OutputList out(2);
        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        AUInternalRenderBlock block = [unit internalRenderBlock];
        REQUIRE(block(&flags, &timestamp, kFrames, 0, out.list(), nullptr, nil) == noErr);

        REQUIRE(received.size() == 1);
        REQUIRE(received[0] == std::array<uint8_t, 4>{0, 0x90, 60, 100});

        [unit deallocateRenderResources];
        [unit release];
    }
}
