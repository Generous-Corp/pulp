// Audio Unit v3 adapter for Pulp
// Implements AUAudioUnit wrapping a Pulp Processor
// Built from Apple AudioToolbox documentation (AUAudioUnit class reference)

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
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

    // Get the processor factory from the global registry
    _bridge.factory = pulp::format::registered_factory();
    if (!_bridge.factory) {
        pulp::runtime::log_error("AU: no processor factory registered — did you use PULP_REGISTER_PLUGIN?");
        if (outError) {
            *outError = [NSError errorWithDomain:@"com.pulp" code:-1
                userInfo:@{NSLocalizedDescriptionKey: @"No processor factory registered"}];
        }
        return nil;
    }

    // Create the processor
    _bridge.processor = _bridge.factory();
    if (!_bridge.processor) {
        pulp::runtime::log_error("AU: processor factory returned null");
        if (outError) {
            *outError = [NSError errorWithDomain:@"com.pulp" code:-2
                userInfo:@{NSLocalizedDescriptionKey: @"Processor factory returned null"}];
        }
        return nil;
    }
    _bridge.processor->set_state_store(&_bridge.store);
    _bridge.processor->define_parameters(_bridge.store);

    auto desc = _bridge.processor->descriptor();

    // Create buses based on processor descriptor
    AVAudioFormat *defaultFormat = [[AVAudioFormat alloc]
        initStandardFormatWithSampleRate:48000.0
        channels:static_cast<AVAudioChannelCount>(desc.default_output_channels)];

    NSMutableArray *outputBusses = [NSMutableArray new];
    NSMutableArray *inputBusses = [NSMutableArray new];

    if (desc.default_output_channels > 0) {
        _outputBus = [[AUAudioUnitBus alloc] initWithFormat:defaultFormat error:outError];
        if (!_outputBus) return nil;
        [outputBusses addObject:_outputBus];
    }

    if (desc.default_input_channels > 0) {
        AVAudioFormat *inputFormat = [[AVAudioFormat alloc]
            initStandardFormatWithSampleRate:48000.0
            channels:static_cast<AVAudioChannelCount>(desc.default_input_channels)];
        AUAudioUnitBus *inputBus = [[AUAudioUnitBus alloc] initWithFormat:inputFormat error:outError];
        if (!inputBus) return nil;
        [inputBusses addObject:inputBus];
    }

    _outputBusArray = [[AUAudioUnitBusArray alloc]
        initWithAudioUnit:self busType:AUAudioUnitBusTypeOutput busses:outputBusses];
    _inputBusArray = [[AUAudioUnitBusArray alloc]
        initWithAudioUnit:self busType:AUAudioUnitBusTypeInput busses:inputBusses];

    self.maximumFramesToRender = 512;

    pulp::runtime::log_info("AU: initialized '{}' (in:{} out:{})",
        desc.name, desc.default_input_channels, desc.default_output_channels);
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
        auto desc = _bridge.processor->descriptor();
        ctx.output_channels = desc.default_output_channels;
        ctx.input_channels = desc.default_input_channels;
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

        // Pull input audio if this is an effect (has input channels)
        pulp::audio::BufferView<const float> input_view;
        AudioBufferList inputBufferList;
        if (pullInputBlock) {
            // Set up input buffer list matching output format
            inputBufferList.mNumberBuffers = outputData->mNumberBuffers;
            for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i) {
                inputBufferList.mBuffers[i].mNumberChannels = 1;
                inputBufferList.mBuffers[i].mDataByteSize = frameCount * sizeof(float);
                inputBufferList.mBuffers[i].mData = outputData->mBuffers[i].mData; // In-place
            }

            AudioUnitRenderActionFlags pullFlags = 0;
            AUAudioUnitStatus pullStatus = pullInputBlock(&pullFlags, timestamp, frameCount, 0, &inputBufferList);
            if (pullStatus == noErr) {
                bridge->input_ptrs.resize(inputBufferList.mNumberBuffers);
                for (UInt32 i = 0; i < inputBufferList.mNumberBuffers; ++i) {
                    bridge->input_ptrs[i] = static_cast<const float*>(inputBufferList.mBuffers[i].mData);
                }
                input_view = pulp::audio::BufferView<const float>(
                    bridge->input_ptrs.data(),
                    bridge->input_ptrs.size(),
                    frameCount);
            }
        }

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
