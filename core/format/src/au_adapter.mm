// Audio Unit v3 adapter for Pulp
// Implements AUAudioUnit wrapping a Pulp Processor
// Built from Apple AudioToolbox documentation (AUAudioUnit class reference)

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <memory>

namespace pulp::format::au {

// C++ bridge: holds the Pulp processor and state
struct AUBridge {
    std::unique_ptr<Processor> processor;
    state::StateStore store;
    ProcessorFactory factory;
    double sample_rate = 48000.0;
    AUAudioFrameCount max_frames = 512;

    std::vector<float*> output_ptrs;
    std::vector<const float*> input_ptrs;
};

} // namespace pulp::format::au

// ── AUAudioUnit subclass ───────────────────────────────────────────────────

@interface PulpAudioUnit : AUAudioUnit {
    pulp::format::au::AUBridge _bridge;
    AUAudioUnitBus *_outputBus;
    AUAudioUnitBusArray *_outputBusArray;
    AUAudioUnitBusArray *_inputBusArray;
}
@end

@implementation PulpAudioUnit

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError **)outError
{
    self = [super initWithComponentDescription:componentDescription
                                       options:options
                                         error:outError];
    if (!self) return nil;

    // Create the processor
    if (_bridge.factory) {
        _bridge.processor = _bridge.factory();
        if (_bridge.processor) {
            _bridge.processor->set_state_store(&_bridge.store);
            _bridge.processor->define_parameters(_bridge.store);
        }
    }

    // Create output bus (stereo)
    AVAudioFormat *defaultFormat = [[AVAudioFormat alloc]
        initStandardFormatWithSampleRate:48000.0 channels:2];

    _outputBus = [[AUAudioUnitBus alloc] initWithFormat:defaultFormat error:outError];
    if (!_outputBus) return nil;

    _outputBusArray = [[AUAudioUnitBusArray alloc]
        initWithAudioUnit:self busType:AUAudioUnitBusTypeOutput busses:@[_outputBus]];
    _inputBusArray = [[AUAudioUnitBusArray alloc]
        initWithAudioUnit:self busType:AUAudioUnitBusTypeInput busses:@[]];

    self.maximumFramesToRender = 512;

    pulp::runtime::log_info("AU: initialized");
    return self;
}

- (AUAudioUnitBusArray *)outputBusses {
    return _outputBusArray;
}

- (AUAudioUnitBusArray *)inputBusses {
    return _inputBusArray;
}

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError {
    if (![super allocateRenderResourcesAndReturnError:outError]) return NO;

    _bridge.sample_rate = _outputBus.format.sampleRate;
    _bridge.max_frames = self.maximumFramesToRender;

    if (_bridge.processor) {
        pulp::format::PrepareContext ctx;
        ctx.sample_rate = _bridge.sample_rate;
        ctx.max_buffer_size = static_cast<int>(_bridge.max_frames);
        ctx.output_channels = 2;
        ctx.input_channels = 0;
        _bridge.processor->prepare(ctx);
    }

    pulp::runtime::log_info("AU: allocated render resources at {} Hz, max {} frames",
        _bridge.sample_rate, _bridge.max_frames);
    return YES;
}

- (void)deallocateRenderResources {
    if (_bridge.processor) {
        _bridge.processor->release();
    }
    [super deallocateRenderResources];
}

- (AUInternalRenderBlock)internalRenderBlock {
    // Capture bridge pointer for the render block
    auto* bridge = &_bridge;

    AUInternalRenderBlock renderBlock = ^AUAudioUnitStatus(
        AudioUnitRenderActionFlags *actionFlags,
        const AudioTimeStamp *timestamp,
        AUAudioFrameCount frameCount,
        NSInteger outputBusNumber,
        AudioBufferList *outputData,
        const AURenderEvent *realtimeEventListHead,
        AURenderPullInputBlock __unsafe_unretained pullInputBlock)
    {
        if (!bridge->processor) {
            // Silence
            for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i) {
                memset(outputData->mBuffers[i].mData, 0, outputData->mBuffers[i].mDataByteSize);
            }
            return noErr;
        }

        // Build output buffer view
        bridge->output_ptrs.resize(outputData->mNumberBuffers);
        for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i) {
            bridge->output_ptrs[i] = static_cast<float*>(outputData->mBuffers[i].mData);
        }

        pulp::audio::BufferView<float> output_view(
            bridge->output_ptrs.data(),
            bridge->output_ptrs.size(),
            frameCount);

        pulp::audio::BufferView<const float> input_view; // No input for instruments

        // Process MIDI events from the render event list
        pulp::midi::MidiBuffer midi_in, midi_out;
        const AURenderEvent* event = realtimeEventListHead;
        while (event) {
            if (event->head.eventType == AURenderEventMIDI) {
                const AUMIDIEvent& midi = event->MIDI;
                pulp::midi::MidiEvent me;
                me.data[0] = midi.data[0];
                me.data[1] = midi.length > 1 ? midi.data[1] : 0;
                me.data[2] = midi.length > 2 ? midi.data[2] : 0;
                me.size = static_cast<uint8_t>(midi.length);
                me.sample_offset = static_cast<int32_t>(event->head.eventSampleTime);
                midi_in.add(me);
            }
            event = event->head.next;
        }

        pulp::format::ProcessContext ctx;
        ctx.sample_rate = bridge->sample_rate;
        ctx.num_samples = static_cast<int>(frameCount);

        bridge->processor->process(output_view, input_view, midi_in, midi_out, ctx);

        return noErr;
    };
    return renderBlock;
}

// ── State persistence ──────────────────────────────────────────────────────

- (NSDictionary<NSString *, id> *)fullState {
    auto data = _bridge.store.serialize();
    NSData *nsData = [NSData dataWithBytes:data.data() length:data.size()];
    NSMutableDictionary *state = [[super fullState] mutableCopy] ?: [NSMutableDictionary new];
    state[@"pulpState"] = nsData;
    return state;
}

- (void)setFullState:(NSDictionary<NSString *, id> *)fullState {
    [super setFullState:fullState];
    NSData *nsData = fullState[@"pulpState"];
    if (nsData) {
        auto* bytes = static_cast<const uint8_t*>(nsData.bytes);
        _bridge.store.deserialize({bytes, nsData.length});
    }
}

@end

// ── Factory function ───────────────────────────────────────────────────────

namespace pulp::format::au {

void register_au_factory(ProcessorFactory factory) {
    // Store the factory for use by the AUAudioUnit subclass
    // In a real implementation, this would be called before the AU is instantiated
    // and the factory would be passed through to the AUAudioUnit init
}

} // namespace pulp::format::au
