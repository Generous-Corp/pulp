// Audio Unit v3 adapter for Pulp
// Implements AUAudioUnit wrapping a Pulp Processor
// Built from Apple AudioToolbox documentation
//
// ─────────────────────────────────────────────────────────────────────────
// Per-method audit checklist
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
//  │ parameterTree                            │ AUParam…  │ main         │ YES (first call) │
//  │ shouldBypassEffect                       │ BOOL      │ main + audio │ NO (atomic)  │
//  │ setShouldBypassEffect:                   │ void      │ main + audio │ NO (RT-safe) │
//  │ allocateRenderResourcesAndReturnError:   │ BOOL      │ main         │ YES (init)   │
//  │ deallocateRenderResources                │ void      │ main         │ YES (release)│
//  │ internalRenderBlock                      │ block     │ main         │ YES (block)  │
//  │   └─ render block body                   │ OSStatus  │ AUDIO        │ NO           │
//  │ fullState                                │ NSDict    │ main         │ YES (serdes) │
//  │ setFullState:                            │ void      │ main         │ YES (serdes) │
//  │ audioUnitARAFactory                      │ void*     │ KVO/main     │ NO (cached)  │
//  └──────────────────────────────────────────┴───────────┴──────────────┴──────────────┘
//
// NOTE: supportedViewConfigurations: / selectViewConfiguration: are
// intentionally NOT overridden (removed). See the "view configuration
// negotiation: intentionally NOT implemented" comment near @end and the
// regression test "AU v3 does not opt into host view configurations". Logic
// locks the editor window to the selected config's aspect; not opting in lets
// it free-resize to the design aspect (the AUv3-Logic sizing fix).
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
#include <pulp/format/adapter_boundary.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/format/parameter_text.hpp>
#include <pulp/format/host_quirks.hpp>
#include <pulp/format/quirk_apply.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/format/ara.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>
#include <pulp/format/detail/playhead_diff.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump.hpp>
#include <pulp/midi/ump_sysex7_reassembler.hpp>
#include <pulp/runtime/assert.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <cmath>
#include <memory>
#include <array>
#include <atomic>
#include <limits>
#include <vector>

namespace pulp::format::au {

// Per-bus channel ceiling. Single source of truth is
// boundary::kBoundaryMaxChannels (adapter_boundary.hpp) — kept as an `int` here
// because AU's channel counts, loop indices, and clamps are all signed. Same
// value (8) across every adapter; change it once at the boundary.
static constexpr int kMaxChannels =
    static_cast<int>(boundary::kBoundaryMaxChannels);

// The bypass parameter has to be tracked on two surfaces: AUAudioUnit's
// `shouldBypassEffect` AUValue and the plugin-provided `Bypass` parameter
// when present. We scan the descriptor's parameter list at init for a
// parameter named "Bypass" (boolean, 0..1, step==1) and route both surfaces
// to it. Hosts that observe one but not the other (Logic vs MainStage) then
// see a consistent value. When no such parameter exists, we still track the
// bypass flag in a bridge-local atomic so the host's `setShouldBypassEffect:`
// request is honoured at the audio thread.
struct AUBridge {
    // The store is declared before the Processor so it is destroyed after it.
    // `Processor::state()` dereferences a pointer to this store, and a Processor
    // may read it from its destructor or from a worker thread that destructor is
    // about to join. Reversing these two lines hands that thread a freed store.
    state::StateStore store;
    std::unique_ptr<Processor> processor;
    double sample_rate = 48000.0;
    AUAudioFrameCount max_frames = 512;
    int input_channels = 0;
    int output_channels = 0;
    // Sidechain bus channel count. 0 when the descriptor has no second input
    // bus; the render block then skips the sidechain pull and calls
    // Processor::set_sidechain(nullptr).
    int sidechain_channels = 0;
    // Dual-tracked bypass.
    //   * `bypass_param_id != 0` means the plugin declared a "Bypass"
    //     parameter; the AUAudioUnit's `shouldBypassEffect` reads/writes
    //     it through StateStore.
    //   * Otherwise, `bypass_flag` is the only authority. The render
    //     block consults whichever path is live and short-circuits the
    //     processor with a pass-through (or silence for instruments).
    state::ParamID bypass_param_id = 0;
    std::atomic<bool> bypass_flag{false};

    // Offline-render state. A host doing a faster-than-real-time bounce sets
    // AUAudioUnit.renderingOffline = YES before rendering; we capture it here so
    // the render block can pass ProcessMode::Offline to the processor (e.g. so a
    // GPU engine drives its work synchronously instead of dropping it). Written
    // from the host's setRenderingOffline:, read on the render thread.
    std::atomic<bool> rendering_offline{false};

    // Dry-delay line for the bypass pass-through. Sized to the processor's
    // reported latency in allocateRenderResources; keeps the bypassed dry
    // signal aligned with the host's plugin-delay-compensation instead of
    // arriving `latency` samples early (a 0 latency leaves it a zero-copy
    // passthrough).
    boundary::LatencyCompensatedBypassT<kMaxChannels> bypass;

    // Pre-allocated — no heap allocation on audio thread
    float* output_ptrs[kMaxChannels] = {};
    const float* input_ptrs[kMaxChannels] = {};
    const float* sidechain_ptrs[kMaxChannels] = {};
    // AU hosts may pass null mData pointers and expect the render block to
    // provide storage. Keep fallback buffers sized at allocation time so the
    // real-time path does not depend on host-provided output memory.
    std::vector<float> output_storage;
    std::vector<float> input_storage;

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
    // (sized to max_frames * kMaxChannels during render-resource allocation).
    std::vector<float> sidechain_storage;
    state::ParameterEventQueue param_events;

    // Per-block MIDI I/O. Hoisted to the bridge (not constructed per render
    // call) and given reserved, realtime-capacity-limited storage in
    // allocateRenderResources so add()/add_sysex_copy() never grow a vector on
    // the audio thread. Reset with clear()+clear_sysex() each block (clear()
    // empties only the event list; clear_sysex() recycles the pooled sysex
    // payloads). Matches the VST3/CLAP adapters. Capacities below are shared
    // with VST3 (vst3_adapter.cpp) so behavior matches across formats.
    static constexpr std::size_t kMaxEventsPerBlock = 2048;
    static constexpr std::size_t kMaxSysexPerBlock = 64;
    static constexpr std::size_t kMaxSysexPayloadBytes = 512;
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    // Reusable reassembler for UMP sysex7 reassembly. Hoisted + reserved so
    // feed_packet() doesn't grow its accumulator/scratch vectors on the audio
    // thread. A single instance is fine: the render block feeds packets in
    // arrival order and the reassembler tracks one in-progress stream.
    midi::UmpSysex7Reassembler sysex_reassembler;

    // MPE sidecar — mirrors the VST3/CLAP wiring. When the Processor opts into
    // MPE (effective_capabilities().supports_mpe), the render block runs midi_in
    // through the tracker and hands the resulting per-note expression buffer to
    // the Processor via set_mpe_input() for the duration of process(). Configured
    // in initWithComponentDescription, reserved + capacity-limited in
    // allocateRenderResources, reset in deallocateRenderResources. See
    // boundary::MpeSidecar.
    boundary::MpeSidecar mpe;

    // Previous-block transport snapshot used to derive the change flags on
    // `ProcessContext`. Default-constructed (no previous block) so the first
    // render-block invocation reports no changes.
    detail::PlayheadSnapshot playhead_prev;

    // Host accommodations, resolved once at init via the runtime policy.
    HostQuirks host_quirks{};
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

// Set while a host parameter write (implementorValueObserver) or our own
// -setValue: push is in flight, so the inline StateStore listener that pushes UI
// edits to the host doesn't echo the host's own value straight back (and so our
// push doesn't loop back through implementorValueObserver → store → listener).
// thread_local because the inline (Audio) listener runs synchronously on
// whichever thread set the store, so set + check always pair on one thread.
static thread_local bool g_au_v3_host_writing = false;

// RAII guard for g_au_v3_host_writing. SAVES and RESTORES the previous value
// (rather than unconditionally clearing it) so an early return, an ObjC
// exception, or a nested guarded write can't leave the flag stuck. The flag is
// thread_local and the StateStore listener it guards is registered
// ListenerThread::Audio — i.e. it runs SYNCHRONOUSLY on the same thread that
// wrote the store (see the listener-registration comment near
// add_listener(..., ListenerThread::Audio) below). So a save/restore pair
// always brackets exactly the synchronous notify it is meant to suppress.
struct ScopedAuV3HostWriting {
    bool previous_;
    ScopedAuV3HostWriting() noexcept : previous_(g_au_v3_host_writing) {
        g_au_v3_host_writing = true;
    }
    ~ScopedAuV3HostWriting() noexcept { g_au_v3_host_writing = previous_; }
    ScopedAuV3HostWriting(const ScopedAuV3HostWriting&) = delete;
    ScopedAuV3HostWriting& operator=(const ScopedAuV3HostWriting&) = delete;
};

} // namespace pulp::format::au

// ── AUAudioUnit subclass ───────────────────────────────────────────────────

@interface PulpAudioUnit : AUAudioUnit {
    pulp::format::au::AUBridge _bridge;
    AUAudioUnitBus *_inputBus;
    AUAudioUnitBus *_outputBus;
    AUAudioUnitBusArray *_inputBusArray;
    AUAudioUnitBusArray *_outputBusArray;
    // MainThreadDispatcher backend token. Held for the lifetime of this plugin
    // instance so DAW-hosted code can post work to the main thread via
    // pulp::events::MainThreadDispatcher::call_async().
    pulp::events::MainThreadDispatcher::Token _mainThreadToken;

    // Parameter automation (UI → host recording). The AUParameterTree is built
    // once and RETAINED — the host observes these exact AUParameter objects, so
    // a per-call rebuild would push UI edits into throwaway objects the host
    // isn't watching. _automationToken is the originator passed to
    // -setValue:originator:… so our own implementorValueObserver isn't the
    // recipient; _automationListener pushes StateStore edits back to the host.
    AUParameterTree *_parameterTree;
    AUParameterObserverToken _automationToken;
    pulp::state::ListenerToken _automationListener;
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
/// otherwise NULL.
@property (nonatomic, readonly, nullable) void *audioUnitARAFactory;

/// Diagnostic accessor for the bypass-param wiring decision the adapter made
/// at init. Returns 0 when no plugin-declared bypass parameter matched (the
/// synthesized-AUValue path is in use); otherwise the StateStore parameter ID
/// that proxies the bypass surface.
- (uint32_t)pulpBypassParameterId;

- (NSUInteger)pulpLastParameterEventCount;
- (NSUInteger)pulpLastParameterEventCapacity;
- (BOOL)pulpLastParameterEventsOverflowed;
- (uint32_t)pulpLastParameterEventDropCount;
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

    // Opt this plugin instance into the process-wide MainThreadDispatcher
    // backend. On macOS this installs (or refcount-bumps) a Cocoa backend
    // posting to `dispatch_get_main_queue`, so KVO publishes (e.g. the
    // `latency` / `tailTime` dispatch_async below) and view-side code reaches
    // the host's main thread without needing its own per-callsite
    // dispatch_async.
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

    // Resolve host accommodations once via the runtime policy
    // (PULP_HOST_QUIRKS env / set_host_quirk_policy API).
    // Qualified: this @implementation method body is at file scope, not
    // inside namespace pulp::format::au, so unqualified lookup misses it.
    {
        const auto host_info = pulp::format::detect_host_info();
        _bridge.host_quirks =
            pulp::format::resolved_quirks(host_info.type, host_info.version);
    }

    // Inject an automatable "Bypass" param when the plugin declared none,
    // BEFORE the detection pass below, which then mirrors it onto the AU
    // bypass surface. No-op when the quirk is filtered out. Qualified
    // (Obj-C method file scope).
    pulp::format::maybe_synthesize_bypass(_bridge.store, _bridge.host_quirks);

    // Auto-detect the plugin's Bypass parameter so the host's
    // `shouldBypassEffect` AUValue and the plugin's automatable parameter stay
    // in lockstep. Uses the shared `is_bypass_param` contract: a declared
    // `Bypass` designation wins, otherwise the legacy name/range heuristic
    // (name == "Bypass", boolean range 0..1 with step >= 1) applies. When
    // found, the AUv3 surface mirrors it.
    for (const auto& p : _bridge.store.all_params()) {
        if (pulp::state::is_bypass_param(p)) {
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

    // Wire the MPE sidecar when the plug-in opts in. configure() binds the
    // tracker's callbacks once here on the host thread (never the render
    // thread); the buffer is reserved + capacity-limited in
    // allocateRenderResources. Mirrors the VST3/CLAP adapters.
    _bridge.mpe.configure(desc.effective_capabilities().supports_mpe);
    if (_bridge.mpe.enabled) {
        pulp::runtime::log_info("AU: MPE sidecar enabled for '{}'", desc.name);
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

    // Sidechain input. When the descriptor declares a second input_bus, expose
    // it as a second AUAudioUnitBus so hosts can connect a sidechain source
    // that the render block will route through Processor::set_sidechain().
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

- (NSArray<NSNumber *> *)channelCapabilities {
    if (!_bridge.processor) return nil;
    const auto desc = _bridge.processor->descriptor();
    if (desc.supported_bus_layouts.empty()) return nil;
    NSMutableArray<NSNumber *> *result = [NSMutableArray array];
    for (const auto& layout : desc.supported_bus_layouts) {
        const int inputs = layout.inputs.empty() ? 0 : layout.inputs.front();
        const int outputs = layout.outputs.empty() ? 0 : layout.outputs.front();
        [result addObject:@(inputs)];
        [result addObject:@(outputs)];
    }
    return result;
}

// ── Required property overrides ────────────────────────────────────────────

- (NSTimeInterval)latency {
    if (!_bridge.processor) return 0.0;
    // Route the non-negative latency clamp through the host-quirks policy so
    // PULP_HOST_QUIRKS=off reports raw latency too.
    int samples = pulp::format::reported_latency_samples(
        _bridge.processor->latency_samples(), _bridge.host_quirks);
    return _bridge.sample_rate > 0 ? static_cast<double>(samples) / _bridge.sample_rate : 0.0;
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
    // Built once and retained: the host observes THESE AUParameter objects, so a
    // per-call rebuild would push UI edits into throwaway objects the host isn't
    // watching (breaking automation recording).
    if (_parameterTree) return _parameterTree;

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
        else if (pulp::state::is_boolean_param(p))
            unit = kAudioUnitParameterUnit_Boolean;

        NSMutableArray<NSString *> *valueStrings = nil;
        if (!p.value_labels.empty()) {
            valueStrings = [NSMutableArray arrayWithCapacity:p.value_labels.size()];
            for (const auto& label : p.value_labels) {
                [valueStrings addObject:[NSString stringWithUTF8String:label.c_str()]];
            }
        }

        AUParameter *auParam = [AUParameterTree createParameterWithIdentifier:identifier
            name:name address:address min:p.range.min max:p.range.max
            unit:unit unitName:nil
            flags:kAudioUnitParameterFlag_IsWritable | kAudioUnitParameterFlag_IsReadable
            valueStrings:valueStrings dependentParameters:nil];
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
        //
        // Guard the inline UI→host listener (below): this is the host's (or our
        // own -setValue:) write, so it must NOT be echoed back to the host. The
        // RAII guard save/restores the thread_local flag so a nested write or an
        // early return can't leave it stuck (the listener it suppresses runs
        // synchronously on this thread — ListenerThread::Audio, below).
        pulp::format::au::ScopedAuV3HostWriting host_writing;
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
            if (info) {
                auto str = pulp::format::format_parameter_text(*info, v);
                return [NSString stringWithUTF8String:str.c_str()];
            }
        }
        return [NSString stringWithFormat:@"%.2f", v];
    };

    tree.implementorValueFromStringCallback = ^AUValue(AUParameter *param, NSString *text) {
        PulpAudioUnit* strongSelf = weakSelf;
        if (!strongSelf || !text) return param.value;
        const auto* info = strongSelf->_bridge.store.info(
            static_cast<pulp::state::ParamID>(param.address));
        if (!info) return param.value;
        const char* utf8 = [text UTF8String];
        if (!utf8) return param.value;
        const auto parsed = pulp::format::parse_parameter_text(*info, utf8);
        return parsed ? *parsed : param.value;
    };

    // Retain: this file is MRC, and `tree` is autoreleased — without a retain it
    // would dangle once the surrounding autorelease pool drains.
    _parameterTree = [tree retain];

    // Originator token: passed to -setValue:originator:… so the host's recording
    // observer is notified while excluding the originator (a no-op observer we own).
    _automationToken = [tree tokenByAddingParameterObserver:^(AUParameterAddress, AUValue) {}];

    // ── UI → host: record automation when the editor moves a parameter ───────
    // When a StateStore edit happens that did NOT come from the host, push it to
    // the matching AUParameter so the host (Logic) records it. Registered as an
    // Audio (inline) listener so it runs synchronously on whichever thread set
    // the store: a UI edit fires it on the main thread (where -setValue: is
    // valid), and a host write fires it with g_au_v3_host_writing set so it is
    // suppressed. Every render-thread store write is bracketed by the RAII guard
    // (both implementorValueObserver and the render block's
    // AURenderEventParameter handler), so this listener early-returns there and
    // -setValue: is never called from audio.
    _automationListener = _bridge.store.add_listener(
        [weakSelf](pulp::state::ParamID id, float value) {
            if (pulp::format::au::g_au_v3_host_writing) return;
            PulpAudioUnit* s = weakSelf;
            if (!s || !s->_parameterTree) return;
            AUParameter* p =
                [s->_parameterTree parameterWithAddress:static_cast<AUParameterAddress>(id)];
            if (!p) return;
            // Suppress re-entry through implementorValueObserver while -setValue:
            // pushes back to the host. RAII so an ObjC exception out of
            // -setValue: restores the flag rather than leaving it stuck.
            pulp::format::au::ScopedAuV3HostWriting host_writing;
            [p setValue:value
              originator:s->_automationToken
               atHostTime:0
                eventType:AUParameterAutomationEventTypeValue];
        },
        pulp::state::ListenerThread::Audio);

    // ── Gesture begin/end → host Touch/Release (clean automation-pass bounds) ─
    // The native widget binding (parameter_binding.hpp) fires begin_gesture on
    // drag start and end_gesture on drag end; relay those as Touch / Release so
    // Logic's Touch/Latch modes bracket the pass correctly. Inlined per lambda
    // (no shared ObjC block) — MRC would not Block_copy a stack block captured by
    // a C++ closure that outlives this scope.
    auto sendGesture = [](PulpAudioUnit* s, pulp::state::ParamID id,
                          AUParameterAutomationEventType type) {
        if (!s || !s->_parameterTree) return;
        AUParameter* p =
            [s->_parameterTree parameterWithAddress:static_cast<AUParameterAddress>(id)];
        if (!p) return;
        pulp::format::au::g_au_v3_host_writing = true;
        [p setValue:p.value originator:s->_automationToken atHostTime:0 eventType:type];
        pulp::format::au::g_au_v3_host_writing = false;
    };
    _bridge.store.set_gesture_callbacks(
        [weakSelf, sendGesture](pulp::state::ParamID id) {
            sendGesture(weakSelf, id, AUParameterAutomationEventTypeTouch);
        },
        [weakSelf, sendGesture](pulp::state::ParamID id) {
            sendGesture(weakSelf, id, AUParameterAutomationEventTypeRelease);
        });

    return _parameterTree;
}

// Dual-tracked bypass.
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

// AUAudioUnit.renderingOffline — the host sets this true before a
// faster-than-real-time (offline) render such as a bounce, false otherwise.
// Capture it into a bridge-local atomic the render block reads so the processor
// gets ProcessMode::Offline; call super so the base class state stays correct.
- (void)setRenderingOffline:(BOOL)flag {
    [super setRenderingOffline:flag];
    _bridge.rendering_offline.store(flag ? true : false, std::memory_order_release);
}

// ── Render resources ───────────────────────────────────────────────────────

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError {
    if (![super allocateRenderResourcesAndReturnError:outError]) return NO;

    _bridge.sample_rate = _outputBus.format.sampleRate;
    _bridge.max_frames = self.maximumFramesToRender;
    const auto storage_samples =
        static_cast<std::size_t>(pulp::format::au::kMaxChannels) *
        static_cast<std::size_t>(_bridge.max_frames);
    _bridge.output_storage.assign(storage_samples, 0.0f);
    _bridge.input_storage.assign(storage_samples, 0.0f);
    _bridge.sidechain_storage.assign(storage_samples, 0.0f);

    // Pre-size the per-block MIDI buffers + UMP reassembler so the render block
    // appends without heap allocation. Capacity-limited: add()/add_sysex_copy()
    // drop past the reserved bound instead of growing (matches VST3/CLAP).
    _bridge.midi_in.reserve(pulp::format::au::AUBridge::kMaxEventsPerBlock,
                            pulp::format::au::AUBridge::kMaxSysexPerBlock,
                            pulp::format::au::AUBridge::kMaxSysexPayloadBytes);
    _bridge.midi_out.reserve(pulp::format::au::AUBridge::kMaxEventsPerBlock,
                             pulp::format::au::AUBridge::kMaxSysexPerBlock,
                             pulp::format::au::AUBridge::kMaxSysexPayloadBytes);
    _bridge.midi_in.set_realtime_capacity_limit(true);
    _bridge.midi_out.set_realtime_capacity_limit(true);
    _bridge.sysex_reassembler.reserve(
        pulp::format::au::AUBridge::kMaxSysexPayloadBytes);

    // Reserve the MPE expression buffer so the render block never allocates: one
    // inbound MIDI event can fan out to several expression events (note-on +
    // bend + pressure + timbre). Reset the tracker so a fresh render-resource
    // allocation starts with no stale per-note state. Matches VST3's
    // setupProcessing.
    _bridge.mpe.reserve(pulp::format::au::AUBridge::kMaxEventsPerBlock);
    _bridge.mpe.reset();

    if (_bridge.processor) {
        pulp::format::PrepareContext ctx;
        ctx.sample_rate = _bridge.sample_rate;
        ctx.max_buffer_size = static_cast<int>(_bridge.max_frames);
        ctx.output_channels = _bridge.output_channels;
        ctx.input_channels = _bridge.input_channels;
        _bridge.processor->prepare(ctx);

        // Size the bypass dry-delay line to the reported latency so a bypassed
        // block emits `input[n - latency]`, matching the host's PDC on the wet
        // path. Off the render thread; a 0 latency leaves it a zero-copy
        // passthrough. Same policy the CLAP/VST3/AU v2 adapters apply.
        _bridge.bypass.prepare(pulp::format::reported_latency_samples(
            _bridge.processor->latency_samples(), _bridge.host_quirks));
    }

    pulp::runtime::log_info("AU: render resources at {} Hz, {} frames",
        _bridge.sample_rate, _bridge.max_frames);
    return YES;
}

- (void)deallocateRenderResources {
    if (_bridge.processor) _bridge.processor->release();
    // Drop per-note expression state so a re-allocation does not route a stale
    // noteId to a voice that no longer exists (mirrors VST3's setActive(false)).
    _bridge.mpe.reset();
    [super deallocateRenderResources];
}

// Symmetric teardown of the MainThreadDispatcher backend installed in init.
- (void)dealloc {
    // Tear down parameter-automation wiring while the C++ StateStore (_bridge) is
    // still alive: drop the gesture callbacks + store listener that capture self,
    // remove our host observer token, and release the retained (MRC) tree.
    _bridge.store.set_gesture_callbacks({}, {});
    _automationListener.reset();
    if (_parameterTree && _automationToken) {
        [_parameterTree removeParameterObserver:_automationToken];
        _automationToken = nullptr;
    }
    [_parameterTree release];
    _parameterTree = nil;

    if (_mainThreadToken != 0) {
        pulp::events::unregister_plugin_backend(_mainThreadToken);
        _mainThreadToken = 0;
    }
    [super dealloc];
}

// ── Render block ───────────────────────────────────────────────────────────

- (AUInternalRenderBlock)internalRenderBlock {
    auto* bridge = &_bridge;

    // Capture the host's musical-context and transport-state blocks at
    // render-block construction time, per Apple's render-block contract.
    // AUAudioUnit::musicalContextBlock and transportStateBlock are KVO-able
    // read/write properties the host installs, often only after
    // `allocateRenderResources`. They are safe to invoke from the render
    // thread; the block we hand back to the host captures them via `__block`
    // so the call sites below stay self-contained.
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
            if (outputData) {
                for (UInt32 i = 0; i < outputData->mNumberBuffers; ++i) {
                    if (outputData->mBuffers[i].mData) {
                        memset(outputData->mBuffers[i].mData, 0, outputData->mBuffers[i].mDataByteSize);
                    }
                }
            }
            return noErr;
        }
        if (!outputData) return noErr;
        // Max-block contract for AU v3 is REJECT, not clamp. Unlike the
        // push-based adapters (CLAP/VST3 clamp+zero-fill; AAX grows its offline
        // scratch), AU renders through a pull graph: pullInputBlock feeds
        // upstream at `frameCount`, so clamping would advance this node by fewer
        // frames than the host and its siblings, desyncing the pull chain.
        // `maximumFramesToRender` is a hard host contract (queried up front and
        // never exceeded; auval never overruns it), and kAudioUnitErr_TooMany-
        // FramesToProcess is the spec-defined response. Returning it here also
        // upholds the frameCount <= max_frames invariant the scratch asserts
        // below rely on.
        if (frameCount > bridge->max_frames) {
            return kAudioUnitErr_TooManyFramesToProcess;
        }

        // Flush denormals to zero for the whole render-block body so quiet
        // tails in recursive filter/reverb/feedback state can't stall the
        // host's audio thread, then restore its prior FP mode on scope exit.
        // See docs/guides/dsp-threading.md "Numeric mode".
        pulp::signal::ScopedFlushDenormals flush_denormals;

        bridge->param_events.clear();

        UInt32 outChans = std::min(outputData->mNumberBuffers,
            static_cast<UInt32>(pulp::format::au::kMaxChannels));

        // Hard invariant, not a live allocation: output_storage was sized to
        // kMaxChannels * max_frames in allocateRenderResources, outChans is
        // clamped to kMaxChannels, and frameCount is clamped to max_frames
        // above — so the scratch always fits. Assert it (debug/sanitizer
        // builds) instead of an on-the-audio-thread assign().
        [[maybe_unused]] const std::size_t neededOutputStorage =
            static_cast<std::size_t>(outChans) * frameCount;
        PULP_DBG_ASSERT(bridge->output_storage.size() >= neededOutputStorage,
            "AU render: output_storage undersized; allocateRenderResources must "
            "pre-size to kMaxChannels * max_frames");

        // Output buffer view (uses preallocated scratch when the host passes
        // null mData during validation/probing).
        for (UInt32 i = 0; i < outChans; ++i) {
            auto& buffer = outputData->mBuffers[i];
            if (!buffer.mData || buffer.mDataByteSize < frameCount * sizeof(float)) {
                buffer.mNumberChannels = 1;
                buffer.mDataByteSize = frameCount * sizeof(float);
                buffer.mData = bridge->output_storage.data() +
                    static_cast<std::size_t>(i) * frameCount;
            }
            bridge->output_ptrs[i] = static_cast<float*>(buffer.mData);
        }
        pulp::audio::BufferView<float> output_view(bridge->output_ptrs, outChans, frameCount);

        // Input: pull from upstream if we have input channels
        pulp::audio::BufferView<const float> input_view;
        if (pullInputBlock && bridge->input_channels > 0) {
            auto& abl = bridge->input_abl;
            abl.mNumberBuffers = outChans;
            [[maybe_unused]] const std::size_t input_samples =
                static_cast<std::size_t>(outChans) * frameCount;
            // Hard invariant (see output_storage above): input_storage is
            // pre-sized to kMaxChannels * max_frames, so it always fits — never
            // an assign() on the audio thread.
            PULP_DBG_ASSERT(bridge->input_storage.size() >= input_samples,
                "AU render: input_storage undersized; allocateRenderResources "
                "must pre-size to kMaxChannels * max_frames");
            for (UInt32 i = 0; i < outChans; ++i) {
                abl.mBuffers[i].mNumberChannels = 1;
                abl.mBuffers[i].mDataByteSize = frameCount * sizeof(float);
                abl.mBuffers[i].mData =
                    bridge->input_storage.data() +
                    static_cast<std::size_t>(i) * frameCount;
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

        // Sidechain: pull bus 1 into its own ABL so it doesn't alias the main
        // input block. Processor::set_sidechain() takes a BufferView that
        // remains valid for the duration of process().
        pulp::audio::BufferView<const float> sidechain_view;
        int scChans = bridge->sidechain_channels;
        if (pullInputBlock && scChans > 0) {
            UInt32 scBufs = std::min(static_cast<UInt32>(scChans),
                                     static_cast<UInt32>(pulp::format::au::kMaxChannels));
            // Hard invariant (see output_storage above): sidechain_storage is
            // pre-sized to kMaxChannels * max_frames, so it always fits — never
            // an assign() on the audio thread.
            [[maybe_unused]] std::size_t needed =
                static_cast<std::size_t>(scBufs) * frameCount;
            PULP_DBG_ASSERT(bridge->sidechain_storage.size() >= needed,
                "AU render: sidechain_storage undersized; allocateRenderResources "
                "must pre-size to kMaxChannels * max_frames");
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

        // MIDI events. Short messages arrive as AURenderEventMIDI; sysex (and
        // long MIDI 2.0 UMP groups from AU v3.1+) arrive as
        // AURenderEventMIDIEventList. Long packets are routed into
        // MidiBuffer's variable-length sysex sidecar.
        // Reuse the bridge-owned, pre-reserved MIDI buffers (no per-block
        // construction / allocation). clear() empties the event list;
        // clear_sysex() recycles the pooled sysex payloads so the sidecar does
        // not leak last block's sysex into this one.
        pulp::midi::MidiBuffer& midi_in = bridge->midi_in;
        pulp::midi::MidiBuffer& midi_out = bridge->midi_out;
        // No-alloc tripwire over the adapter-owned MIDI input translation: the
        // buffer reset, the render-event parse loop (short MIDI, UMP sysex7
        // reassembly into the pre-reserved payload pool), the guarded param
        // store-writes, and the event sort. All of this is pre-reserved /
        // capacity-limited, so it must not allocate. (The host transport
        // callbacks that follow — musicalContextBlock / transportStateBlock —
        // are Apple framework calls outside our control and are deliberately
        // outside this scope; process() gets its own scope below.)
        {
            pulp::runtime::ScopedNoAlloc midi_in_no_alloc;
            midi_in.clear();
            midi_in.clear_sysex();
            midi_out.clear();
            midi_out.clear_sysex();
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
                    // This is the host playing back automation, so it must NOT echo
                    // back to the host: guard the inline UI→host automation listener
                    // exactly like implementorValueObserver does. `set_value_rt` →
                    // notify_rt fires _automationListener SYNCHRONOUSLY on this
                    // render thread (it is registered ListenerThread::Audio); the
                    // guard flag is thread_local, so bracketing this write
                    // short-circuits that same-thread listener (its
                    // `if (g_au_v3_host_writing) return;` guard) — no -setValue:
                    // (lock-taking / allocating Foundation ObjC) on the audio thread,
                    // and no value echoed straight back into the pass the host is
                    // already recording. RAII so the flag is restored even if a
                    // nested write or early-out intervenes.
                    {
                        pulp::format::au::ScopedAuV3HostWriting host_writing;
                        bridge->store.set_value_rt(param_id,
                                                   static_cast<float>(p.value));
                    }
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
                    // UMP message-type nibble (bits 28-31 of word 0) identifies
                    // the message class; nibble 0x3 is sysex7 and reassembly is
                    // delegated to the shared UmpSysex7Reassembler in core/midi.
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
                        // add_sysex_copy (not add_sysex(vector)) so the payload is
                        // copied into the buffer's pre-reserved realtime payload
                        // pool — add_sysex(std::vector) takes the vector by value and
                        // would allocate a fresh heap vector per sysex on the audio
                        // thread.
                        auto emit = [](const std::vector<uint8_t>& payload,
                                       void* user) {
                            auto* c = static_cast<EmitCtx*>(user);
                            c->sink->add_sysex_copy(payload.data(), payload.size(),
                                                    c->sample_offset, 0.0);
                        };

                        // Reuse the bridge's pre-reserved reassembler rather than
                        // constructing one per event (its accumulator/scratch are
                        // already sized). reset() clears any partial stream left by
                        // a previous event/block so an orphaned continue/end can't
                        // bleed across logical sysex boundaries.
                        auto& reassembler = bridge->sysex_reassembler;
                        reassembler.reset();
                        const MIDIEventPacket* pkt = &packets->packet[0];

                        for (UInt32 i = 0; i < packets->numPackets; ++i) {
                            // Walk this packet's UMP words with the shared
                            // spec-complete cursor: it advances each message by
                            // its true word length (type 0x3 is 2 words), so a
                            // multi-word message's trailing words never
                            // masquerade as a fresh header, and a truncated
                            // packet stops the walk before an over-read. Only
                            // type-0x3 (Data/SysEx7) feeds the reassembler.
                            pulp::midi::walk_ump_packet(
                                pkt->words, static_cast<uint32_t>(pkt->wordCount),
                                [&](uint8_t mt, const uint32_t* mw, uint32_t) {
                                    if (mt == 0x3) {
                                        reassembler.feed_packet(mw[0], mw[1],
                                                                emit, &ctx);
                                    }
                                });
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
        } // end MIDI-input no-alloc scope

        // Bypass short-circuit. Consult the plugin's Bypass parameter when it
        // has one; otherwise the bridge-local atomic the host wrote via
        // setShouldBypassEffect:. When bypassed we skip `processor_->process()`
        // entirely and emit pass-through:
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
            // Copy input → output latency-compensated through the shared dry-
            // delay line so the bypassed dry signal stays aligned with the
            // host's PDC on the wet path (channels beyond the boundary ceiling
            // fall back to an undelayed copy uniformly); zero any output channel
            // with no matching input.
            pulp::format::boundary::render_bypass_passthrough(
                bridge->bypass, bridge->output_ptrs,
                static_cast<int>(outChans), bridge->input_ptrs,
                bridge->input_channels,
                static_cast<std::uint32_t>(frameCount));
            // Trigger reset is a single-exit invariant: settle Reset/trigger
            // params even on the bypass short-circuit so a panic/reset raised
            // while bypassed clears this block. The parameter tree's
            // implementorValueProvider reads the store live, so the host
            // reflects the settled value on its next poll.
            bridge->store.reset_triggers_rt();
            return noErr;
        }

        // Process
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = bridge->sample_rate;
        ctx.num_samples = static_cast<int>(frameCount);
        // The host sets AUAudioUnit.renderingOffline for a faster-than-real-time
        // bounce; surface it so the processor can drive offline-only paths (e.g.
        // a GPU engine running synchronously) instead of dropping work to misses.
        const bool offline = bridge->rendering_offline.load(std::memory_order_acquire);
        ctx.process_mode = offline ? pulp::format::ProcessMode::Offline
                                   : pulp::format::ProcessMode::Realtime;
        ctx.render_speed_hint = offline ? pulp::format::RenderSpeedHint::FasterThanRealtime
                                        : pulp::format::RenderSpeedHint::Realtime;

        // Populate transport fields from the host blocks the AUv3 host
        // installed via the KVO-able properties on AUAudioUnit. The blocks may
        // legitimately be nil (hosts that don't expose transport state,
        // AUv2-bridged hosts, render tests). In that case the fields stay at
        // their documented defaults.
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

        std::array<pulp::format::ProcessBusBufferView<const float>, 2> input_buses{{
            {
                .info = {
                    .name = "Audio In",
                    .index = 0,
                    .direction = pulp::format::BusDirection::Input,
                    .role = pulp::format::BusRole::Main,
                    .declared_channels = bridge->input_channels,
                    .optional = bridge->input_channels == 0,
                    .active = input_view.num_channels() > 0,
                },
                .buffer = input_view,
            },
            {
                .info = {
                    .name = "Sidechain",
                    .index = 1,
                    .direction = pulp::format::BusDirection::Input,
                    .role = pulp::format::BusRole::Sidechain,
                    .declared_channels = bridge->sidechain_channels,
                    .optional = true,
                    .active = sidechain_view.num_channels() > 0,
                },
                .buffer = sidechain_view,
            },
        }};
        std::array<pulp::format::ProcessBusBufferView<float>, 1> output_buses{{
            {
                .info = {
                    .name = "Audio Out",
                    .index = 0,
                    .direction = pulp::format::BusDirection::Output,
                    .role = pulp::format::BusRole::Main,
                    .declared_channels = bridge->output_channels,
                    .optional = false,
                    .active = output_view.num_channels() > 0,
                },
                .buffer = output_view,
            },
        }};
        pulp::format::ProcessBuffers process_buffers{
            pulp::format::ProcessBusBufferSet<const float>{std::span(input_buses)},
            pulp::format::ProcessBusBufferSet<float>{std::span(output_buses)},
        };

        // MPE sidecar: run inbound MIDI through the voice tracker and attach the
        // resulting per-note expression buffer to the processor for the duration
        // of this process() call. Events are taken in host delivery order (like
        // CLAP; the AU render path does not sort midi_in). No-op (detaches the
        // buffer) when the plug-in did not opt into MPE. Allocation-free: the
        // buffer was reserved + capacity-limited in allocateRenderResources.
        bridge->mpe.run(*bridge->processor, midi_in);

        bridge->processor->set_param_events(&bridge->param_events);
        {
            // Adapter-owned DSP region: MIDI buffers are pre-reserved +
            // capacity-limited, param events are bounded, and the Processor
            // honours the no-alloc contract — so this must not allocate. The
            // tripwire (test builds / sanitizers) traps any stray allocation.
            // Matches VST3/CLAP/AU v2. The MIDI-output emit below stays OUTSIDE
            // the scope because it calls the host's MIDIOutputEventBlock (a
            // framework block outside our control).
            pulp::runtime::ScopedNoAlloc no_alloc_guard;
            bridge->processor->process(process_buffers, midi_in, midi_out, ctx);
        }

        // Return trigger / momentary params (panic, reset, tap) to their
        // default now that the Processor has observed this block, so the
        // host's parameter tree reads the control settled back on its own.
        bridge->store.reset_triggers_rt();

        // Drain RT-safe pending flags the processor may have set during
        // process() and publish them via KVO. AUAudioUnit exposes `latency`
        // and `tailTime` as KVO-able read-only properties; an AU v3 host
        // observes them and re-queries. dispatch_async (vs the synchronous KVO
        // call) keeps the audio thread out of Foundation's KVO machinery.
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

        // Forward any MIDI the Processor emitted to the host via the AUv3
        // MIDIOutputEventBlock. The block is installed by the host on an
        // ARC-managed retained property; we capture it via `self` at
        // block-creation time so the retain cycle stays safe.
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

- (NSUInteger)pulpLastParameterEventCapacity {
    return static_cast<NSUInteger>(_bridge.param_events.capacity());
}

- (BOOL)pulpLastParameterEventsOverflowed {
    return _bridge.param_events.overflowed() ? YES : NO;
}

- (uint32_t)pulpLastParameterEventDropCount {
    return _bridge.param_events.dropped_event_count();
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

// ARA-aware AU hosts (Logic Pro 11+, etc.) read this property via KVO during
// scan. Returns an opaque ARA::ARAFactory* when the plug-in participates in
// ARA; nullptr otherwise.
- (void *)audioUnitARAFactory {
    return const_cast<void *>(
        pulp::format::ara_companion_factory_for(nullptr));
}

// ── AU v3 view configuration negotiation: intentionally NOT implemented ─────
//
// We deliberately do NOT override `supportedViewConfigurations:` /
// `selectViewConfiguration:` for Pulp's fixed-design GPU editors.
//
// Why: Logic Pro drives AU v3 editor sizing through the view-configuration
// path and offers ONLY oversized ~4:3 configs (measured: 1024x768 / 1366x1024).
// When an AU opts in by returning any supported config, Logic LOCKS the editor
// window to that config's aspect ratio at every size (confirmed: a 900x520 /
// ~16:9.4 design forced into Logic's 4:3 window letterboxes with top/bottom
// bars that cannot be resized away). Apple's CoreAudioKit header states an
// empty index set means "use the largest available view configuration", so
// returning empty makes Logic pick its *largest* 4:3 config — strictly worse,
// not better.
//
// By NOT implementing these selectors at all, Logic falls back to the plain
// view at the controller's `preferredContentSize` and lets the window
// free-resize to the design's own aspect — matching the tight, proportional
// fit Pulp already gets in REAPER, CLAP, VST3, and standalone. (REAPER/CLAP/
// VST3 never used these selectors, so this is a no-op for them.)
//
// If a future non-fixed (truly fluid / multi-config) editor needs host view
// configurations, reintroduce these selectors gated on that editor kind — but
// keep them OFF for design-viewport / `set_fixed_aspect_ratio` editors.

@end
