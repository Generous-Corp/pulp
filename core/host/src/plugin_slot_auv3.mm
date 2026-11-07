// Audio Unit slot built on AUAudioUnit (macOS).
//
// This is the slot for AU v3 app extensions, and it hosts AU v2 components
// too, which the system wraps as AUAudioUnit. By default the plugin runs out of
// process (kAudioComponentInstantiation_LoadOutOfProcess): the system hosts it
// in its Audio Unit hosting service, and this slot talks to it through the
// AUAudioUnit proxy.
//
// What the plugin gets:
//   - every input and output bus, each at the width the plugin accepted (the
//     main buses take the caller's preferred layout when it gives one), bound
//     to the host's buses by SlotBusBinder;
//   - MIDI and SysEx through scheduleMIDIEventBlock, and parameter automation
//     through scheduleParameterBlock, each at its sample offset;
//   - the host transport through musicalContextBlock and transportStateBlock,
//     and the prepared process mode as renderingOffline;
//   - MIDI it emits comes back through MIDIOutputEventBlock;
//   - parameter changes it makes (in its editor or from its own processing)
//     come back through the parameter tree's automation observer, gestures
//     included, as drain_parameter_edits();
//   - a latency change (KVO on `latency`) is a restart request, so a graph
//     rebuilds its delay compensation through apply_plugin_restarts();
//   - state is `fullState` as a binary property list;
//   - the editor is the view controller from
//     requestViewControllerWithCompletionHandler, adopted into a host-owned
//     container.
//
// Threads: prepare, release, state, parameters, and the editor are control
// thread calls. process() never allocates, locks, or sends an Objective-C
// message: the render, MIDI, and parameter blocks are fetched in prepare().

#include <pulp/host/plugin_slot_auv3.hpp>

#include <pulp/format/process_context.hpp>
#include <pulp/host/hosted_editor_container.hpp>
#include <pulp/host/native_handle_visitor.hpp>
#include <pulp/runtime/log.hpp>

#include "plugin_slot_process_request.hpp"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pulp::host {
class AuV3Slot;
}

// Forwards KVO on the unit's `latency` to the slot.
@interface PulpHostAuV3LatencyObserver : NSObject {
  @public
    pulp::host::AuV3Slot* slot;
}
@end

namespace pulp::host {
namespace {

bool parse_component_id(const std::string& id, AudioComponentDescription& desc) {
    if (id.size() != 14 || id[4] != ':' || id[9] != ':')
        return false;
    auto pack = [](const char* p) -> OSType {
        return (static_cast<OSType>(static_cast<std::uint8_t>(p[0])) << 24) |
               (static_cast<OSType>(static_cast<std::uint8_t>(p[1])) << 16) |
               (static_cast<OSType>(static_cast<std::uint8_t>(p[2])) << 8) |
               static_cast<OSType>(static_cast<std::uint8_t>(p[3]));
    };
    desc = {};
    desc.componentType = pack(id.data());
    desc.componentSubType = pack(id.data() + 5);
    desc.componentManufacturer = pack(id.data() + 10);
    return true;
}

/// Wait for `done`, keeping the main run loop turning when called on the main
/// thread: the system may deliver a completion handler there.
bool wait_for(dispatch_semaphore_t done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (![NSThread isMainThread]) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
        return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, ns)) == 0;
    }
    while (std::chrono::steady_clock::now() < deadline) {
        if (dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) == 0)
            return true;
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                 beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    }
    return dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) == 0;
}

std::string to_string(NSString* s) {
    return s ? std::string([s UTF8String]) : std::string();
}

/// The transport a block runs with, copied before render so the context
/// blocks read plain values.
struct TransportSnapshot {
    bool valid = false;
    bool playing = false;
    bool recording = false;
    bool looping = false;
    double tempo = 120.0;
    double beats = 0.0;
    double samples = 0.0;
    double bar_start_beats = 0.0;
    double loop_start = 0.0;
    double loop_end = 0.0;
    double numerator = 4.0;
    NSInteger denominator = 4;
    bool moved = false;
};

/// Parameter id and AUParameterAddress, sorted by address for lookup from
/// observer callbacks.
struct AddressMapping {
    AUParameterAddress address = 0;
    std::uint32_t id = 0;
};

} // namespace

class AuV3Slot final : public detail::RequestDrivenSlot {
  public:
    AuV3Slot(PluginInfo info, AUAudioUnit* unit) : info_(std::move(info)), unit_([unit retain]) {
        is_instrument_ = info_.is_instrument;
        build_parameters();
        latency_observer_ = [[PulpHostAuV3LatencyObserver alloc] init];
        latency_observer_->slot = this;
        [unit_ addObserver:latency_observer_
                forKeyPath:@"latency"
                   options:NSKeyValueObservingOptionNew
                   context:nullptr];
    }

    ~AuV3Slot() override {
        close_editor();
        release();
        if (automation_token_) {
            [unit_.parameterTree removeParameterObserver:automation_token_];
            automation_token_ = nullptr;
        }
        [unit_ removeObserver:latency_observer_ forKeyPath:@"latency"];
        latency_observer_->slot = nullptr;
        [latency_observer_ release];
        clear_blocks();
        // The system's AU v2 bridge delivers a plugin's parameter events on its
        // own queue, after the plugin posted them. Disposing the unit while
        // some are still queued crashes that queue, so the last reference is
        // dropped once it has had time to drain.
        AUAudioUnit* unit = unit_;
        unit_ = nil;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kDisposeDelayNs),
                       dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                         [unit release];
                       });
    }

    /// From KVO, on whichever thread the unit changed its latency.
    void latency_changed() {
        note_restart_request();
    }

    const PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return unit_ != nil;
    }

    void set_preferred_channel_layout(int inputs, int outputs) override {
        preferred_inputs_ = inputs;
        preferred_outputs_ = outputs;
    }

    bool prepare(double sample_rate, int max_block_size) override {
        if (!unit_ || max_block_size <= 0)
            return false;
        release();
        sample_rate_ = sample_rate;
        max_block_ = max_block_size;
        const auto mode = settings().process_mode;
        @autoreleasepool {
            NSError* error = nil;
            if ([unit_ respondsToSelector:@selector(setRenderingOffline:)])
                unit_.renderingOffline = mode == format::ProcessMode::Offline;
            unit_.maximumFramesToRender = static_cast<AUAudioFrameCount>(max_block_size);
            if (!configure_buses(sample_rate))
                return false;
            install_context_blocks();
            if (![unit_ allocateRenderResourcesAndReturnError:&error]) {
                runtime::log_error("AUv3: allocateRenderResources failed for '{}': {}", info_.name,
                                   to_string(error.localizedDescription));
                clear_context_blocks();
                return false;
            }
            render_ = [unit_.renderBlock copy];
            midi_block_ = [unit_.scheduleMIDIEventBlock copy];
            param_block_ = [unit_.scheduleParameterBlock copy];
            AuV3Slot* self = this;
            pull_input_ = [^AUAudioUnitStatus(AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                                              AUAudioFrameCount frames, NSInteger bus,
                                              AudioBufferList* list) {
              return self->pull_input(static_cast<std::size_t>(bus), frames, list);
            } copy];
        }
        binder_.prepare(input_decls_, output_decls_, max_block_size, false);
        allocate_render_storage();
        sample_time_ = 0.0;
        set_prepared_mode(mode);
        prepared_ = true;
        return true;
    }

    void release() override {
        if (!prepared_)
            return;
        prepared_ = false;
        [unit_ deallocateRenderResources];
        clear_blocks();
        clear_context_blocks();
    }

    std::vector<HostParamInfo> parameters() const override {
        return params_;
    }

    float get_parameter(std::uint32_t id) const override {
        AUParameter* p = parameter_for(id);
        return p ? p.value : 0.0f;
    }

    void set_parameter(std::uint32_t id, float plain_value) override {
        AUParameter* p = parameter_for(id);
        if (!p)
            return;
        // The observer token as originator keeps the host's own edit from
        // coming back as a plugin edit.
        [p setValue:plain_value originator:automation_token_];
    }

    void set_bypass(bool b) override {
        bypassed_.store(b, std::memory_order_relaxed);
        if (unit_ && [unit_ respondsToSelector:@selector(setShouldBypassEffect:)])
            unit_.shouldBypassEffect = b;
    }
    bool is_bypassed() const override {
        return bypassed_.load(std::memory_order_relaxed);
    }
    BypassSurface bypass_surface() const override {
        return BypassSurface::unit_property;
    }

    std::vector<std::uint8_t> save_state() const override {
        @autoreleasepool {
            NSDictionary* state = unit_.fullState;
            if (!state)
                return {};
            NSError* error = nil;
            NSData* data =
                [NSPropertyListSerialization dataWithPropertyList:state
                                                           format:NSPropertyListBinaryFormat_v1_0
                                                          options:0
                                                            error:&error];
            if (!data)
                return {};
            const auto* bytes = static_cast<const std::uint8_t*>(data.bytes);
            return {bytes, bytes + data.length};
        }
    }

    bool restore_state(const std::vector<std::uint8_t>& bytes) override {
        if (bytes.empty())
            return false;
        @autoreleasepool {
            NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
            NSError* error = nil;
            id plist = [NSPropertyListSerialization propertyListWithData:data
                                                                 options:NSPropertyListImmutable
                                                                  format:nullptr
                                                                   error:&error];
            if (![plist isKindOfClass:[NSDictionary class]])
                return false;
            unit_.fullState = plist;
            note_state_change();
            return true;
        }
    }

    bool has_editor() const override {
        return unit_ && unit_.providesUserInterface;
    }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    std::unique_ptr<HostedEditor> create_hosted_editor(void* parent_window) override {
        if (!unit_ || !parent_window || editor_container_)
            return nullptr;
        // An out-of-process editor is a remote view, which AppKit can only
        // build in an application that has finished launching; earlier it
        // throws instead of failing.
        if (!NSApp) {
            runtime::log_error("AUv3 editor: '{}' needs an NSApplication to show its editor",
                               info_.name);
            return nullptr;
        }
        __block NSViewController* controller = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        @try {
            [unit_ requestViewControllerWithCompletionHandler:^(AUViewControllerBase* vc) {
              controller = [vc retain];
              dispatch_semaphore_signal(done);
            }];
        } @catch (NSException* e) {
            runtime::log_error("AUv3 editor: '{}' could not open its editor: {}", info_.name,
                               to_string(e.reason));
            dispatch_release(done);
            return nullptr;
        }
        const bool arrived = wait_for(done, kEditorTimeout);
        dispatch_release(done);
        if (!arrived || !controller) {
            runtime::log_error("AUv3 editor: '{}' gave no view controller", info_.name);
            [controller release];
            return nullptr;
        }
        NSView* view = controller.view;
        NSSize size = controller.preferredContentSize;
        if (size.width < 1 || size.height < 1)
            size = view.frame.size;
        if (size.width < 1 || size.height < 1)
            size = NSMakeSize(400, 300);
        const auto width = static_cast<std::uint32_t>(size.width);
        const auto height = static_cast<std::uint32_t>(size.height);
        void* container = create_editor_container(parent_window, width, height);
        if (!container ||
            !editor_container_adopt_view(container, (__bridge void*)view, width, height)) {
            if (container)
                destroy_editor_container(container);
            [controller release];
            return nullptr;
        }
        editor_container_ = container;
        editor_controller_ = controller;
        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = container;
        ed->width = width;
        ed->height = height;
        ed->resizable = true;
        return ed;
    }

    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        if (ed && ed->native_handle == editor_container_)
            close_editor();
    }

    bool set_hosted_editor_size(std::uint32_t& width, std::uint32_t& height) override {
        if (!editor_container_)
            return false;
        resize_editor_container(editor_container_, width, height);
        return true;
    }

    int latency_samples() const override {
        return unit_ ? static_cast<int>(std::lround(unit_.latency * sample_rate_)) : 0;
    }
    LatencyQuery latency_query() const override {
        return unit_ ? LatencyQuery::Available : LatencyQuery::Unsupported;
    }
    int tail_samples() const override {
        return unit_ ? static_cast<int>(std::lround(unit_.tailTime * sample_rate_)) : 0;
    }

    void accept(NativeHandleVisitor& visitor) const override {
        AudioUnitV3NativeHandle ext;
        ext.audio_unit = (__bridge void*)unit_;
        visitor.visit_audio_unit_v3(*this, ext);
    }

    void begin_process() noexcept override {
        process_status_.store(ProcessStatus::ok, std::memory_order_release);
    }
    ProcessStatus process_status() const noexcept override {
        return process_status_.load(std::memory_order_acquire);
    }

  protected:
    void process_request(const detail::SlotProcessRequest& request) override {
        auto& audio = *request.audio;
        const int frames = request.num_samples;
        if (!prepared_ || bypassed_.load(std::memory_order_relaxed) || frames <= 0) {
            detail::SlotBusBinder::pass_through(audio, frames);
            sample_time_ += std::max(0, frames);
            return;
        }
        if (!binder_.bind(audio, frames)) {
            process_status_.store(ProcessStatus::failed, std::memory_order_release);
            detail::SlotBusBinder::pass_through(audio, frames);
            sample_time_ += frames;
            return;
        }

        snapshot_transport(request.transport);
        schedule_events(request);
        midi_out_ = request.midi_out;

        AudioTimeStamp stamp{};
        stamp.mSampleTime = sample_time_;
        stamp.mFlags = kAudioTimeStampSampleTimeValid;
        bool ok = true;
        for (std::size_t bus = 0; bus < output_lists_.size() && ok; ++bus) {
            auto* list = fill_output_list(bus, static_cast<std::uint32_t>(frames));
            AudioUnitRenderActionFlags flags = 0;
            const AUAudioUnitStatus status =
                render_(&flags, &stamp, static_cast<AUAudioFrameCount>(frames),
                        static_cast<NSInteger>(bus), list, pull_input_);
            if (status != noErr) {
                ok = false;
                break;
            }
            copy_back_output(bus, list, static_cast<std::uint32_t>(frames));
        }
        midi_out_ = nullptr;
        if (!ok) {
            process_status_.store(ProcessStatus::failed, std::memory_order_release);
            detail::SlotBusBinder::pass_through(audio, frames);
        } else {
            binder_.finish(audio, frames);
        }
        sample_time_ += frames;
    }

  private:
    static constexpr std::chrono::milliseconds kEditorTimeout{5000};
    static constexpr std::int64_t kDisposeDelayNs = 500 * NSEC_PER_MSEC;

    // ── Parameters ──────────────────────────────────────────────────────

    void build_parameters() {
        params_.clear();
        by_id_.clear();
        by_address_.clear();
        NSArray<AUParameter*>* all = unit_.parameterTree.allParameters;
        bool addresses_fit = true;
        for (AUParameter* p in all)
            if (p.address > 0xFFFFFFFFull)
                addresses_fit = false;
        // A unit with per-bus parameters repeats display names ("Gain" on
        // every input); those get their key path so a host can tell them apart.
        NSCountedSet* names = [[[NSCountedSet alloc] init] autorelease];
        for (AUParameter* p in all)
            if (p.displayName)
                [names addObject:p.displayName];
        std::uint32_t ordinal = 0;
        for (AUParameter* p in all) {
            const std::uint32_t id =
                addresses_fit ? static_cast<std::uint32_t>(p.address) : ordinal;
            ++ordinal;
            HostParamInfo h;
            h.id = id;
            h.name = to_string(p.displayName);
            if (p.displayName && [names countForObject:p.displayName] > 1 && p.keyPath)
                h.name += " (" + to_string(p.keyPath) + ")";
            h.min_value = p.minValue;
            h.max_value = p.maxValue;
            h.default_value = p.value;
            const auto flags = p.flags;
            const bool writable = (flags & kAudioUnitParameterFlag_IsWritable) != 0;
            h.flags.read_only = !writable;
            h.flags.automatable = writable;
            h.flags.stepped = p.unit == kAudioUnitParameterUnit_Indexed ||
                              p.unit == kAudioUnitParameterUnit_Boolean ||
                              (flags & kAudioUnitParameterFlag_ValuesHaveStrings) != 0;
            h.flags.rampable = !h.flags.stepped;
            h.flags.modulatable = false;
            params_.push_back(std::move(h));
            by_id_.push_back({p.address, id});
            parameters_.push_back(p);
        }
        by_address_ = by_id_;
        std::sort(
            by_address_.begin(), by_address_.end(),
            [](const AddressMapping& a, const AddressMapping& b) { return a.address < b.address; });

        AuV3Slot* self = this;
        automation_token_ = [unit_.parameterTree
            tokenByAddingParameterAutomationObserver:^(NSInteger count,
                                                       const AUParameterAutomationEvent* events) {
              for (NSInteger i = 0; i < count; ++i)
                  self->plugin_parameter_event(events[i]);
            }];
    }

    AUParameter* parameter_for(std::uint32_t id) const {
        for (std::size_t i = 0; i < by_id_.size(); ++i)
            if (by_id_[i].id == id)
                return parameters_[i];
        return nil;
    }

    bool address_for(std::uint32_t id, AUParameterAddress& address) const noexcept {
        for (const auto& m : by_id_)
            if (m.id == id) {
                address = m.address;
                return true;
            }
        return false;
    }

    void plugin_parameter_event(const AUParameterAutomationEvent& event) {
        const auto it = std::lower_bound(
            by_address_.begin(), by_address_.end(), event.address,
            [](const AddressMapping& m, AUParameterAddress a) { return m.address < a; });
        if (it == by_address_.end() || it->address != event.address)
            return;
        PluginParameterEdit edit;
        edit.id = it->id;
        switch (event.eventType) {
        case AUParameterAutomationEventTypeTouch:
            edit.kind = PluginParameterEdit::Kind::begin_gesture;
            break;
        case AUParameterAutomationEventTypeRelease:
            edit.kind = PluginParameterEdit::Kind::end_gesture;
            break;
        default:
            edit.kind = PluginParameterEdit::Kind::value;
            edit.plain_value = event.value;
            break;
        }
        if (detail::AudioThreadScope::active())
            edit_inbox().push_from_audio(edit);
        else
            edit_inbox().push_from_control(edit);
    }

    // ── Buses ───────────────────────────────────────────────────────────

    bool configure_buses(double sample_rate) {
        input_decls_.clear();
        output_decls_.clear();
        auto configure = [&](AUAudioUnitBusArray* buses, int preferred,
                             std::vector<detail::SlotBusDecl>& decls) {
            for (NSUInteger b = 0; b < buses.count; ++b) {
                AUAudioUnitBus* bus = buses[b];
                const auto current = static_cast<int>(bus.format.channelCount);
                int want = current > 0 ? current : 2;
                if (b == 0 && preferred > 0)
                    want = preferred;
                NSError* error = nil;
                AVAudioFormat* format = [[AVAudioFormat alloc]
                    initStandardFormatWithSampleRate:sample_rate
                                            channels:static_cast<AVAudioChannelCount>(want)];
                BOOL accepted = format && [bus setFormat:format error:&error];
                [format release];
                if (!accepted && want != current && current > 0) {
                    format = [[AVAudioFormat alloc]
                        initStandardFormatWithSampleRate:sample_rate
                                                channels:static_cast<AVAudioChannelCount>(current)];
                    accepted = format && [bus setFormat:format error:&error];
                    [format release];
                }
                if (!accepted) {
                    runtime::log_error("AUv3: '{}' refused a {} Hz format on bus {}", info_.name,
                                       sample_rate, static_cast<unsigned>(b));
                    return false;
                }
                // Out of process, input buses start disabled; a disabled bus is
                // never pulled and the render fails.
                bus.enabled = YES;
                decls.push_back({static_cast<int>(bus.format.channelCount), b == 0});
            }
            return true;
        };
        const int out_pref = preferred_outputs_;
        const int in_pref = preferred_inputs_ >= 0 ? preferred_inputs_ : preferred_outputs_;
        return configure(unit_.inputBusses, in_pref, input_decls_) &&
               configure(unit_.outputBusses, out_pref, output_decls_);
    }

    void allocate_render_storage() {
        const auto frames = static_cast<std::size_t>(max_block_);
        output_lists_.assign(output_decls_.size(), {});
        output_targets_.assign(output_decls_.size(), {});
        for (std::size_t b = 0; b < output_decls_.size(); ++b) {
            const auto channels = static_cast<std::size_t>(std::max(1, output_decls_[b].channels));
            output_lists_[b].assign(sizeof(AudioBufferList) + sizeof(AudioBuffer) * (channels - 1),
                                    0);
            output_targets_[b].assign(channels, nullptr);
        }
        input_scratch_.assign(input_decls_.size(), {});
        for (std::size_t b = 0; b < input_decls_.size(); ++b)
            input_scratch_[b].assign(
                static_cast<std::size_t>(std::max(0, input_decls_[b].channels)),
                std::vector<float>(frames, 0.0f));
    }

    AudioBufferList* fill_output_list(std::size_t bus, std::uint32_t frames) noexcept {
        auto* list = reinterpret_cast<AudioBufferList*>(output_lists_[bus].data());
        const auto channels = binder_.output_channels(bus);
        float** targets = binder_.output32(bus);
        list->mNumberBuffers = channels;
        for (std::uint32_t c = 0; c < channels; ++c) {
            output_targets_[bus][c] = targets[c];
            list->mBuffers[c].mNumberChannels = 1;
            list->mBuffers[c].mDataByteSize = frames * sizeof(float);
            list->mBuffers[c].mData = targets[c];
        }
        return list;
    }

    /// A unit may render into its own buffers and hand those back instead.
    void copy_back_output(std::size_t bus, const AudioBufferList* list,
                          std::uint32_t frames) noexcept {
        for (std::uint32_t c = 0; c < list->mNumberBuffers; ++c) {
            float* target = output_targets_[bus][c];
            const void* produced = list->mBuffers[c].mData;
            if (target && produced && produced != target)
                std::memcpy(target, produced, frames * sizeof(float));
        }
    }

    AUAudioUnitStatus pull_input(std::size_t bus, AUAudioFrameCount frames,
                                 AudioBufferList* list) noexcept {
        if (bus >= binder_.input_count() || frames > static_cast<AUAudioFrameCount>(max_block_))
            return kAudioUnitErr_InvalidElement;
        float** source = binder_.input32(bus);
        const auto channels = binder_.input_channels(bus);
        for (std::uint32_t c = 0; c < list->mNumberBuffers; ++c) {
            auto& buffer = list->mBuffers[c];
            if (!buffer.mData) {
                // The host owns the memory; the unit may process in place, so
                // hand it a copy rather than the host's input.
                if (c >= input_scratch_[bus].size())
                    return kAudioUnitErr_InvalidPropertyValue;
                buffer.mData = input_scratch_[bus][c].data();
            }
            buffer.mDataByteSize = frames * sizeof(float);
            if (c < channels && source[c])
                std::memcpy(buffer.mData, source[c], frames * sizeof(float));
            else
                std::memset(buffer.mData, 0, frames * sizeof(float));
        }
        return noErr;
    }

    // ── Events and transport ────────────────────────────────────────────

    void schedule_events(const detail::SlotProcessRequest& request) noexcept {
        if (midi_block_) {
            detail::for_each_midi_in_time_order(
                *request.midi_in,
                [&](const midi::MidiEvent& e) {
                    const auto length = e.message.length();
                    if (length < 1 || length > 3)
                        return;
                    midi_block_(AUEventSampleTimeImmediate + e.sample_offset, 0,
                                static_cast<NSInteger>(length), e.message.data());
                },
                [&](const auto& sysex) {
                    if (sysex.data.empty())
                        return;
                    midi_block_(AUEventSampleTimeImmediate + sysex.sample_offset, 0,
                                static_cast<NSInteger>(sysex.data.size()), sysex.data.data());
                });
        }
        if (param_block_) {
            for (const auto& pe : *request.param_events) {
                AUParameterAddress address = 0;
                if (address_for(pe.param_id, address))
                    param_block_(AUEventSampleTimeImmediate + pe.sample_offset, 0, address,
                                 pe.value);
            }
        }
    }

    void snapshot_transport(const format::ProcessContext* t) noexcept {
        auto& s = transport_;
        if (!t) {
            s.valid = false;
            return;
        }
        s.valid = true;
        s.moved = t->transport_jump || t->transport_changed || t->is_playing != s.playing;
        s.playing = t->is_playing;
        s.recording = t->is_recording;
        s.looping = t->is_looping;
        s.tempo = t->tempo_bpm;
        s.beats = t->position_beats;
        s.samples = static_cast<double>(t->position_samples);
        s.numerator = std::max(1, t->time_sig_numerator);
        s.denominator = std::max(1, t->time_sig_denominator);
        const double bar_quarters = s.numerator * 4.0 / static_cast<double>(s.denominator);
        s.bar_start_beats = std::floor(s.beats / bar_quarters) * bar_quarters;
        s.loop_start = t->loop_start_beats;
        s.loop_end = t->loop_end_beats;
    }

    void install_context_blocks() {
        clear_context_blocks();
        if (!settings().deliver_transport)
            return;
        AuV3Slot* self = this;
        unit_.musicalContextBlock = ^BOOL(double* tempo, double* numerator, NSInteger* denominator,
                                          double* beat, NSInteger* to_next_beat, double* downbeat) {
          const auto& s = self->transport_;
          if (!s.valid)
              return NO;
          if (tempo)
              *tempo = s.tempo;
          if (numerator)
              *numerator = s.numerator;
          if (denominator)
              *denominator = s.denominator;
          if (beat)
              *beat = s.beats;
          if (to_next_beat) {
              const double frac = s.beats - std::floor(s.beats);
              const double samples_per_beat = 60.0 / std::max(1.0, s.tempo) * self->sample_rate_;
              *to_next_beat =
                  frac == 0.0
                      ? 0
                      : static_cast<NSInteger>(std::lround((1.0 - frac) * samples_per_beat));
          }
          if (downbeat)
              *downbeat = s.bar_start_beats;
          return YES;
        };
        unit_.transportStateBlock = ^BOOL(AUHostTransportStateFlags* flags, double* position,
                                          double* cycle_start, double* cycle_end) {
          const auto& s = self->transport_;
          if (!s.valid)
              return NO;
          if (flags) {
              AUHostTransportStateFlags f = 0;
              if (s.moved)
                  f |= AUHostTransportStateChanged;
              if (s.playing)
                  f |= AUHostTransportStateMoving;
              if (s.recording)
                  f |= AUHostTransportStateRecording;
              if (s.looping)
                  f |= AUHostTransportStateCycling;
              *flags = f;
          }
          if (position)
              *position = s.samples;
          if (cycle_start)
              *cycle_start = s.loop_start;
          if (cycle_end)
              *cycle_end = s.loop_end;
          return YES;
        };
        if ([unit_ respondsToSelector:@selector(setMIDIOutputEventBlock:)]) {
            unit_.MIDIOutputEventBlock =
                ^OSStatus(AUEventSampleTime time, uint8_t, NSInteger length, const uint8_t* bytes) {
                  return self->plugin_midi(time, length, bytes);
                };
        }
    }

    OSStatus plugin_midi(AUEventSampleTime time, NSInteger length, const std::uint8_t* bytes) {
        midi::MidiBuffer* out = midi_out_;
        if (!out || length < 1 || !bytes)
            return noErr;
        // Times are absolute sample times; the block started at sample_time_.
        const auto offset = static_cast<std::int32_t>(
            std::clamp<double>(static_cast<double>(time) - sample_time_, 0.0,
                               static_cast<double>(std::max(0, max_block_ - 1))));
        if (bytes[0] == 0xF0) {
            out->add_sysex_copy(bytes, static_cast<std::size_t>(length), offset, 0.0);
            return noErr;
        }
        if (length > 3)
            return noErr;
        midi::MidiEvent e;
        e.message = choc::midi::ShortMessage(bytes[0], length > 1 ? bytes[1] : 0,
                                             length > 2 ? bytes[2] : 0);
        e.sample_offset = offset;
        out->add(e);
        return noErr;
    }

    void clear_context_blocks() {
        if (!unit_)
            return;
        unit_.musicalContextBlock = nil;
        unit_.transportStateBlock = nil;
        if ([unit_ respondsToSelector:@selector(setMIDIOutputEventBlock:)])
            unit_.MIDIOutputEventBlock = nil;
    }

    void clear_blocks() {
        [render_ release];
        render_ = nil;
        [midi_block_ release];
        midi_block_ = nil;
        [param_block_ release];
        param_block_ = nil;
        [pull_input_ release];
        pull_input_ = nil;
    }

    void close_editor() {
        if (editor_container_) {
            destroy_editor_container(editor_container_);
            editor_container_ = nullptr;
        }
        [editor_controller_ release];
        editor_controller_ = nil;
    }

    PluginInfo info_;
    AUAudioUnit* unit_ = nil;
    PulpHostAuV3LatencyObserver* latency_observer_ = nil;
    AUParameterObserverToken automation_token_ = nullptr;
    bool is_instrument_ = false;

    std::vector<HostParamInfo> params_;
    std::vector<AUParameter*> parameters_; // Not retained: the tree owns them.
    std::vector<AddressMapping> by_id_;
    std::vector<AddressMapping> by_address_;

    AURenderBlock render_ = nil;
    AUScheduleMIDIEventBlock midi_block_ = nil;
    AUScheduleParameterBlock param_block_ = nil;
    AURenderPullInputBlock pull_input_ = nil;

    std::vector<detail::SlotBusDecl> input_decls_;
    std::vector<detail::SlotBusDecl> output_decls_;
    detail::SlotBusBinder binder_;
    std::vector<std::vector<std::uint8_t>> output_lists_;
    std::vector<std::vector<float*>> output_targets_;
    std::vector<std::vector<std::vector<float>>> input_scratch_;

    TransportSnapshot transport_;
    midi::MidiBuffer* midi_out_ = nullptr;
    double sample_time_ = 0.0;
    double sample_rate_ = 48000.0;
    int max_block_ = 0;
    int preferred_inputs_ = -1;
    int preferred_outputs_ = -1;
    bool prepared_ = false;
    std::atomic<bool> bypassed_{false};
    std::atomic<ProcessStatus> process_status_{ProcessStatus::ok};

    void* editor_container_ = nullptr;
    NSViewController* editor_controller_ = nil;
};

std::unique_ptr<PluginSlot> load_auv3_plugin(const PluginInfo& requested,
                                             const AuV3LoadOptions& options) {
    AudioComponentDescription desc{};
    if (!parse_component_id(requested.unique_id, desc)) {
        runtime::log_error("AUv3 load: unique_id '{}' is not TYPE:SUBT:MANU", requested.unique_id);
        return nullptr;
    }
    if (!AudioComponentFindNext(nullptr, &desc)) {
        runtime::log_error("AUv3 load: no component registered for '{}'", requested.unique_id);
        return nullptr;
    }
    __block AUAudioUnit* unit = nil;
    __block NSError* failure = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    const AudioComponentInstantiationOptions how =
        options.out_of_process ? kAudioComponentInstantiation_LoadOutOfProcess
                               : kAudioComponentInstantiation_LoadInProcess;
    [AUAudioUnit instantiateWithComponentDescription:desc
                                             options:how
                                   completionHandler:^(AUAudioUnit* u, NSError* e) {
                                     unit = [u retain];
                                     failure = [e retain];
                                     dispatch_semaphore_signal(done);
                                   }];
    const bool arrived = wait_for(done, options.instantiate_timeout);
    if (!arrived) {
        // The handler may still run; leave the semaphore and its captures to it.
        runtime::log_error("AUv3 load: '{}' did not instantiate within {} ms", requested.unique_id,
                           options.instantiate_timeout.count());
        return nullptr;
    }
    dispatch_release(done);
    if (!unit) {
        runtime::log_error("AUv3 load: '{}' failed to instantiate: {}", requested.unique_id,
                           failure ? to_string(failure.localizedDescription) : std::string());
        [failure release];
        return nullptr;
    }
    [failure release];

    PluginInfo info = requested;
    if (info.name.empty())
        info.name = to_string(unit.audioUnitName);
    if (info.manufacturer.empty())
        info.manufacturer = to_string(unit.manufacturerName);
    info.is_instrument = desc.componentType == kAudioUnitType_MusicDevice;
    info.is_effect = !info.is_instrument && desc.componentType != kAudioUnitType_Generator;
    auto slot = std::make_unique<AuV3Slot>(std::move(info), unit);
    [unit release];
    return slot;
}

} // namespace pulp::host

@implementation PulpHostAuV3LatencyObserver
- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
    if (slot)
        slot->latency_changed();
}
@end
