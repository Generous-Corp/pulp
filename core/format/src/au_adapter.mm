// Audio Unit v3 adapter for Pulp
// Implements AUAudioUnit wrapping a Pulp Processor
// Built from Apple AudioToolbox documentation

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include <pulp/format/processor.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/format/ara.hpp>
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
    // Workstream 01 slice 1.4: sidechain bus channel count. 0 when the
    // descriptor has no second input bus — the render block then skips
    // the sidechain pull + calls Processor::set_sidechain(nullptr).
    int sidechain_channels = 0;

    // Pre-allocated — no heap allocation on audio thread
    float* output_ptrs[kMaxChannels] = {};
    const float* input_ptrs[kMaxChannels] = {};
    const float* sidechain_ptrs[kMaxChannels] = {};

    // Pre-allocated input buffer list for pulling input (stereo max for now)
    struct InputBufferStorage {
        UInt32 mNumberBuffers;
        AudioBuffer mBuffers[kMaxChannels];
    } input_abl = {};
    // Separate storage for the sidechain pull so it doesn't alias the
    // main input block.
    struct SidechainBufferStorage {
        UInt32 mNumberBuffers;
        AudioBuffer mBuffers[kMaxChannels];
    } sidechain_abl = {};
    // Backing buffer for the sidechain pull — AURenderPullInputBlock writes
    // into this on each process call. Keeps the audio thread allocation-free
    // (sized at init to match max_frames * kMaxChannels).
    std::vector<float> sidechain_storage;
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

/// ARA companion factory, surfaced under the KVO-standard property
/// name Apple's ARA-aware AU hosts observe ("audioUnitARAFactory" —
/// see pulp::format::kAuAraFactoryPropertyKey). Returns an opaque
/// ARA::ARAFactory* when Pulp was built with PULP_HAS_ARA and the
/// plug-in's Processor overrode create_ara_document_controller();
/// otherwise NULL. Issue #252.
@property (nonatomic, readonly, nullable) void *audioUnitARAFactory;

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
    _bridge.sidechain_channels =
        (desc.input_buses.size() > 1 && desc.input_buses[1].default_channels > 0)
            ? desc.input_buses[1].default_channels
            : 0;
    if (_bridge.sidechain_channels > 0) {
        _bridge.sidechain_storage.assign(
            _bridge.sidechain_channels * 512 /*max_frames hint*/, 0.0f);
    }

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

    // Workstream 01 slice 1.4 — sidechain input. When the descriptor declares
    // a second input_bus, expose it as a second AUAudioUnitBus so hosts can
    // connect a sidechain source that the render block will route through
    // Processor::set_sidechain(). Mirrors CLAP slice 1.1 / VST3 slice 1.2.
    if (desc.input_buses.size() > 1 && desc.input_buses[1].default_channels > 0) {
        AVAudioFormat *scFormat = [[AVAudioFormat alloc]
            initStandardFormatWithSampleRate:48000.0
            channels:static_cast<AVAudioChannelCount>(desc.input_buses[1].default_channels)];
        AUAudioUnitBus *sidechainBus =
            [[AUAudioUnitBus alloc] initWithFormat:scFormat error:outError];
        if (!sidechainBus) return nil;
        [inBusses addObject:sidechainBus];
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

        // Sidechain: pull bus 1 into its own ABL so it doesn't alias the
        // main input block. Processor::set_sidechain() takes a BufferView
        // that remains valid for the duration of process(). Workstream 01
        // slice 1.4.
        pulp::audio::BufferView<const float> sidechain_view;
        int scChans = bridge->sidechain_channels;
        if (pullInputBlock && scChans > 0) {
            UInt32 scBufs = std::min(static_cast<UInt32>(scChans),
                                     static_cast<UInt32>(kMaxChannels));
            // Size sidechain_storage for this block if needed (rare —
            // max_frames should cover; guard anyway).
            std::size_t needed = static_cast<std::size_t>(scBufs) * frameCount;
            if (bridge->sidechain_storage.size() < needed) {
                bridge->sidechain_storage.assign(needed, 0.0f);
            }
            auto& scAbl = bridge->sidechain_abl;
            scAbl.mNumberBuffers = scBufs;
            for (UInt32 i = 0; i < scBufs; ++i) {
                scAbl.mBuffers[i].mNumberChannels = 1;
                scAbl.mBuffers[i].mDataByteSize = frameCount * sizeof(float);
                scAbl.mBuffers[i].mData =
                    bridge->sidechain_storage.data() + i * frameCount;
            }
            AudioUnitRenderActionFlags pullFlags = 0;
            // Input bus index 1 is the sidechain, per the bus-array init
            // order in initWithComponentDescription.
            auto scStatus = pullInputBlock(
                &pullFlags, timestamp, frameCount, 1,
                reinterpret_cast<AudioBufferList*>(&scAbl));
            if (scStatus == noErr) {
                for (UInt32 i = 0; i < scBufs; ++i) {
                    bridge->sidechain_ptrs[i] =
                        static_cast<const float*>(scAbl.mBuffers[i].mData);
                }
                sidechain_view = pulp::audio::BufferView<const float>(
                    bridge->sidechain_ptrs, scBufs, frameCount);
                bridge->processor->set_sidechain(&sidechain_view);
            } else {
                bridge->processor->set_sidechain(nullptr);
            }
        } else {
            bridge->processor->set_sidechain(nullptr);
        }

        // MIDI events. Short messages arrive as AURenderEventMIDI;
        // sysex (and long MIDI 2.0 UMP groups from AU v3.1+) arrive
        // as AURenderEventMIDIEventList. Workstream 01 #288 completes
        // the sysex triad — CLAP half #269, VST3 half #274, AU half
        // here — by routing long packets into MidiBuffer's variable-
        // length sysex sidecar (#231).
        pulp::midi::MidiBuffer midi_in, midi_out;
        const AURenderEvent* event = realtimeEventListHead;
        while (event) {
            if (event->head.eventType == AURenderEventMIDI) {
                const AUMIDIEvent& m = event->MIDI;
                // A well-formed short MIDI message has a status byte
                // (MSB set) and total length 1..3. Skip anything else.
                if (m.length >= 1 && m.length <= 3 && (m.data[0] & 0x80)) {
                    pulp::midi::MidiEvent me;
                    me.message = choc::midi::ShortMessage(
                        m.data[0],
                        m.length > 1 ? m.data[1] : uint8_t(0),
                        m.length > 2 ? m.data[2] : uint8_t(0));
                    me.sample_offset =
                        static_cast<int32_t>(event->head.eventSampleTime);
                    midi_in.add(me);
                }
            } else if (event->head.eventType == AURenderEventMIDIEventList) {
                // AUMIDIEventList delivers UMP-encoded events. For
                // sysex the UMP type-3 (7-bit Data Messages, status
                // 0x3x) stream carries the F0..F7 payload spread
                // across 8-byte packets with continuation flags.
                // We reassemble into a single std::vector<uint8_t>
                // and hand it to MidiBuffer::add_sysex.
                const auto& elist = event->MIDIEventsList;
                const MIDIEventList* packets = &elist.eventList;
                if (packets) {
                    std::vector<uint8_t> payload;
                    const MIDIEventPacket* pkt = &packets->packet[0];
                    for (UInt32 i = 0; i < packets->numPackets; ++i) {
                        // Each packet's `words[0..wordCount]` holds
                        // UMP 32-bit words. Type-3 (sysex7) status
                        // lives in the top nibble of word 0.
                        for (UInt32 w = 0; w < pkt->wordCount; ++w) {
                            const uint32_t word = pkt->words[w];
                            const uint8_t mt = (word >> 28) & 0x0F;
                            if (mt != 0x3) continue; // not sysex7 UMP
                            // word bytes: [mt/group][status|size][data..]
                            const uint32_t size = (word >> 16) & 0x0F;
                            const uint8_t b2 = (word >>  8) & 0xFF;
                            const uint8_t b3 = (word >>  0) & 0xFF;
                            if (size >= 1) payload.push_back(b2);
                            if (size >= 2) payload.push_back(b3);
                            // Additional 4 data bytes live in word 1
                            // for size > 2; walk them here.
                            if (w + 1 < pkt->wordCount && size > 2) {
                                const uint32_t w1 = pkt->words[w + 1];
                                const uint8_t extras[4] = {
                                    static_cast<uint8_t>((w1 >> 24) & 0xFF),
                                    static_cast<uint8_t>((w1 >> 16) & 0xFF),
                                    static_cast<uint8_t>((w1 >>  8) & 0xFF),
                                    static_cast<uint8_t>((w1 >>  0) & 0xFF),
                                };
                                for (uint32_t e = 0; e < size - 2 && e < 4; ++e)
                                    payload.push_back(extras[e]);
                            }
                        }
                        pkt = reinterpret_cast<const MIDIEventPacket*>(
                            reinterpret_cast<const uint8_t*>(pkt) +
                            sizeof(MIDIEventPacket) +
                            (pkt->wordCount > 0 ? (pkt->wordCount - 1) * sizeof(UInt32) : 0));
                    }
                    if (!payload.empty()) {
                        midi_in.add_sysex(std::move(payload),
                            static_cast<int32_t>(event->head.eventSampleTime),
                            0.0);
                    }
                }
            }
            event = event->head.next;
        }

        // Process
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = bridge->sample_rate;
        ctx.num_samples = static_cast<int>(frameCount);
        bridge->processor->process(output_view, input_view, midi_in, midi_out, ctx);

        // Forward any MIDI the Processor emitted to the host via the
        // AUv3 MIDIOutputEventBlock. The block is installed by the host
        // on an ARC-managed retained property; we capture it via `self`
        // at block-creation time so the retain cycle stays safe.
        // Workstream 01 — AUv3 MIDI-out (#242).
        if (midi_out.size() > 0) {
            AUMIDIOutputEventBlock outBlock = self.MIDIOutputEventBlock;
            if (outBlock) {
                for (const auto& e : midi_out) {
                    const AUEventSampleTime sampleTime =
                        static_cast<AUEventSampleTime>(
                            timestamp->mSampleTime + e.sample_offset);
                    uint8_t bytes[3] = {0, 0, 0};
                    const uint32_t n = e.message.length();
                    if (n >= 1) bytes[0] = e.data()[0];
                    if (n >= 2) bytes[1] = e.data()[1];
                    if (n >= 3) bytes[2] = e.data()[2];
                    // cable = 0 (single-port AUv3), length = short-msg size.
                    outBlock(sampleTime, 0, static_cast<NSInteger>(n), bytes);
                }
            }
        }

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

// ARA-aware AU hosts (Logic Pro 11+, etc.) read this property via KVO
// during scan. Returns an opaque ARA::ARAFactory* when the plug-in
// participates in ARA; nullptr otherwise. See issue #252 and the
// A2c ralph slice.
- (void *)audioUnitARAFactory {
    return const_cast<void *>(
        pulp::format::ara_companion_factory_for(nullptr));
}

@end
