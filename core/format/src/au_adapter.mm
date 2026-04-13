// Audio Unit v3 adapter for Pulp
// Implements AUAudioUnit wrapping a Pulp Processor
// Built from Apple AudioToolbox documentation

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/runtime/log.hpp>
#include <memory>
#include <array>
#include <limits>

namespace pulp::format::au {

static constexpr int kMaxChannels = 8;

struct AUBridge {
    std::unique_ptr<Processor> processor;
    state::StateStore store;
    double sample_rate = 48000.0;
    AUAudioFrameCount max_frames = 512;
    int input_channels = 0;
    int output_channels = 0;

    // Pre-allocated — no heap allocation on audio thread
    float* output_ptrs[kMaxChannels] = {};
    const float* input_ptrs[kMaxChannels] = {};

    // Pre-allocated input buffer list for pulling input (stereo max for now)
    struct InputBufferStorage {
        UInt32 mNumberBuffers;
        AudioBuffer mBuffers[kMaxChannels];
    } input_abl = {};
};

} // namespace pulp::format::au

// ── AUAudioUnit subclass ───────────────────────────────────────────────────

@interface PulpAudioUnit : AUAudioUnit {
    pulp::format::au::AUBridge _bridge;
    AUAudioUnitBus *_inputBus;
    AUAudioUnitBus *_outputBus;
    AUAudioUnitBusArray *_inputBusArray;
    AUAudioUnitBusArray *_outputBusArray;
}

/// Raw pointer to the host-owned Processor + StateStore. Used by the
/// AUv3 view controller to construct a `ViewBridge` against the same
/// Processor that runs the audio callback (avoiding the dual-Processor
/// bug that the AU v2 path used to hit).
- (pulp::format::Processor *)pulpProcessor;
- (pulp::state::StateStore *)pulpStore;

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

    // Get processor factory from global registry
    auto factory = pulp::format::registered_factory();
    if (!factory) {
        if (outError) {
            *outError = [NSError errorWithDomain:@"com.pulp" code:-1
                userInfo:@{NSLocalizedDescriptionKey: @"No processor factory registered"}];
        }
        return nil;
    }

    _bridge.processor = factory();
    if (!_bridge.processor) {
        if (outError) {
            *outError = [NSError errorWithDomain:@"com.pulp" code:-2
                userInfo:@{NSLocalizedDescriptionKey: @"Processor factory returned null"}];
        }
        return nil;
    }
    _bridge.processor->set_state_store(&_bridge.store);
    _bridge.processor->define_parameters(_bridge.store);

    auto desc = _bridge.processor->descriptor();
    _bridge.input_channels = desc.default_input_channels();
    _bridge.output_channels = desc.default_output_channels();

    // Create buses
    AVAudioFormat *format = [[AVAudioFormat alloc]
        initStandardFormatWithSampleRate:48000.0
        channels:static_cast<AVAudioChannelCount>(std::max(desc.default_output_channels(), 1))];

    NSMutableArray *outBusses = [NSMutableArray new];
    NSMutableArray *inBusses = [NSMutableArray new];

    _outputBus = [[AUAudioUnitBus alloc] initWithFormat:format error:outError];
    if (!_outputBus) return nil;
    [outBusses addObject:_outputBus];

    if (desc.default_input_channels() > 0) {
        AVAudioFormat *inFormat = [[AVAudioFormat alloc]
            initStandardFormatWithSampleRate:48000.0
            channels:static_cast<AVAudioChannelCount>(desc.default_input_channels())];
        _inputBus = [[AUAudioUnitBus alloc] initWithFormat:inFormat error:outError];
        if (!_inputBus) return nil;
        [inBusses addObject:_inputBus];
    }

    _outputBusArray = [[AUAudioUnitBusArray alloc]
        initWithAudioUnit:self busType:AUAudioUnitBusTypeOutput busses:outBusses];
    _inputBusArray = [[AUAudioUnitBusArray alloc]
        initWithAudioUnit:self busType:AUAudioUnitBusTypeInput busses:inBusses];

    self.maximumFramesToRender = 512;

    pulp::runtime::log_info("AU: initialized '{}' (in:{} out:{})",
        desc.name, desc.default_input_channels(), desc.default_output_channels());
    return self;
}

// ── Bus access ─────────────────────────────────────────────────────────────

- (AUAudioUnitBusArray *)outputBusses { return _outputBusArray; }
- (AUAudioUnitBusArray *)inputBusses { return _inputBusArray; }

// ── Required property overrides ────────────────────────────────────────────

- (NSTimeInterval)latency {
    if (!_bridge.processor) return 0.0;
    int samples = _bridge.processor->latency_samples();
    return samples > 0 ? static_cast<double>(samples) / _bridge.sample_rate : 0.0;
}

- (NSTimeInterval)tailTime {
    if (!_bridge.processor) return 0.0;
    auto tail = _bridge.processor->descriptor().tail_samples;
    if (tail < 0) return std::numeric_limits<double>::infinity();
    return tail > 0 ? static_cast<double>(tail) / _bridge.sample_rate : 0.0;
}

- (BOOL)supportsUserPresets { return NO; }
- (BOOL)canProcessInPlace { return YES; }

- (AUParameterTree *)parameterTree {
    auto params = _bridge.store.all_params();
    NSMutableArray<AUParameter *> *auParams = [NSMutableArray new];

    for (const auto& p : params) {
        AUParameterAddress address = static_cast<AUParameterAddress>(p.id);
        NSString *identifier = [NSString stringWithFormat:@"param_%u", p.id];
        NSString *name = [NSString stringWithUTF8String:p.name.c_str()];

        // Map units
        AudioUnitParameterUnit unit = kAudioUnitParameterUnit_Generic;
        if (p.unit == "dB") unit = kAudioUnitParameterUnit_Decibels;
        else if (p.unit == "Hz") unit = kAudioUnitParameterUnit_Hertz;
        else if (p.unit == "%") unit = kAudioUnitParameterUnit_Percent;
        else if (p.range.step >= 1.0f && p.range.min == 0.0f && p.range.max == 1.0f)
            unit = kAudioUnitParameterUnit_Boolean;

        AUParameter *auParam = [AUParameterTree createParameterWithIdentifier:identifier
            name:name address:address min:p.range.min max:p.range.max
            unit:unit unitName:nil
            flags:kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable
            valueStrings:nil dependentParameters:nil];
        auParam.value = p.range.default_value;
        [auParams addObject:auParam];
    }

    AUParameterTree *tree = [AUParameterTree createTreeWithChildren:auParams];

    // Wire parameter changes from host to StateStore
    __weak typeof(self) weakSelf = self;
    tree.implementorValueObserver = ^(AUParameter *param, AUValue value) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_bridge.store.set_value(
            static_cast<pulp::state::ParamID>(param.address), value);
    };

    tree.implementorValueProvider = ^AUValue(AUParameter *param) {
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (!strongSelf) return 0.0f;
        return strongSelf->_bridge.store.get_value(
            static_cast<pulp::state::ParamID>(param.address));
    };

    tree.implementorStringFromValueCallback = ^NSString *(AUParameter *param, const AUValue *value) {
        AUValue v = value ? *value : param.value;
        __strong typeof(weakSelf) strongSelf = weakSelf;
        if (strongSelf) {
            auto* info = strongSelf->_bridge.store.info(
                static_cast<pulp::state::ParamID>(param.address));
            if (info && info->to_string) {
                auto str = info->to_string(v);
                return [NSString stringWithUTF8String:str.c_str()];
            }
        }
        return [NSString stringWithFormat:@"%.2f", v];
    };

    return tree;
}

- (BOOL)shouldBypassEffect { return NO; }
- (void)setShouldBypassEffect:(BOOL)bypass { (void)bypass; }

// ── Render resources ───────────────────────────────────────────────────────

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError {
    if (![super allocateRenderResourcesAndReturnError:outError]) return NO;

    _bridge.sample_rate = _outputBus.format.sampleRate;
    _bridge.max_frames = self.maximumFramesToRender;

    if (_bridge.processor) {
        pulp::format::PrepareContext ctx;
        ctx.sample_rate = _bridge.sample_rate;
        ctx.max_buffer_size = static_cast<int>(_bridge.max_frames);
        ctx.output_channels = _bridge.output_channels;
        ctx.input_channels = _bridge.input_channels;
        _bridge.processor->prepare(ctx);
    }

    pulp::runtime::log_info("AU: render resources at {} Hz, {} frames",
        _bridge.sample_rate, _bridge.max_frames);
    return YES;
}

- (void)deallocateRenderResources {
    if (_bridge.processor) _bridge.processor->release();
    [super deallocateRenderResources];
}

// ── Render block ───────────────────────────────────────────────────────────

- (AUInternalRenderBlock)internalRenderBlock {
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
            for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i)
                memset(outputData->mBuffers[i].mData, 0, outputData->mBuffers[i].mDataByteSize);
            return noErr;
        }

        UInt32 outChans = std::min(outputData->mNumberBuffers,
            static_cast<UInt32>(pulp::format::au::kMaxChannels));

        // Output buffer view (no allocation)
        for (UInt32 i = 0; i < outChans; ++i)
            bridge->output_ptrs[i] = static_cast<float*>(outputData->mBuffers[i].mData);
        pulp::audio::BufferView<float> output_view(bridge->output_ptrs, outChans, frameCount);

        // Input: pull from upstream if we have input channels
        pulp::audio::BufferView<const float> input_view;
        if (pullInputBlock && bridge->input_channels > 0) {
            auto& abl = bridge->input_abl;
            abl.mNumberBuffers = outChans;
            for (UInt32 i = 0; i < outChans; ++i) {
                abl.mBuffers[i].mNumberChannels = 1;
                abl.mBuffers[i].mDataByteSize = frameCount * sizeof(float);
                abl.mBuffers[i].mData = outputData->mBuffers[i].mData;
            }

            AudioUnitRenderActionFlags pullFlags = 0;
            auto status = pullInputBlock(&pullFlags, timestamp, frameCount, 0,
                reinterpret_cast<AudioBufferList*>(&abl));
            if (status == noErr) {
                for (UInt32 i = 0; i < outChans; ++i)
                    bridge->input_ptrs[i] = static_cast<const float*>(abl.mBuffers[i].mData);
                input_view = pulp::audio::BufferView<const float>(
                    bridge->input_ptrs, outChans, frameCount);
            }
        }

        // MIDI events
        pulp::midi::MidiBuffer midi_in, midi_out;
        const AURenderEvent* event = realtimeEventListHead;
        while (event) {
            if (event->head.eventType == AURenderEventMIDI) {
                const AUMIDIEvent& m = event->MIDI;
                pulp::midi::MidiEvent me;
                me.message = choc::midi::ShortMessage(
                    m.data[0],
                    m.length > 1 ? m.data[1] : uint8_t(0),
                    m.length > 2 ? m.data[2] : uint8_t(0));
                me.sample_offset = static_cast<int32_t>(event->head.eventSampleTime);
                midi_in.add(me);
            }
            event = event->head.next;
        }

        // Process
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

- (pulp::format::Processor *)pulpProcessor {
    return _bridge.processor.get();
}

- (pulp::state::StateStore *)pulpStore {
    return &_bridge.store;
}

@end
