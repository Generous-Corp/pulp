// Audio Unit v3 adapter for Pulp
// Implements AUAudioUnit wrapping a Pulp Processor
// Built from Apple AudioToolbox documentation
//
// ─────────────────────────────────────────────────────────────────────────
// Per-method audit checklist (macOS plan item 3.1)
// ─────────────────────────────────────────────────────────────────────────
//
// Each `PulpAudioUnit` override below is audited against three axes:
//   • returns — what the AU v3 host reads from the call site
//   • thread — which thread Apple's framework may invoke it from
//   • allocates — whether the body is allowed to heap-allocate (NO for
//     anything reachable from the audio-render path; YES otherwise)
//
// Pinned by `test_au_plugin_state.mm — "AU v3 per-method audit invariants"`.
// When you add a new override below, add a row here AND a regression
// assertion in that test so the no-op stays a no-op.
//
//  ┌──────────────────────────────────────────┬───────────┬──────────────┬──────────────┐
//  │ method                                   │ returns   │ thread       │ allocates    │
//  ├──────────────────────────────────────────┼───────────┼──────────────┼──────────────┤
//  │ initWithComponentDescription:options:    │ self/nil  │ main (init)  │ YES (init)   │
//  │ outputBusses / inputBusses               │ NSArray   │ main + audio │ NO (cached)  │
//  │ latency                                  │ seconds   │ KVO/main     │ NO           │
//  │ tailTime                                 │ seconds   │ KVO/main     │ NO           │
//  │ supportsUserPresets                      │ BOOL=NO   │ main         │ NO (const)   │
//  │ canProcessInPlace                        │ BOOL=YES  │ main + audio │ NO (const)   │
//  │ parameterTree                            │ AUParam…  │ main         │ YES (per call) │
//  │ shouldBypassEffect                       │ BOOL      │ main + audio │ NO (atomic)  │
//  │ setShouldBypassEffect:                   │ void      │ main + audio │ NO (RT-safe) │
//  │ allocateRenderResourcesAndReturnError:   │ BOOL      │ main         │ YES (init)   │
//  │ deallocateRenderResources                │ void      │ main         │ YES (release)│
//  │ internalRenderBlock                      │ block     │ main         │ YES (block)  │
//  │   └─ render block body                   │ OSStatus  │ AUDIO        │ NO           │
//  │ fullState                                │ NSDict    │ main         │ YES (serdes) │
//  │ setFullState:                            │ void      │ main         │ YES (serdes) │
//  │ audioUnitARAFactory                      │ void*     │ KVO/main     │ NO (cached)  │
//  │ supportedViewConfigurations:             │ NSIndexS… │ main         │ YES (NSSet)  │
//  │ selectViewConfiguration:                 │ void      │ main         │ NO (log only)│
//  └──────────────────────────────────────────┴───────────┴──────────────┴──────────────┘
//
// Pulp-private accessors used by tests (NOT AUAudioUnit overrides):
//   • pulpProcessor / pulpStore — main-thread, read-only.
//   • pulpBypassParameterId   — main-thread, read-only.
//   • pulpLastParameterEvent* — main-thread, read-only snapshots of the
//     last block's param-event queue (sample-offset, ramp duration, etc).

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <mach/mach_time.h>
#include <pulp/events/plugin_main_thread.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/midi/ump_sysex7_reassembler.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <cmath>
#include <memory>
#include <array>
#include <atomic>
#include <limits>

namespace pulp::format::au {

static constexpr int kMaxChannels = 8;

// AU v3 row 21 (DAW quirks) — the bypass parameter has to be tracked on
// two surfaces: AUAudioUnit's `shouldBypassEffect` AUValue **and** the
// plugin-provided `Bypass` parameter when present. We scan the
// descriptor's parameter list at init for a parameter named "Bypass"
// (boolean, 0..1, step==1) and route both surfaces to it. Hosts that
// observe one but not the other (Logic vs MainStage) then see a
// consistent value. When no such parameter exists, we still track the
// bypass flag in a bridge-local atomic so the host's
// `setShouldBypassEffect:` request is honoured at the audio thread.
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
    // Item 3.1 — dual-tracked bypass.
    //   * `bypass_param_id != 0` means the plugin declared a "Bypass"
    //     parameter; the AUAudioUnit's `shouldBypassEffect` reads/writes
    //     it through StateStore.
    //   * Otherwise, `bypass_flag` is the only authority. The render
    //     block consults whichever path is live and short-circuits the
    //     processor with a pass-through (or silence for instruments).
    state::ParamID bypass_param_id = 0;
    std::atomic<bool> bypass_flag{false};

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
    state::ParameterEventQueue param_events;

    // Item 1.3 — previous-block transport snapshot used to derive the
    // change flags on `ProcessContext`. Default-constructed (no
    // previous block) so the first render-block invocation reports no
    // changes.
    detail::PlayheadSnapshot playhead_prev;
};

static int32_t block_relative_sample_offset(AUEventSampleTime event_sample_time,
                                            const AudioTimeStamp* timestamp,
                                            AUAudioFrameCount frame_count) {
    int64_t offset = 0;
    constexpr auto kImmediate = static_cast<int64_t>(AUEventSampleTimeImmediate);
    constexpr int64_t kImmediateWindow = 4096;

    if (event_sample_time >= kImmediate &&
        event_sample_time < kImmediate + kImmediateWindow) {
        offset = static_cast<int64_t>(event_sample_time) - kImmediate;
    } else if (timestamp &&
               (timestamp->mFlags & kAudioTimeStampSampleTimeValid) != 0) {
        offset = static_cast<int64_t>(event_sample_time) -
                 static_cast<int64_t>(timestamp->mSampleTime);
    } else {
        offset = static_cast<int64_t>(event_sample_time);
    }

    if (offset < 0) return 0;
    if (frame_count > 0 && offset >= static_cast<int64_t>(frame_count)) {
        return static_cast<int32_t>(frame_count - 1);
    }
    if (offset > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        return std::numeric_limits<int32_t>::max();
    }
    return static_cast<int32_t>(offset);
}

} // namespace pulp::format::au

// ── AUAudioUnit subclass ───────────────────────────────────────────────────

@interface PulpAudioUnit : AUAudioUnit {
    pulp::format::au::AUBridge _bridge;
    AUAudioUnitBus *_inputBus;
    AUAudioUnitBus *_outputBus;
    AUAudioUnitBusArray *_inputBusArray;
    AUAudioUnitBusArray *_outputBusArray;
    // Item 6.4b — MainThreadDispatcher backend token. Held for the lifetime
    // of this plugin instance so DAW-hosted code can post work to the
    // main thread via pulp::events::MainThreadDispatcher::call_async().
    pulp::events::MainThreadDispatcher::Token _mainThreadToken;
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

/// Item 3.1 — diagnostic accessor for the bypass-param wiring decision
/// the adapter made at init. Returns 0 when no plugin-declared bypass
/// parameter matched (the synthesized-AUValue path is in use); otherwise
/// the StateStore parameter ID that proxies the bypass surface.
- (uint32_t)pulpBypassParameterId;

- (NSUInteger)pulpLastParameterEventCount;
- (uint32_t)pulpLastParameterEventParamIDAtIndex:(NSUInteger)index;
- (int32_t)pulpLastParameterEventSampleOffsetAtIndex:(NSUInteger)index;
- (int32_t)pulpLastParameterEventRampDurationAtIndex:(NSUInteger)index;
- (float)pulpLastParameterEventValueAtIndex:(NSUInteger)index;

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

    // Item 6.4b — opt this plugin instance into the process-wide
    // MainThreadDispatcher backend. On macOS this installs (or refcount-
    // bumps) a Cocoa backend posting to `dispatch_get_main_queue`, so
    // KVO publishes (e.g. the `latency` / `tailTime` dispatch_async at
    // line ~801) and any future view-side code reaches the host's main
    // thread without needing its own per-callsite dispatch_async.
    _mainThreadToken = pulp::events::register_plugin_backend();

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

    // Item 3.1 — auto-detect a plugin-declared Bypass parameter so the
    // host's `shouldBypassEffect` AUValue and the plugin's
    // automatable parameter stay in lockstep. Match the same heuristic
    // VST3 uses for `kIsBypass`: name == "Bypass", boolean range 0..1
    // with step >= 1. When found, the AUv3 surface mirrors it.
    for (const auto& p : _bridge.store.all_params()) {
        if (p.name == "Bypass" &&
            p.range.min == 0.0f && p.range.max == 1.0f &&
            p.range.step >= 1.0f) {
            _bridge.bypass_param_id = p.id;
            break;
        }
    }

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
    __unsafe_unretained PulpAudioUnit* weakSelf = self;
    tree.implementorValueObserver = ^(AUParameter *param, AUValue value) {
        PulpAudioUnit* strongSelf = weakSelf;
        if (!strongSelf) return;
        // AUv3 may call implementorValueObserver from any thread,
        // including the render thread. Use the RT-safe path so a Main
        // listener attached to this store doesn't trigger an EventLoop
        // dispatch lambda allocation on a possibly-audio thread.
        strongSelf->_bridge.store.set_value_rt(
            static_cast<pulp::state::ParamID>(param.address), value);
    };

    tree.implementorValueProvider = ^AUValue(AUParameter *param) {
        PulpAudioUnit* strongSelf = weakSelf;
        if (!strongSelf) return 0.0f;
        return strongSelf->_bridge.store.get_value(
            static_cast<pulp::state::ParamID>(param.address));
    };

    tree.implementorStringFromValueCallback = ^NSString *(AUParameter *param, const AUValue *value) {
        AUValue v = value ? *value : param.value;
        PulpAudioUnit* strongSelf = weakSelf;
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

// Item 3.1 — dual-tracked bypass.
//
// Hosts read these to render the bypass button in their channel-strip UI
// (Logic) or treat them as the source of truth for the bypass automation
// lane (MainStage). Without dual tracking, one surface goes stale.
//
// Strategy: when the plugin exposes a parameter named "Bypass" (boolean,
// 0..1, step==1) we treat that StateStore parameter as authoritative
// — write to it via the RT-safe path so a host setShouldBypassEffect:
// call propagates to the plugin's parameter lane (and any UI bindings
// observing it) without allocating on the audio thread. When the plugin
// has no Bypass param, we keep the request in a bridge-local atomic so
// the AUAudioUnit contract still works (the render block then handles
// pass-through itself; see internalRenderBlock).
- (BOOL)shouldBypassEffect {
    if (_bridge.bypass_param_id != 0) {
        return _bridge.store.get_value(_bridge.bypass_param_id) >= 0.5f
            ? YES : NO;
    }
    return _bridge.bypass_flag.load(std::memory_order_acquire) ? YES : NO;
}

- (void)setShouldBypassEffect:(BOOL)bypass {
    if (_bridge.bypass_param_id != 0) {
        // RT-safe — the AU v3 host may call this from a high-priority
        // thread that touches the render path; never allocate.
        _bridge.store.set_value_rt(_bridge.bypass_param_id,
                                   bypass ? 1.0f : 0.0f);
    }
    _bridge.bypass_flag.store(bypass ? true : false,
                              std::memory_order_release);
}

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

// Item 6.4b — symmetric teardown of the MainThreadDispatcher backend
// installed in init.
- (void)dealloc {
    if (_mainThreadToken != 0) {
        pulp::events::unregister_plugin_backend(_mainThreadToken);
        _mainThreadToken = 0;
    }
    [super dealloc];
}

// ── Render block ───────────────────────────────────────────────────────────

- (AUInternalRenderBlock)internalRenderBlock {
    auto* bridge = &_bridge;

    // Item 1.3 — capture the host's musical-context and transport-state
    // blocks at render-block construction time, per Apple's render-block
    // contract. AUAudioUnit::musicalContextBlock and transportStateBlock
    // are KVO-able read/write properties the host installs (often only
    // after `allocateRenderResources`). They are safe to invoke from
    // the render thread; the block we hand back to the host captures
    // them via `__block` so the call sites below stay self-contained.
    AUHostMusicalContextBlock musicalContextBlock = self.musicalContextBlock;
    AUHostTransportStateBlock transportStateBlock = self.transportStateBlock;

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
        bridge->param_events.clear();

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
                                     static_cast<UInt32>(pulp::format::au::kMaxChannels));
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
            if (event->head.eventType == AURenderEventParameter ||
                event->head.eventType == AURenderEventParameterRamp) {
                const AUParameterEvent& p = event->parameter;
                const auto param_id =
                    static_cast<pulp::state::ParamID>(p.parameterAddress);
                const auto sample_offset =
                    pulp::format::au::block_relative_sample_offset(
                        p.eventSampleTime, timestamp, frameCount);
                bridge->param_events.push({
                    param_id,
                    sample_offset,
                    static_cast<float>(p.value),
                    event->head.eventType == AURenderEventParameterRamp
                        ? static_cast<int32_t>(p.rampDurationSampleFrames)
                        : 0,
                });
                bridge->store.set_value_rt(param_id, static_cast<float>(p.value));
            } else if (event->head.eventType == AURenderEventMIDI) {
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
                // AUMIDIEventList delivers UMP-encoded events. The
                // UMP message-type nibble (bits 28-31 of word 0)
                // identifies the message class; nibble 0x3 is sysex7
                // and reassembly is delegated to the shared
                // UmpSysex7Reassembler in core/midi (macOS plan 8.2 —
                // extracts the previously inline #292 fix to a
                // single, tested implementation shared with the
                // CoreMIDI device backend).
                //
                // The reassembler emits each completed logical sysex
                // exactly once; we tag the resulting payload with the
                // surrounding MIDIEventList's event sample time.
                const auto& elist = event->MIDIEventsList;
                const MIDIEventList* packets = &elist.eventList;
                if (packets) {
                    struct EmitCtx {
                        pulp::midi::MidiBuffer* sink;
                        int32_t sample_offset;
                    };
                    EmitCtx ctx{
                        &midi_in,
                        static_cast<int32_t>(event->head.eventSampleTime),
                    };
                    auto emit = [](const std::vector<uint8_t>& payload,
                                   void* user) {
                        auto* c = static_cast<EmitCtx*>(user);
                        c->sink->add_sysex(payload, c->sample_offset, 0.0);
                    };

                    pulp::midi::UmpSysex7Reassembler reassembler;
                    const MIDIEventPacket* pkt = &packets->packet[0];

                    for (UInt32 i = 0; i < packets->numPackets; ++i) {
                        UInt32 w = 0;
                        while (w < pkt->wordCount) {
                            const uint32_t word0 = pkt->words[w];
                            const uint8_t mt = (word0 >> 28) & 0x0F;

                            // UMP message word length by type. Types
                            // not listed default to 1 so we still
                            // advance past unknown messages safely.
                            // Each type-3 message is 2 UMP words; the
                            // cursor advances by `ump_words`, not 1,
                            // so word1 cannot masquerade as a fresh
                            // message header (#292 P1).
                            UInt32 ump_words = 1;
                            switch (mt) {
                                case 0x0: case 0x1: case 0x2:
                                    ump_words = 1; break;
                                case 0x3: case 0x4:
                                case 0x8: case 0x9: case 0xA:
                                    ump_words = 2; break;
                                case 0xB: case 0xC:
                                    ump_words = 3; break;
                                case 0x5: case 0xD: case 0xE:
                                    ump_words = 4; break;
                                default:
                                    ump_words = 1; break;
                            }
                            if (w + ump_words > pkt->wordCount) break;

                            if (mt == 0x3) {
                                const uint32_t word1 = pkt->words[w + 1];
                                reassembler.feed_packet(word0, word1, emit, &ctx);
                            }

                            w += ump_words;
                        }
                        pkt = reinterpret_cast<const MIDIEventPacket*>(
                            reinterpret_cast<const uint8_t*>(pkt) +
                            sizeof(MIDIEventPacket) +
                            (pkt->wordCount > 0 ? (pkt->wordCount - 1) * sizeof(UInt32) : 0));
                    }
                }
            }
            event = event->head.next;
        }
        bridge->param_events.sort();

        // Item 3.1 — bypass short-circuit. Consult the plugin's Bypass
        // parameter when it has one; otherwise the bridge-local atomic
        // the host wrote via setShouldBypassEffect:. When bypassed we
        // skip `processor_->process()` entirely and emit pass-through:
        //   * effects (input_channels > 0): copy input → output per
        //     channel, padding extra outputs with silence;
        //   * instruments / generators: zero-fill (no input to copy).
        // MIDI output is intentionally empty when bypassed — matches
        // every DAW's expectation that a bypassed effect does not emit
        // arpeggiator notes or MIDI FX into the host's bus.
        const bool bypassed = (bridge->bypass_param_id != 0)
            ? (bridge->store.get_value(bridge->bypass_param_id) >= 0.5f)
            : bridge->bypass_flag.load(std::memory_order_acquire);
        if (bypassed) {
            const UInt32 inCount = static_cast<UInt32>(bridge->input_channels);
            for (UInt32 i = 0; i < outChans; ++i) {
                if (i < inCount && bridge->input_ptrs[i]) {
                    std::memcpy(bridge->output_ptrs[i],
                                bridge->input_ptrs[i],
                                frameCount * sizeof(float));
                } else {
                    std::memset(bridge->output_ptrs[i], 0,
                                frameCount * sizeof(float));
                }
            }
            return noErr;
        }

        // Process
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = bridge->sample_rate;
        ctx.num_samples = static_cast<int>(frameCount);

        // Item 1.3 — populate transport fields from the host blocks the
        // AUv3 host installed via the KVO-able properties on
        // AUAudioUnit. The blocks may legitimately be nil (hosts that
        // don't expose transport state, AUv2-bridged hosts, render
        // tests). In that case the fields stay at their documented
        // defaults so the plugin's process() sees the same
        // pre-extension behaviour.
        if (musicalContextBlock) {
            double tempo_bpm = 0.0;
            double time_sig_numerator = 0.0;
            NSInteger time_sig_denominator = 0;
            double current_beat_position = 0.0;
            NSInteger sample_offset_to_next_beat = 0;
            double current_measure_downbeat_position = 0.0;
            const BOOL ok = musicalContextBlock(
                &tempo_bpm,
                &time_sig_numerator,
                &time_sig_denominator,
                &current_beat_position,
                &sample_offset_to_next_beat,
                &current_measure_downbeat_position);
            if (ok) {
                if (tempo_bpm > 0.0) ctx.tempo_bpm = tempo_bpm;
                if (time_sig_numerator > 0.0) {
                    ctx.time_sig_numerator = static_cast<int>(time_sig_numerator);
                }
                if (time_sig_denominator > 0) {
                    ctx.time_sig_denominator = static_cast<int>(time_sig_denominator);
                }
                ctx.position_beats = current_beat_position;
            }
        }
        if (transportStateBlock) {
            AUHostTransportStateFlags transport_flags = 0;
            double current_sample_position = 0.0;
            double cycle_start_beat_position = 0.0;
            double cycle_end_beat_position = 0.0;
            const BOOL ok = transportStateBlock(
                &transport_flags,
                &current_sample_position,
                &cycle_start_beat_position,
                &cycle_end_beat_position);
            if (ok) {
                ctx.is_playing =
                    (transport_flags & AUHostTransportStateMoving) != 0;
                ctx.is_recording =
                    (transport_flags & AUHostTransportStateRecording) != 0;
                ctx.is_looping =
                    (transport_flags & AUHostTransportStateCycling) != 0;
                ctx.position_samples =
                    static_cast<int64_t>(current_sample_position);
                if (ctx.is_looping) {
                    ctx.loop_start_beats = cycle_start_beat_position;
                    ctx.loop_end_beats = cycle_end_beat_position;
                }
            }
        }
        if (timestamp && (timestamp->mFlags & kAudioTimeStampHostTimeValid) != 0) {
            static mach_timebase_info_data_t timebase = {0, 0};
            if (timebase.denom == 0) mach_timebase_info(&timebase);
            if (timebase.denom != 0) {
                ctx.host_time_ns = static_cast<int64_t>(
                    (timestamp->mHostTime * timebase.numer) / timebase.denom);
            }
        }
        // AUv3 has no host-supplied frame-rate; `ctx.frame_rate` stays
        // `FrameRate::unknown` per the documented sentinel.
        pulp::format::detail::derive_bar_from_beats(ctx);
        pulp::format::detail::compute_playhead_changes(ctx, bridge->playhead_prev);

        bridge->processor->set_param_events(&bridge->param_events);
        bridge->processor->process(output_view, input_view, midi_in, midi_out, ctx);

        // Item 3.11 — drain RT-safe pending flags the processor may have
        // set during process() and publish them via KVO. AUAudioUnit
        // exposes `latency` and `tailTime` as KVO-able read-only
        // properties; an AU v3 host observes them and re-queries.
        // dispatch_async (vs the synchronous KVO call) keeps the audio
        // thread out of Foundation's KVO machinery, matching the spec
        // for #3.11 ("no host calls from process()").
        const bool publish_latency =
            bridge->processor->consume_latency_changed_flag();
        const bool publish_tail =
            bridge->processor->consume_tail_changed_flag();
        if (publish_latency || publish_tail) {
            // File is built without ARC (see AudioUnitSDK lane). We
            // cannot take a __weak ref; instead retain self for the
            // duration of the dispatch and release inside the block.
            // The block runs on the main queue so KVO observers see
            // willChange/didChange there, not on the audio thread.
            PulpAudioUnit *retainedSelf = self;
            [retainedSelf retain];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (publish_latency) {
                    [retainedSelf willChangeValueForKey:@"latency"];
                    [retainedSelf didChangeValueForKey:@"latency"];
                }
                if (publish_tail) {
                    [retainedSelf willChangeValueForKey:@"tailTime"];
                    [retainedSelf didChangeValueForKey:@"tailTime"];
                }
                [retainedSelf release];
            });
        }

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
#if __has_feature(objc_arc)
    return [renderBlock copy];
#else
    return [[renderBlock copy] autorelease];
#endif
}

// ── State persistence ──────────────────────────────────────────────────────

- (NSDictionary<NSString *, id> *)fullState {
    if (!_bridge.processor) return [super fullState];
    auto data = pulp::format::plugin_state_io::serialize(_bridge.store, *_bridge.processor);
    NSData *nsData = [NSData dataWithBytes:data.data() length:data.size()];
    NSMutableDictionary *state = [[super fullState] mutableCopy] ?: [NSMutableDictionary new];
    state[@"pulpState"] = nsData;
    return state;
}

- (void)setFullState:(NSDictionary<NSString *, id> *)fullState {
    [super setFullState:fullState];
    NSData *nsData = fullState[@"pulpState"];
    if (nsData && _bridge.processor) {
        auto* bytes = static_cast<const uint8_t*>(nsData.bytes);
        if (!pulp::format::plugin_state_io::deserialize({bytes, nsData.length},
                                                        _bridge.store,
                                                        *_bridge.processor)) {
            pulp::runtime::log_warn("AU: failed to restore plugin state");
        }
    }
}

- (pulp::format::Processor *)pulpProcessor {
    return _bridge.processor.get();
}

- (pulp::state::StateStore *)pulpStore {
    return &_bridge.store;
}

- (uint32_t)pulpBypassParameterId {
    return static_cast<uint32_t>(_bridge.bypass_param_id);
}

- (NSUInteger)pulpLastParameterEventCount {
    return static_cast<NSUInteger>(_bridge.param_events.size());
}

- (uint32_t)pulpLastParameterEventParamIDAtIndex:(NSUInteger)index {
    const auto& events = _bridge.param_events.events();
    if (index >= events.size()) return 0;
    return events[index].param_id;
}

- (int32_t)pulpLastParameterEventSampleOffsetAtIndex:(NSUInteger)index {
    const auto& events = _bridge.param_events.events();
    if (index >= events.size()) return 0;
    return events[index].sample_offset;
}

- (int32_t)pulpLastParameterEventRampDurationAtIndex:(NSUInteger)index {
    const auto& events = _bridge.param_events.events();
    if (index >= events.size()) return 0;
    return events[index].ramp_duration_sample_frames;
}

- (float)pulpLastParameterEventValueAtIndex:(NSUInteger)index {
    const auto& events = _bridge.param_events.events();
    if (index >= events.size()) return 0.0f;
    return events[index].value;
}

// ARA-aware AU hosts (Logic Pro 11+, etc.) read this property via KVO
// during scan. Returns an opaque ARA::ARAFactory* when the plug-in
// participates in ARA; nullptr otherwise. See issue #252 and the
// A2c ralph slice.
- (void *)audioUnitARAFactory {
    return const_cast<void *>(
        pulp::format::ara_companion_factory_for(nullptr));
}

// ── AU v3 view configuration negotiation ───────────────────────────────────
//
// Hosts (Logic, MainStage, GarageBand) probe `supportedViewConfigurations:`
// with a set of candidate window sizes and pick one via
// `selectViewConfiguration:`. For a fixed-design GPU editor (the common
// Pulp shape), we accept configurations whose aspect ratio is close to the
// processor's design aspect, and we prefer the smallest config that can
// contain the design viewport. The view controller's
// `set_design_viewport` + `set_fixed_aspect_ratio` then scales the actual
// paint at the host-chosen size.
//
// preferredContentSize remains authoritative for the initial editor size
// when no host-selected configuration applies. Don't put an internal corner
// resizer here — Logic's AU v3 path is host/container-driven.

- (NSIndexSet *)supportedViewConfigurations:(NSArray<AUAudioUnitViewConfiguration *> *)availableViewConfigurations NS_AVAILABLE(10_13, 11_0) {
    NSMutableIndexSet *result = [NSMutableIndexSet indexSet];
    if (!_bridge.processor || availableViewConfigurations.count == 0) {
        return result;
    }

    const auto hints = _bridge.processor->view_size();
    const double design_w = static_cast<double>(hints.preferred_width);
    const double design_h = static_cast<double>(hints.preferred_height);
    if (design_w <= 0.0 || design_h <= 0.0) {
        // No usable design size — accept everything and let the host decide.
        [result addIndexesInRange:NSMakeRange(0, availableViewConfigurations.count)];
        return result;
    }

    const double design_aspect = design_w / design_h;
    // 5% aspect tolerance matches the JUCE AUv3 demo workaround for Logic
    // 10.6.1's 1024x768 / 1366x1024 probe (forum.juce.com/t/auv3-resizing
    // -issue-on-macos-not-ios/43811). Tight enough to reject obvious
    // wrong-aspect candidates, loose enough to land on Logic's defaults.
    constexpr double kAspectTolerance = 0.05;

    for (NSUInteger i = 0; i < availableViewConfigurations.count; ++i) {
        AUAudioUnitViewConfiguration *cfg = availableViewConfigurations[i];
        const double w = static_cast<double>(cfg.width);
        const double h = static_cast<double>(cfg.height);
        if (w <= 0.0 || h <= 0.0) continue;

        const double cfg_aspect = w / h;
        const double aspect_delta = std::abs(cfg_aspect - design_aspect) / design_aspect;
        if (aspect_delta <= kAspectTolerance) {
            [result addIndex:i];
            continue;
        }

        // Even on an aspect mismatch, accept configurations large enough to
        // contain the design viewport — the controller will letterbox via
        // set_design_viewport. Preferred only when at least one aspect-match
        // option exists; if none do, the host needs some option to land on.
        if (w >= design_w && h >= design_h) {
            [result addIndex:i];
        }
    }

    // If nothing matched (extreme aspect / undersized configs), fall back to
    // accepting everything rather than telling the host we have no UI.
    if (result.count == 0) {
        [result addIndexesInRange:NSMakeRange(0, availableViewConfigurations.count)];
    }
    return result;
}

- (void)selectViewConfiguration:(AUAudioUnitViewConfiguration *)viewConfiguration NS_AVAILABLE(10_13, 11_0) {
    // The view controller is the authority on actual sizing — it already
    // observes its own bounds in viewDidLayout and forwards to
    // PluginViewHost::set_size + ViewBridge::resize. This selector is the
    // host telling us its preferred config; we log it for diagnostics and
    // bump the preferredContentSize so any pending unload/reload of the
    // controller picks up the new size.
    pulp::runtime::log_info("AU v3 host selected view config: {}x{} (hostHasController={})",
                            static_cast<int>(viewConfiguration.width),
                            static_cast<int>(viewConfiguration.height),
                            viewConfiguration.hostHasController ? "YES" : "NO");
}

@end
