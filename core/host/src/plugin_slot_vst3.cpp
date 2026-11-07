// VST3 plugin slot implementation.
//
// Loads a .vst3 bundle, resolves GetPluginFactory, creates the first audio
// processor class, and wires the PluginSlot interface onto IComponent +
// IAudioProcessor. The host covers every audio bus the plug-in declares
// (sidechain inputs and aux outputs included), host transport through
// ProcessContext, offline and 64-bit processing, the full MIDI 1.0 map in both
// directions (notes, poly pressure, sysex as events; controllers, channel
// pressure, pitch bend and program change through IMidiMapping), parameter
// feedback through IComponentHandler, latency and restart requests, state
// serialization, and an embedded IPlugView editor.

#include <pulp/host/plugin_slot.hpp>
#include <pulp/runtime/log.hpp>

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/vsttypes.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/base/ibstream.h>

// SDK helper containers used for block-local event and parameter queues, plus
// the reference IHostApplication (IMessage / IAttributeList factory).
#include <public.sdk/source/vst/hosting/parameterchanges.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
// hostclasses.h declares virtual destructors on `final` classes, which newer
// clang flags. The SDK target silences its own warnings with -w; this include
// pulls the header into Pulp's own (warning-clean) translation unit.
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wunknown-warning-option"
#  pragma clang diagnostic ignored "-Wunnecessary-virtual-specifier"
#endif
#include <public.sdk/source/vst/hosting/hostclasses.h>
#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

#include "plugin_slot_vst3_internal.hpp"
#include "plugin_slot_process_request.hpp"

#include <pulp/host/dl_shim.hpp>
#include <pulp/host/detail/vst3_connection.hpp>
#include <pulp/host/detail/vst3_state_sync.hpp>
#include <pulp/host/detail/vst3_editor.hpp>
#include <pulp/host/hosted_editor_container.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pulp::host {
namespace {

namespace fs = std::filesystem;
using namespace Steinberg;

// Resolve path to the actual loadable binary inside a .vst3 bundle.
// macOS: <Name>.vst3/Contents/MacOS/<Name>
// Linux: <Name>.vst3/Contents/x86_64-linux/<Name>.so  or <Name>.vst3 (flat)
// Windows: <Name>.vst3/Contents/<arch>-win/<Name>.vst3
std::string resolve_vst3_binary(const std::string& path) {
    fs::path p(path);
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return path;
    auto stem = p.stem().string();
#if defined(__APPLE__)
    auto inner = p / "Contents" / "MacOS" / stem;
#elif defined(__linux__)
    auto inner = p / "Contents" / "x86_64-linux" / (stem + ".so");
    if (!fs::exists(inner, ec)) inner = p / "Contents" / "aarch64-linux" / (stem + ".so");
#elif defined(_WIN32)
    // CMake's BUNDLE property is ignored for Windows MODULE targets.  The
    // Pulp format helper therefore assembles a standard VST3 bundle with an
    // architecture-specific Windows directory.  Discover the directory
    // instead of hard-coding x64 so the same host loads ARM64 and x86_64
    // plugins (and remains tolerant of future Windows architectures).
    auto contents = p / "Contents";
    if (fs::is_directory(contents, ec)) {
        for (const auto& entry : fs::directory_iterator(contents, ec)) {
            if (ec || !entry.is_directory(ec))
                continue;
            const auto arch = entry.path().filename().string();
            if (arch.size() < 4 || arch.substr(arch.size() - 4) != "-win")
                continue;
            auto candidate = entry.path() / (stem + ".vst3");
            if (fs::exists(candidate, ec))
                return candidate.string();
        }
    }
    auto inner = p / (stem + ".dll");
#else
    auto inner = p;
#endif
    if (fs::exists(inner, ec)) return inner.string();
    return path;
}

// The IHostApplication handed to IComponent / IEditController::initialize().
//
// The SDK's HostApplication supplies the two services a hosted plug-in actually
// asks its host for: createInstance() for the IMessage / IAttributeList objects
// that all connection-point traffic is built from, and IPlugInterfaceSupport so
// the plug-in can discover which host interfaces exist. A host application whose
// createInstance() returns kNotImplemented leaves a separated plug-in unable to
// send its own controller a single message.
//
// The instance is a process-wide singleton (host_app()), so reference counting
// is neutered: a plug-in that over-releases must not be able to delete it.
class HostApp final : public Vst::HostApplication {
public:
    tresult PLUGIN_API getName(Vst::String128 name) override {
        if (!name)
            return kInvalidArgument;
        // UTF-8 is narrowed byte-wise; host names are expected to be ASCII.
        const std::string host_name = plugin_host_identity().name;
        int i = 0;
        for (; i < 127 && i < (int)host_name.size(); ++i)
            name[i] = (Vst::TChar)(unsigned char)host_name[(size_t)i];
        name[i] = 0;
        return kResultTrue;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

// The host's IPlugFrame.
//
// IPlugView needs one to exist before attached(): the plug-in calls
// resizeView() from inside attached() to report the size it really wants, and a
// view with no frame either mis-sizes itself or refuses to attach outright.
// Lifetime belongs to the slot that owns it, so reference counting is neutered;
// vst3_release_editor_view() clears the plug-in's pointer to it before the last
// release, so a plug-in can never call into a destroyed frame.
class EditorFrame final : public IPlugFrame {
public:
    using ResizeHandler = std::function<tresult(IPlugView*, ViewRect*)>;

    void set_handler(ResizeHandler handler) { handler_ = std::move(handler); }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* new_size) override {
        return handler_ ? handler_(view, new_size) : kResultFalse;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<IPlugFrame*>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

private:
    ResizeHandler handler_;
};

// The host's IComponentHandler / IComponentHandler2, installed on the edit
// controller. It is how an edit made in the plug-in's own editor reaches the
// host (and, through the host, the processor), and how the plug-in asks for a
// restart when its latency or I/O changes. Lifetime belongs to the slot, which
// clears the controller's pointer before tearing down, so reference counting
// is neutered.
class ComponentHandler final : public Vst::IComponentHandler, public Vst::IComponentHandler2 {
  public:
    struct Callbacks {
        std::function<void(Vst::ParamID)> begin_edit;
        std::function<void(Vst::ParamID, Vst::ParamValue)> perform_edit;
        std::function<void(Vst::ParamID)> end_edit;
        std::function<void(int32)> restart;
        std::function<void(bool)> set_dirty;
    };

    void set_callbacks(Callbacks callbacks) {
        callbacks_ = std::move(callbacks);
    }

    tresult PLUGIN_API beginEdit(Vst::ParamID id) override {
        if (callbacks_.begin_edit)
            callbacks_.begin_edit(id);
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue normalized) override {
        if (callbacks_.perform_edit)
            callbacks_.perform_edit(id, normalized);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(Vst::ParamID id) override {
        if (callbacks_.end_edit)
            callbacks_.end_edit(id);
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        if (callbacks_.restart)
            callbacks_.restart(flags);
        return kResultOk;
    }
    tresult PLUGIN_API setDirty(TBool state) override {
        if (callbacks_.set_dirty)
            callbacks_.set_dirty(state != 0);
        return kResultOk;
    }
    // Pulp opens editors only when its host asks, and edits are not grouped.
    tresult PLUGIN_API requestOpenEditor(FIDString) override {
        return kResultFalse;
    }
    tresult PLUGIN_API startGroupEdit() override {
        return kNotImplemented;
    }
    tresult PLUGIN_API finishGroupEdit() override {
        return kNotImplemented;
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IComponentHandler*>(this);
            return kResultTrue;
        }
        if (FUnknownPrivate::iidEqual(iid, Vst::IComponentHandler2::iid)) {
            *obj = static_cast<Vst::IComponentHandler2*>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override {
        return 1;
    }
    uint32 PLUGIN_API release() override {
        return 1;
    }

  private:
    Callbacks callbacks_;
};

// Speaker arrangement for a requested channel count, or 0 to keep the plug-in's.
Vst::SpeakerArrangement arrangement_for(int channels) {
    switch (channels) {
    case 1:
        return Vst::SpeakerArr::kMono;
    case 2:
        return Vst::SpeakerArr::kStereo;
    default:
        return 0;
    }
}

class Vst3Slot final : public detail::RequestDrivenSlot {
  public:
    Vst3Slot(PluginInfo info, void* handle, IPluginFactory* factory,
             Vst::IComponent* component, Vst::IAudioProcessor* processor,
             Vst::IEditController* controller)
        : info_(std::move(info)),
          handle_(handle),
          factory_(factory),
          component_(component),
          processor_(processor),
          controller_(controller) {
        editor_frame_.set_handler([this](IPlugView* view, ViewRect* new_size) {
            return on_plugin_resize_request_(view, new_size);
        });
        connect_component_and_controller_();
        cache_params_();
        install_component_handler_();
    }

    ~Vst3Slot() override {
        // Tear the editor down before terminating the controller it came from.
        close_editor_();
        release();
        // No plug-in callback may reach the handler once the slot is going away.
        if (controller_)
            controller_->setComponentHandler(nullptr);
        // Both halves must stop holding each other before either terminates.
        disconnect_component_and_controller_();
        // Combined plugins implement IComponent + IEditController on the
        // same object — terminating both pointers would call IPluginBase
        // ::terminate() twice on it. Identity has to go through the FUnknown
        // query (see vst3_same_object), not a cast of the two pointers.
        const bool combined = (controller_ != nullptr && !is_separated_controller_());
        if (controller_ && !combined) {
            controller_->terminate();
            controller_->release();
        } else if (controller_) {
            // Combined: just drop the extra reference; component branch
            // handles terminate.
            controller_->release();
        }
        controller_ = nullptr;
        if (component_) {
            component_->terminate();
            component_->release();
            component_ = nullptr;
        }
        if (processor_) {
            processor_->release();
            processor_ = nullptr;
        }
        if (factory_) {
            factory_->release();
            factory_ = nullptr;
        }
        if (handle_) {
            // Call ModuleExit / bundleExit if present, then dlclose.
            auto* exit_fn = (void (*)())dlsym(handle_,
#if defined(__APPLE__)
                                              "bundleExit"
#else
                                              "ModuleExit"
#endif
            );
            if (exit_fn) exit_fn();
            dlclose(handle_);
            handle_ = nullptr;
        }
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return component_ && processor_; }

    void set_preferred_channel_layout(int inputs, int outputs) override {
        preferred_in_ = inputs;
        preferred_out_ = outputs;
    }

    bool prepare(double sample_rate, int max_block_size) override {
        if (!component_ || !processor_) return false;
        if (active_) release();

        configure_buses_();

        // Activate all audio + event buses the plugin exposes, so a sidechain
        // input and aux outputs take part instead of being left disabled.
        auto activate_all = [&](Vst::MediaType m, Vst::BusDirection d) {
            int32 count = component_->getBusCount(m, d);
            for (int32 i = 0; i < count; ++i) component_->activateBus(m, d, i, true);
        };
        activate_all(Vst::kAudio, Vst::kInput);
        activate_all(Vst::kAudio, Vst::kOutput);
        activate_all(Vst::kEvent, Vst::kInput);
        activate_all(Vst::kEvent, Vst::kOutput);

        const bool offline = settings().process_mode == format::ProcessMode::Offline;
        const bool use_double = settings().prefer_double_precision &&
                                processor_->canProcessSampleSize(Vst::kSample64) == kResultTrue;
        process_mode_ = offline ? Vst::kOffline : Vst::kRealtime;
        sample_size_ = use_double ? Vst::kSample64 : Vst::kSample32;

        Vst::ProcessSetup setup{};
        setup.processMode = process_mode_;
        setup.symbolicSampleSize = sample_size_;
        setup.maxSamplesPerBlock = max_block_size;
        setup.sampleRate         = sample_rate;
        if (processor_->setupProcessing(setup) != kResultOk) {
            runtime::log_warn("VST3Slot: setupProcessing failed for '{}'", info_.name);
            return false;
        }

        // Prepared buffers for every bus, every event, and every parameter the
        // audio thread may touch.
        binder_.prepare(input_decls_, output_decls_, max_block_size, use_double);
        in_buses_.assign(input_decls_.size(), Vst::AudioBusBuffers{});
        out_buses_.assign(output_decls_.size(), Vst::AudioBusBuffers{});
        constexpr int32 kMaxEvents = 2048;
        in_events_.setMaxSize(kMaxEvents);
        out_events_.setMaxSize(kMaxEvents);
        const auto param_count = (int32)std::max<std::size_t>(params_.size(), 16);
        in_param_changes_.setMaxParameters(param_count);
        out_param_changes_.setMaxParameters(param_count);
        build_midi_mapping_();

        if (component_->setActive(true) != kResultOk) {
            runtime::log_warn("VST3Slot: setActive(true) failed for '{}'", info_.name);
            return false;
        }
        processor_->setProcessing(true);
        process_clock_.prepare(sample_rate);
        sample_rate_ = sample_rate;
        max_block_size_ = max_block_size;
        active_ = true;
        set_prepared_mode(offline ? format::ProcessMode::Offline : format::ProcessMode::Realtime);
        // Reserve the pending-edit vectors so the audio-thread swap + drain
        // cycle never grows. One slot per advertised parameter is the worst
        // case when every knob moves in a single block.
        pending_host_edits_.reserve(params_.size());
        return true;
    }

    void release() override {
        if (!active_) return;
        if (processor_) processor_->setProcessing(false);
        if (component_) component_->setActive(false);
        active_ = false;
    }

    void process_request(const detail::SlotProcessRequest& request) override {
        auto& audio = *request.audio;
        const int num_samples = request.num_samples;
        if (!active_ || !processor_ || bypassed_.load(std::memory_order_relaxed)) {
            detail::SlotBusBinder::pass_through(audio, num_samples);
            if (active_ && processor_)
                process_clock_.advance(num_samples);
            return;
        }
        // A block larger than maxSamplesPerBlock breaks the setupProcessing
        // contract; refuse it instead of handing the plug-in an overrun.
        if (!binder_.bind(audio, num_samples)) {
            process_status_.store(ProcessStatus::failed, std::memory_order_release);
            detail::SlotBusBinder::pass_through(audio, num_samples);
            process_clock_.advance(num_samples);
            return;
        }
        fill_bus_buffers_();

        Vst::ProcessContext ctx{};
        if (request.transport != nullptr) {
            vst3_process_context_from(*request.transport, sample_rate_, ctx);
            ctx.state |= Vst::ProcessContext::kContTimeValid;
            ctx.continousTimeSamples = process_clock_.sample_position();
        } else {
            process_clock_.write(ctx);
        }

        build_input_events_(request);
        build_input_parameters_(request);
        out_events_.clear();
        out_param_changes_.clearQueue();

        Vst::ProcessData data{};
        data.processMode = process_mode_;
        data.symbolicSampleSize = sample_size_;
        data.numSamples = num_samples;
        data.numInputs = (int32)in_buses_.size();
        data.numOutputs = (int32)out_buses_.size();
        data.inputs = in_buses_.empty() ? nullptr : in_buses_.data();
        data.outputs = out_buses_.empty() ? nullptr : out_buses_.data();
        data.inputEvents = in_events_.getEventCount() > 0 ? &in_events_ : nullptr;
        data.outputEvents = &out_events_;
        data.inputParameterChanges =
            in_param_changes_.getParameterCount() > 0 ? &in_param_changes_ : nullptr;
        data.outputParameterChanges = &out_param_changes_;
        data.processContext = &ctx;

        if (processor_->process(data) != kResultOk) {
            process_status_.store(ProcessStatus::failed, std::memory_order_release);
            // Plugin rejected the block; fall back to input pass-through so the
            // host buffer isn't left with stale contents.
            detail::SlotBusBinder::pass_through(audio, num_samples);
        } else {
            binder_.finish(audio, num_samples);
            harvest_output_events_(*request.midi_out);
            harvest_output_parameters_();
        }
        process_clock_.advance(num_samples);
    }

    // The plug-in's declared audio buses, after asking for the caller's
    // preferred main widths (stereo when none was given). Aux buses keep the
    // plug-in's own arrangement. What the plug-in accepted is read back, so the
    // buffers always match the arrangement it will process.
    void configure_buses_() {
        const int32 n_in = component_->getBusCount(Vst::kAudio, Vst::kInput);
        const int32 n_out = component_->getBusCount(Vst::kAudio, Vst::kOutput);
        std::vector<Vst::SpeakerArrangement> ins((size_t)std::max(0, n_in), 0);
        std::vector<Vst::SpeakerArrangement> outs((size_t)std::max(0, n_out), 0);
        for (int32 i = 0; i < n_in; ++i)
            processor_->getBusArrangement(Vst::kInput, i, ins[(size_t)i]);
        for (int32 i = 0; i < n_out; ++i)
            processor_->getBusArrangement(Vst::kOutput, i, outs[(size_t)i]);
        auto want_main = [](int preferred) {
            const auto a = arrangement_for(preferred);
            return a != 0 ? a : Vst::SpeakerArr::kStereo;
        };
        if (!ins.empty())
            ins[0] = want_main(preferred_in_);
        if (!outs.empty())
            outs[0] = want_main(preferred_out_);
        // Missing buses (a generator with no inputs) degrade gracefully; a
        // refusal leaves the plug-in's own arrangement, read back below.
        processor_->setBusArrangements(ins.empty() ? nullptr : ins.data(), n_in,
                                       outs.empty() ? nullptr : outs.data(), n_out);

        auto read_back = [&](Vst::BusDirection dir, int32 count,
                             std::vector<detail::SlotBusDecl>& decls) {
            decls.clear();
            for (int32 i = 0; i < count; ++i) {
                Vst::SpeakerArrangement arr = 0;
                processor_->getBusArrangement(dir, i, arr);
                Vst::BusInfo bus{};
                const bool has_info = component_->getBusInfo(Vst::kAudio, dir, i, bus) == kResultOk;
                const bool is_main = has_info ? bus.busType == Vst::kMain : i == 0;
                int channels = Vst::SpeakerArr::getChannelCount(arr);
                if (channels == 0 && has_info)
                    channels = bus.channelCount;
                decls.push_back({channels, is_main && i == 0});
            }
            // The main bus is the first one, by VST3 convention.
            if (!decls.empty())
                decls.front().main = true;
        };
        read_back(Vst::kInput, n_in, input_decls_);
        read_back(Vst::kOutput, n_out, output_decls_);
    }

    void fill_bus_buffers_() noexcept {
        const bool dbl = binder_.use_double();
        for (std::size_t b = 0; b < in_buses_.size(); ++b) {
            auto& bus = in_buses_[b];
            bus = Vst::AudioBusBuffers{};
            bus.numChannels = (int32)binder_.input_channels(b);
            if (dbl)
                bus.channelBuffers64 = binder_.input64(b);
            else
                bus.channelBuffers32 = binder_.input32(b);
        }
        for (std::size_t b = 0; b < out_buses_.size(); ++b) {
            auto& bus = out_buses_[b];
            bus = Vst::AudioBusBuffers{};
            bus.numChannels = (int32)binder_.output_channels(b);
            if (dbl)
                bus.channelBuffers64 = binder_.output64(b);
            else
                bus.channelBuffers32 = binder_.output32(b);
        }
    }

    // Notes, polyphonic pressure, and sysex travel as VST3 events. Controllers,
    // channel pressure, pitch bend and program change have no VST3 event: they
    // become parameter changes through the plug-in's IMidiMapping, and a
    // message the plug-in did not map is dropped (VST3 defines no other path).
    void build_input_events_(const detail::SlotProcessRequest& request) noexcept {
        in_events_.clear();
        const int32 last_frame = std::max(0, request.num_samples - 1);
        auto at = [last_frame](int32 offset) { return std::clamp<int32>(offset, 0, last_frame); };
        detail::for_each_midi_in_time_order(
            *request.midi_in,
            [&](const midi::MidiEvent& me) {
                const auto m = detail::decode_short_midi(me.message.data(), me.message.length());
                using K = detail::ShortMidi::Kind;
                Vst::Event e{};
                e.busIndex = 0;
                e.sampleOffset = at(me.sample_offset);
                e.flags = Vst::Event::kIsLive;
                switch (m.kind) {
                case K::note_on:
                    e.type = Vst::Event::kNoteOnEvent;
                    e.noteOn.channel = m.channel;
                    e.noteOn.pitch = m.data1;
                    e.noteOn.velocity = m.data2 / 127.0f;
                    e.noteOn.noteId = -1;
                    in_events_.addEvent(e);
                    return;
                case K::note_off:
                    e.type = Vst::Event::kNoteOffEvent;
                    e.noteOff.channel = m.channel;
                    e.noteOff.pitch = m.data1;
                    e.noteOff.velocity = m.data2 / 127.0f;
                    e.noteOff.noteId = -1;
                    in_events_.addEvent(e);
                    return;
                case K::poly_pressure:
                    e.type = Vst::Event::kPolyPressureEvent;
                    e.polyPressure.channel = m.channel;
                    e.polyPressure.pitch = m.data1;
                    e.polyPressure.pressure = m.data2 / 127.0f;
                    e.polyPressure.noteId = -1;
                    in_events_.addEvent(e);
                    return;
                case K::control_change:
                    queue_mapped_controller_(m.channel, m.data1, m.data2 / 127.0, e.sampleOffset);
                    return;
                case K::channel_pressure:
                    queue_mapped_controller_(m.channel, Vst::kAfterTouch, m.data1 / 127.0,
                                             e.sampleOffset);
                    return;
                case K::pitch_bend:
                    queue_mapped_controller_(m.channel, Vst::kPitchBend, m.bend / 16383.0,
                                             e.sampleOffset);
                    return;
                case K::program_change: {
                    const int32 steps = program_steps_[m.channel];
                    const double value =
                        steps > 0 ? std::min(1.0, m.data1 / (double)steps) : m.data1 / 127.0;
                    queue_mapped_controller_(m.channel, Vst::kCtrlProgramChange, value,
                                             e.sampleOffset);
                    return;
                }
                default:
                    return;
                }
            },
            [&](const midi::MidiBuffer::SysexEvent& sx) {
                if (sx.data.empty())
                    return;
                Vst::Event e{};
                e.busIndex = 0;
                e.sampleOffset = at(sx.sample_offset);
                e.flags = Vst::Event::kIsLive;
                e.type = Vst::Event::kDataEvent;
                e.data.type = Vst::DataEvent::kMidiSysEx;
                e.data.size = (uint32)sx.data.size();
                e.data.bytes = sx.data.data();
                in_events_.addEvent(e);
            });
    }

    // Controller messages are queued straight into the block's parameter
    // changes; build_input_parameters_() adds automation to the same queues.
    void queue_mapped_controller_(uint8_t channel, int32 controller, Vst::ParamValue value,
                                  int32 offset) noexcept {
        if (channel >= 16 || controller < 0 || controller >= Vst::kCountCtrlNumber)
            return;
        const Vst::ParamID id = midi_map_[channel][(size_t)controller];
        if (id == Vst::kNoParamId)
            return;
        int32 index = 0;
        if (auto* q = in_param_changes_.addParameterData(id, index)) {
            int32 point = 0;
            q->addPoint(offset, std::clamp(value, 0.0, 1.0), point);
        }
    }

    // Parameter-automation input. Host set_parameter edits and editor edits
    // land at offset 0; each sample-accurate ParameterEvent becomes one point.
    // VST3 expects normalized [0..1]; Pulp's events are plain.
    void build_input_parameters_(const detail::SlotProcessRequest& request) noexcept {
        if (!controller_)
            return;
        pending_host_edits_.drain([this](uint32_t id, Vst::ParamValue normalized) {
            int32 index = 0;
            if (auto* q = in_param_changes_.addParameterData(id, index)) {
                int32 point = 0;
                q->addPoint(0, normalized, point);
            }
        });
        for (const auto& pe : *request.param_events) {
            int32 index = 0;
            auto* q = in_param_changes_.addParameterData(pe.param_id, index);
            if (!q)
                continue;
            const Vst::ParamValue norm = controller_->plainParamToNormalized(
                pe.param_id, static_cast<Vst::ParamValue>(pe.value));
            int32 point = 0;
            q->addPoint(pe.sample_offset, norm, point);
        }
    }

    // Plug-in output events back to MIDI 1.0. Sysex is copied through the
    // output buffer's own realtime payload pool when the host reserved one.
    void harvest_output_events_(midi::MidiBuffer& midi_out) noexcept {
        const int32 count = out_events_.getEventCount();
        for (int32 i = 0; i < count; ++i) {
            Vst::Event e{};
            if (out_events_.getEvent(i, e) != kResultOk)
                continue;
            auto emit = [&](uint8_t status, uint8_t d1, uint8_t d2) {
                midi::MidiEvent out;
                out.sample_offset = e.sampleOffset;
                out.message = choc::midi::ShortMessage(status, d1, d2);
                midi_out.add(out);
            };
            auto to7 = [](float v, int floor) {
                return (uint8_t)std::clamp((int)std::lround(v * 127.0f), floor, 127);
            };
            switch (e.type) {
            case Vst::Event::kNoteOnEvent:
                emit((uint8_t)(0x90 | (e.noteOn.channel & 0x0F)), (uint8_t)(e.noteOn.pitch & 0x7F),
                     to7(e.noteOn.velocity, 1));
                break;
            case Vst::Event::kNoteOffEvent:
                emit((uint8_t)(0x80 | (e.noteOff.channel & 0x0F)),
                     (uint8_t)(e.noteOff.pitch & 0x7F), to7(e.noteOff.velocity, 0));
                break;
            case Vst::Event::kPolyPressureEvent:
                emit((uint8_t)(0xA0 | (e.polyPressure.channel & 0x0F)),
                     (uint8_t)(e.polyPressure.pitch & 0x7F), to7(e.polyPressure.pressure, 0));
                break;
            case Vst::Event::kLegacyMIDICCOutEvent: {
                const auto& cc = e.midiCCOut;
                const uint8_t ch = (uint8_t)(cc.channel & 0x0F);
                const auto v1 = (uint8_t)(cc.value & 0x7F);
                const auto v2 = (uint8_t)(cc.value2 & 0x7F);
                if (cc.controlNumber < 128)
                    emit((uint8_t)(0xB0 | ch), cc.controlNumber, v1);
                else if (cc.controlNumber == Vst::kAfterTouch)
                    emit((uint8_t)(0xD0 | ch), v1, 0);
                else if (cc.controlNumber == Vst::kPitchBend)
                    emit((uint8_t)(0xE0 | ch), v1, v2);
                else if (cc.controlNumber == Vst::kCtrlProgramChange)
                    emit((uint8_t)(0xC0 | ch), v1, 0);
                else if (cc.controlNumber == Vst::kCtrlPolyPressure)
                    emit((uint8_t)(0xA0 | ch), v1, v2);
                break;
            }
            case Vst::Event::kDataEvent:
                if (e.data.type == Vst::DataEvent::kMidiSysEx && e.data.bytes && e.data.size)
                    midi_out.add_sysex_copy(e.data.bytes, e.data.size, e.sampleOffset);
                break;
            default:
                break;
            }
        }
    }

    // Values the processor reported (meters, values it changed itself). The
    // normalized-to-plain conversion is an edit-controller call, so it waits
    // for the control thread: drain_parameter_edits() converts them and also
    // forwards them to the controller, as the VST3 host contract requires.
    void harvest_output_parameters_() noexcept {
        const int32 count = out_param_changes_.getParameterCount();
        for (int32 i = 0; i < count; ++i) {
            auto* q = out_param_changes_.getParameterData(i);
            if (!q)
                continue;
            const int32 points = q->getPointCount();
            if (points <= 0)
                continue;
            int32 offset = 0;
            Vst::ParamValue value = 0.0;
            if (q->getPoint(points - 1, offset, value) != kResultOk)
                continue;
            if (!processor_outputs_.try_push({q->getParameterId(), value}))
                dropped_processor_outputs_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::size_t drain_parameter_edits(std::span<PluginParameterEdit> out) override {
        if (controller_) {
            ProcessorOutput value{};
            while (processor_outputs_.try_pop(value)) {
                controller_->setParamNormalized(value.id, value.normalized);
                edit_inbox().push_from_control(
                    {PluginParameterEdit::Kind::value, value.id,
                     (float)controller_->normalizedParamToPlain(value.id, value.normalized)});
            }
        }
        return RequestDrivenSlot::drain_parameter_edits(out);
    }

    // IMidiMapping, read once at prepare (the controller is not an audio-thread
    // object). Rebuilt at the next prepare after the plug-in reports
    // kMidiCCAssignmentChanged.
    void build_midi_mapping_() {
        for (auto& row : midi_map_)
            row.fill(Vst::kNoParamId);
        program_steps_.fill(0);
        if (!controller_)
            return;
        Vst::IMidiMapping* mapping = nullptr;
        if (controller_->queryInterface(Vst::IMidiMapping::iid, (void**)&mapping) != kResultOk ||
            !mapping)
            return;
        for (int16 ch = 0; ch < 16; ++ch) {
            for (int32 ctrl = 0; ctrl < Vst::kCountCtrlNumber; ++ctrl) {
                Vst::ParamID id = Vst::kNoParamId;
                if (mapping->getMidiControllerAssignment(0, ch, (Vst::CtrlNumber)ctrl, id) ==
                    kResultTrue)
                    midi_map_[(size_t)ch][(size_t)ctrl] = id;
            }
            const Vst::ParamID program = midi_map_[(size_t)ch][Vst::kCtrlProgramChange];
            if (program != Vst::kNoParamId) {
                const int32 count = controller_->getParameterCount();
                for (int32 i = 0; i < count; ++i) {
                    Vst::ParameterInfo pi{};
                    if (controller_->getParameterInfo(i, pi) == kResultOk && pi.id == program) {
                        program_steps_[(size_t)ch] = pi.stepCount;
                        break;
                    }
                }
            }
        }
        mapping->release();
    }

    // Wire the editor's edits and the plug-in's restart requests back to the
    // host. An edit is delivered to the processor at the next block and to the
    // host through drain_parameter_edits().
    void install_component_handler_() {
        if (!controller_)
            return;
        ComponentHandler::Callbacks callbacks;
        callbacks.begin_edit = [this](Vst::ParamID id) {
            push_editor_edit_({PluginParameterEdit::Kind::begin_gesture, id, 0.0f});
        };
        callbacks.perform_edit = [this](Vst::ParamID id, Vst::ParamValue normalized) {
            // performEdit is a UI-thread call. Queue it for the processor, and
            // report the plain value the controller maps it to.
            if (detail::AudioThreadScope::active()) {
                // Out of contract (a processor-side call), but the processor
                // already holds the value: report it like a processor output.
                if (!processor_outputs_.try_push({id, normalized}))
                    dropped_processor_outputs_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            pending_host_edits_.set(id, normalized);
            push_editor_edit_({PluginParameterEdit::Kind::value, id,
                               (float)controller_->normalizedParamToPlain(id, normalized)});
        };
        callbacks.end_edit = [this](Vst::ParamID id) {
            push_editor_edit_({PluginParameterEdit::Kind::end_gesture, id, 0.0f});
        };
        callbacks.restart = [this](int32 flags) { on_restart_component_(flags); };
        callbacks.set_dirty = [this](bool dirty) {
            if (dirty)
                note_state_change();
        };
        component_handler_.set_callbacks(std::move(callbacks));
        controller_->setComponentHandler(&component_handler_);
    }

    void push_editor_edit_(const PluginParameterEdit& edit) {
        if (detail::AudioThreadScope::active()) {
            edit_inbox().push_from_audio(edit);
            return;
        }
        try {
            edit_inbox().push_from_control(edit);
        } catch (...) {
        }
    }

    // restartComponent is a UI-thread call. Anything that needs the component
    // deactivated (latency, I/O, a reload, a new MIDI mapping) is a restart
    // request the host honors by re-preparing; parameter metadata is re-read
    // in place.
    void on_restart_component_(int32 flags) {
        constexpr int32 kNeedsReprepare = Vst::kLatencyChanged | Vst::kIoChanged |
                                          Vst::kReloadComponent | Vst::kMidiCCAssignmentChanged;
        if (flags & kNeedsReprepare)
            note_restart_request();
        if (flags & (Vst::kParamTitlesChanged | Vst::kParamValuesChanged)) {
            if (!detail::AudioThreadScope::active())
                cache_params_();
        }
    }

    void begin_process() noexcept override {
        process_status_.store(ProcessStatus::ok, std::memory_order_release);
    }

    ProcessStatus process_status() const noexcept override {
        return process_status_.load(std::memory_order_acquire);
    }

    std::vector<HostParamInfo> parameters() const override { return params_; }

    float get_parameter(uint32_t id) const override {
        if (!controller_) return 0.0f;
        // VST3 works in normalized [0..1]; convert to plain using the
        // controller's conversion.
        Vst::ParamValue norm = controller_->getParamNormalized(id);
        return (float)controller_->normalizedParamToPlain(id, norm);
    }

    void set_parameter(uint32_t id, float plain_value) override {
        if (!controller_) return;
        // Host-originated edits must reach the processor. Mirror into the
        // controller for UI/state reads and queue a normalized point-at-time-0
        // edit for the next process() block to deliver via IParameterChanges.
        Vst::ParamValue norm =
            controller_->plainParamToNormalized(id, plain_value);
        controller_->setParamNormalized(id, norm);

        // Coalesced on the host side so the audio-thread drain stays
        // allocation-free; see detail::PendingHostEdits.
        pending_host_edits_.set(id, norm);
    }

    void set_bypass(bool bypassed) override {
        bypassed_.store(bypassed, std::memory_order_relaxed);
    }
    bool is_bypassed() const override {
        return bypassed_.load(std::memory_order_relaxed);
    }

    // VST3 and CLAP both carry bypass AS a parameter, so the flagged
    // `parameters()` entry is the whole answer and there is nothing else to
    // probe.
    BypassSurface bypass_surface() const override {
        for (const auto& p : params_) {
            if (p.flags.is_bypass) return BypassSurface::parameter;
        }
        return BypassSurface::none;
    }

    // True when the plugin exposes a *separate* edit controller (a distinct
    // object from the component). Teardown, state serialization, and
    // connection-point wiring all key off this.
    bool is_separated_controller_() const {
        return detail::vst3_is_separated(component_, controller_);
    }

    // Detach + release the editor view and destroy its container. Idempotent;
    // safe to call from destroy_hosted_editor() and from the destructor.
    void close_editor_() {
        if (editor_view_) {
            detail::vst3_detach_editor_view(editor_view_);
            detail::vst3_release_editor_view(editor_view_);
            editor_view_ = nullptr;
        }
        if (editor_container_) {
            destroy_editor_container(editor_container_);
            editor_container_ = nullptr;
        }
        editor_width_ = 0;
        editor_height_ = 0;
    }

    // Join a separated plug-in's two halves so its editor can talk to its own
    // processor, then hand the controller the state it would otherwise never
    // see. Both are no-ops for a combined plug-in.
    void connect_component_and_controller_() {
        connection_ = detail::vst3_connect_component_controller(component_, controller_);
        switch (connection_.result) {
            case detail::Vst3ConnectResult::ComponentRefused:
                runtime::log_warn(
                    "VST3 load: component refused its controller connection for '{}'",
                    info_.name);
                break;
            case detail::Vst3ConnectResult::ControllerRefused:
                runtime::log_warn(
                    "VST3 load: controller refused its component connection for '{}'",
                    info_.name);
                break;
            default:
                break;
        }
        if (is_separated_controller_()) {
            detail::vst3_push_component_state(component_, controller_);
        }
    }

    void disconnect_component_and_controller_() {
        detail::vst3_disconnect_component_controller(connection_);
    }

    // Plug-in-driven resize, reached through EditorFrame. IPlugView documents
    // the sequence: the plug-in calls resizeView(newSize), the host resizes the
    // platform representation, then — in the same callstack — calls onSize().
    // The container is host-owned so resizing it is always safe; an installed
    // resize-request handler is the embedder's veto over how much room the
    // editor may claim inside its window.
    tresult on_plugin_resize_request_(IPlugView* view, ViewRect* new_size) {
        if (!view || !new_size) return kInvalidArgument;
        // A view this slot does not have open cannot be resized through it —
        // including a view still being created, which has no container yet.
        if (view != editor_view_ || !editor_container_) return kResultFalse;
        const int32 w = new_size->right - new_size->left;
        const int32 h = new_size->bottom - new_size->top;
        if (w <= 0 || h <= 0) return kInvalidArgument;
        if (resize_request_handler_
            && !resize_request_handler_((uint32_t)w, (uint32_t)h)) {
            return kResultFalse;
        }
        resize_editor_container(editor_container_, (uint32_t)w, (uint32_t)h);
        editor_width_ = (uint32_t)w;
        editor_height_ = (uint32_t)h;
        if (!detail::vst3_commit_requested_size(view, *new_size)) {
            runtime::log_warn(
                "VST3 editor: '{}' rejected onSize for the size it just requested",
                info_.name);
            return kResultFalse;
        }
        return kResultTrue;
    }

    std::vector<uint8_t> save_state() const override {
        if (!component_) return {};
        return detail::vst3_serialize_state(component_, controller_,
                                            is_separated_controller_());
    }

    bool restore_state(const std::vector<uint8_t>& data) override {
        if (!component_ || data.empty()) return false;
        return detail::vst3_restore_state(data, component_, controller_,
                                          is_separated_controller_());
    }

    bool has_editor() const override { return controller_ != nullptr; }
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    // The legacy void* path cannot express VST3's parent-consuming
    // IPlugView::attached() model — it has nowhere to receive the parent
    // window. Hosts reach the VST3 editor through create_hosted_editor();
    // these satisfy the still-pure-virtual legacy interface only.
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

    std::unique_ptr<HostedEditor> create_hosted_editor(void* parent_window) override {
        if (!controller_ || !parent_window) return nullptr;
        if (editor_view_) {
            runtime::log_error(
                "VST3 editor: create requested while one is already open for '{}'",
                info_.name);
            return nullptr;
        }

        uint32_t width = 0;
        uint32_t height = 0;
        bool resizable = false;
        IPlugView* view = detail::vst3_create_editor_view(
            controller_, &editor_frame_, &width, &height, &resizable);
        if (!view) return nullptr;

        // Order mirrors the container contract (hosted_editor_container.hpp):
        // size the container from the view, create it already inserted into the
        // parent window, then let the plug-in attach into it.
        void* container = create_editor_container(parent_window, width, height);
        if (!container) {
            detail::vst3_release_editor_view(view);
            return nullptr;
        }

        // Published BEFORE attached(): IPlugView documents resizeView() as
        // callable from inside attached(), and the frame can only honor it once
        // the view and its container are reachable from the slot.
        editor_view_ = view;
        editor_container_ = container;
        editor_width_ = width;
        editor_height_ = height;

        if (!detail::vst3_attach_editor_view(view, container)) {
            runtime::log_error("VST3 editor: attached() failed for '{}'", info_.name);
            editor_view_ = nullptr;
            editor_container_ = nullptr;
            editor_width_ = 0;
            editor_height_ = 0;
            destroy_editor_container(container);
            detail::vst3_release_editor_view(view);
            return nullptr;
        }

        auto ed = std::make_unique<HostedEditor>();
        ed->native_handle = container;
        // attached() may have driven a resizeView, so report what the editor
        // ended up at rather than the size queried before it existed.
        ed->width = editor_width_;
        ed->height = editor_height_;
        ed->resizable = resizable;
        return ed;
    }

    void set_editor_resize_request_handler(EditorResizeRequestHandler handler) override {
        resize_request_handler_ = std::move(handler);
    }

    void destroy_hosted_editor(std::unique_ptr<HostedEditor> ed) override {
        if (!ed) return;
        if (ed->native_handle != editor_container_) {
            runtime::log_error(
                "VST3 editor: destroy_hosted_editor for '{}' got an editor this "
                "slot does not own; ignoring",
                info_.name);
            return;
        }
        close_editor_();
    }

    bool set_hosted_editor_size(uint32_t& width, uint32_t& height) override {
        if (!editor_view_) return false;
        if (!detail::vst3_resize_editor_view(editor_view_, &width, &height)) return false;
        resize_editor_container(editor_container_, width, height);
        editor_width_ = width;
        editor_height_ = height;
        return true;
    }

    int latency_samples() const override {
        if (!processor_) return 0;
        return (int)processor_->getLatencySamples();
    }
    int tail_samples() const override {
        if (!processor_) return 0;
        return (int)processor_->getTailSamples();
    }

    // Typed plugin introspection surfaces the IComponent and related VST3
    // interfaces so callers that link against the SDK can reach into vendor
    // extensions without round-tripping through `void*`.
    void accept(NativeHandleVisitor& visitor) const override {
        Vst3NativeHandle ext;
        ext.component = component_;
        ext.audio_processor = processor_;
        ext.edit_controller = controller_;
        ext.class_id = info_.unique_id;
        visitor.visit_vst3(*this, ext);
    }

    static HostApp& host_app() {
        static HostApp app;
        return app;
    }

    void cache_params_() {
        params_.clear();
        if (!controller_) return;
        const int32 count = controller_->getParameterCount();
        params_.reserve((size_t)count);
        for (int32 i = 0; i < count; ++i) {
            Vst::ParameterInfo pi{};
            if (controller_->getParameterInfo(i, pi) != kResultOk) continue;
            HostParamInfo h;
            h.id = (uint32_t)pi.id;
            for (int c = 0; c < 127 && pi.title[c]; ++c) h.name.push_back((char)pi.title[c]);
            for (int c = 0; c < 127 && pi.units[c]; ++c) h.unit.push_back((char)pi.units[c]);
            // VST3 reports stepCount > 0 for stepped; 0 = continuous.
            h.flags.stepped = (pi.stepCount > 0);
            h.flags.rampable = !h.flags.stepped;
            h.flags.automatable = (pi.flags & Vst::ParameterInfo::kCanAutomate) != 0;
            h.flags.read_only   = (pi.flags & Vst::ParameterInfo::kIsReadOnly) != 0;
            h.flags.hidden      = (pi.flags & Vst::ParameterInfo::kIsHidden) != 0;
            h.flags.is_bypass   = (pi.flags & Vst::ParameterInfo::kIsBypass) != 0;
            h.flags.modulatable = false;  // VST3 has no per-voice mod primitive
            // Plain param range: normalize 0/1 via controller.
            h.min_value = (float)controller_->normalizedParamToPlain(pi.id, 0.0);
            h.max_value = (float)controller_->normalizedParamToPlain(pi.id, 1.0);
            h.default_value = (float)controller_->normalizedParamToPlain(pi.id, pi.defaultNormalizedValue);
            params_.push_back(std::move(h));
        }
    }

  private:
    std::atomic<ProcessStatus> process_status_{ProcessStatus::ok};
    PluginInfo info_;
    void* handle_ = nullptr;
    IPluginFactory* factory_ = nullptr;
    // Hosted editor (embedded IPlugView + its host container), null unless open.
    IPlugView* editor_view_ = nullptr;
    void* editor_container_ = nullptr;
    // Live editor size. Tracks plug-in-driven resizes, which the size queried
    // before attached() does not.
    uint32_t editor_width_ = 0;
    uint32_t editor_height_ = 0;
    EditorFrame editor_frame_;
    EditorResizeRequestHandler resize_request_handler_;

    Vst::IComponent* component_ = nullptr;
    Vst::IAudioProcessor* processor_ = nullptr;
    Vst::IEditController* controller_ = nullptr;
    // Separated-model connection points. Empty for a combined plug-in or one
    // that exposes no connection point.
    detail::Vst3Connection connection_;
    std::vector<HostParamInfo> params_;
    // Host set_parameter edits and editor performEdit values (normalized),
    // delivered to the processor at the next block.
    detail::PendingHostEdits<Vst::ParamValue> pending_host_edits_;
    ComponentHandler component_handler_;

    // Audio routing: the plug-in's buses as negotiated at prepare.
    std::vector<detail::SlotBusDecl> input_decls_;
    std::vector<detail::SlotBusDecl> output_decls_;
    detail::SlotBusBinder binder_;
    std::vector<Vst::AudioBusBuffers> in_buses_;
    std::vector<Vst::AudioBusBuffers> out_buses_;
    int preferred_in_ = -1;
    int preferred_out_ = -1;

    // Pre-sized event/parameter buffers (setMaxSize / setMaxParameters at
    // prepare) so the process callback does not heap-allocate.
    Vst::EventList in_events_;
    Vst::EventList out_events_;
    Vst::ParameterChanges in_param_changes_;
    Vst::ParameterChanges out_param_changes_;

    // Processor-reported parameter values, waiting for the control thread.
    struct ProcessorOutput {
        Vst::ParamID id = 0;
        Vst::ParamValue normalized = 0.0;
    };
    runtime::SpscQueue<ProcessorOutput, 1024> processor_outputs_;
    std::atomic<std::uint64_t> dropped_processor_outputs_{0};

    // IMidiMapping table: [channel][controller] -> parameter, kNoParamId when
    // unmapped. Program-change step counts per channel turn a program number
    // into the normalized value of a list parameter.
    std::array<std::array<Vst::ParamID, Vst::kCountCtrlNumber>, 16> midi_map_{};
    std::array<int32, 16> program_steps_{};

    std::atomic<bool> bypassed_{false};
    Vst3HostProcessClock process_clock_;
    Vst::ProcessModes process_mode_ = Vst::kRealtime;
    Vst::SymbolicSampleSizes sample_size_ = Vst::kSample32;
    double sample_rate_ = 44100.0;
    int max_block_size_ = 0;
    bool active_ = false;
};

} // namespace

Vst::IHostApplication* vst3_host_application() {
    return &Vst3Slot::host_app();
}

void vst3_process_context_from(const format::ProcessContext& context, double sample_rate,
                               Vst::ProcessContext& out) noexcept {
    using F = format::TransportField;
    using PC = Vst::ProcessContext;
    out = PC{};
    auto has = [&](F field) { return detail::transport_has(context, field); };
    out.sampleRate = sample_rate > 0.0 ? sample_rate : context.sample_rate;

    uint32 state = 0;
    if (has(F::Playing) && context.is_playing)
        state |= PC::kPlaying;
    if (has(F::Recording) && context.is_recording)
        state |= PC::kRecording;
    if (has(F::Looping) && context.is_looping)
        state |= PC::kCycleActive;
    if (has(F::SamplePosition))
        out.projectTimeSamples = context.position_samples;
    if (has(F::Tempo) && context.tempo_bpm > 0.0 && std::isfinite(context.tempo_bpm)) {
        state |= PC::kTempoValid;
        out.tempo = context.tempo_bpm;
    }
    const bool beats = has(F::BeatPosition) && std::isfinite(context.position_beats);
    if (beats) {
        state |= PC::kProjectTimeMusicValid;
        out.projectTimeMusic = context.position_beats;
    }
    const bool tsig =
        has(F::TimeSignature) && context.time_sig_numerator > 0 && context.time_sig_denominator > 0;
    if (tsig) {
        state |= PC::kTimeSigValid;
        out.timeSigNumerator = context.time_sig_numerator;
        out.timeSigDenominator = context.time_sig_denominator;
    }
    if (beats && tsig) {
        // Start of the current bar, in quarter notes.
        const double bar_beats =
            4.0 * context.time_sig_numerator / (double)context.time_sig_denominator;
        const double bar =
            has(F::Bar) ? (double)context.bar : std::floor(context.position_beats / bar_beats);
        state |= PC::kBarPositionValid;
        out.barPositionMusic = bar * bar_beats;
    }
    if (detail::transport_has_loop_range(context)) {
        state |= PC::kCycleValid;
        out.cycleStartMusic = context.loop_start_beats;
        out.cycleEndMusic = context.loop_end_beats;
    }
    if (has(F::HostTime) && context.host_time_ns != 0) {
        state |= PC::kSystemTimeValid;
        out.systemTime = context.host_time_ns;
    }
    if (has(F::FrameRate) && context.frame_rate != format::FrameRate::unknown) {
        using R = format::FrameRate;
        uint32 fps = 0;
        uint32 flags = 0;
        switch (context.frame_rate) {
        case R::fps_24:
            fps = 24;
            break;
        case R::fps_25:
            fps = 25;
            break;
        case R::fps_29_97:
            fps = 30;
            flags = Vst::FrameRate::kPullDownRate;
            break;
        case R::fps_29_97_drop:
            fps = 30;
            flags = Vst::FrameRate::kPullDownRate | Vst::FrameRate::kDropRate;
            break;
        case R::fps_30:
            fps = 30;
            break;
        case R::fps_30_drop:
            fps = 30;
            flags = Vst::FrameRate::kDropRate;
            break;
        case R::fps_60:
            fps = 60;
            break;
        default:
            break;
        }
        if (fps != 0) {
            state |= PC::kSmpteValid;
            out.frameRate.framesPerSecond = fps;
            out.frameRate.flags = flags;
        }
    }
    out.state = state;
}

std::unique_ptr<PluginSlot> load_vst3_plugin(const PluginInfo& info) {
    std::string bin = resolve_vst3_binary(info.path);
    void* handle = dlopen(bin.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        const char* err = dlerror();
        runtime::log_error("VST3 load: dlopen failed for '{}': {}", bin, err ? err : "unknown");
        return nullptr;
    }

    // Bundle/module entry (optional).
#if defined(__APPLE__)
    auto* entry_fn = (bool (*)(void*))dlsym(handle, "bundleEntry");
    if (entry_fn) {
        // Without a real CFBundleRef, pass nullptr; most plugins tolerate
        // this, though a few require a proper CFBundle. Plugins that require
        // CFBundle-backed initialization are not supported by this loader yet.
        entry_fn(nullptr);
    }
#else
    auto* entry_fn = (bool (*)())dlsym(handle, "ModuleEntry");
    if (entry_fn) entry_fn();
#endif

    using GetPluginFactoryProc = IPluginFactory* (PLUGIN_API*)();
    auto* get_factory =
        reinterpret_cast<GetPluginFactoryProc>(dlsym(handle, "GetPluginFactory"));
    if (!get_factory) {
        runtime::log_error("VST3 load: GetPluginFactory missing in '{}'", bin);
        dlclose(handle);
        return nullptr;
    }

    IPluginFactory* factory = get_factory();
    if (!factory) {
        runtime::log_error("VST3 load: GetPluginFactory returned null for '{}'", bin);
        dlclose(handle);
        return nullptr;
    }

    // Find the first Audio Module Class ("Audio Module Class").
    int32 class_count = factory->countClasses();
    PClassInfo chosen{};
    bool found = false;
    for (int32 i = 0; i < class_count; ++i) {
        PClassInfo ci{};
        if (factory->getClassInfo(i, &ci) != kResultOk) continue;
        if (std::strcmp(ci.category, kVstAudioEffectClass) == 0) {
            chosen = ci;
            found = true;
            break;
        }
    }
    if (!found) {
        runtime::log_error("VST3 load: no audio module class in '{}'", bin);
        factory->release();
        dlclose(handle);
        return nullptr;
    }

    Vst::IComponent* component = nullptr;
    if (factory->createInstance(chosen.cid, Vst::IComponent::iid, (void**)&component) != kResultOk
            || !component) {
        runtime::log_error("VST3 load: createInstance failed for '{}'", chosen.name);
        factory->release();
        dlclose(handle);
        return nullptr;
    }

    // initialize with a minimal host context so the plugin can query services.
    if (component->initialize(&Vst3Slot::host_app()) != kResultOk) {
        runtime::log_error("VST3 load: IComponent::initialize failed for '{}'", chosen.name);
        component->release();
        factory->release();
        dlclose(handle);
        return nullptr;
    }

    Vst::IAudioProcessor* processor = nullptr;
    if (component->queryInterface(Vst::IAudioProcessor::iid, (void**)&processor) != kResultOk
            || !processor) {
        runtime::log_error("VST3 load: no IAudioProcessor on '{}'", chosen.name);
        component->terminate();
        component->release();
        factory->release();
        dlclose(handle);
        return nullptr;
    }

    // Try to get IEditController. Preferred path: plugin implements both
    // IComponent and IEditController on the same object (combined). Fallback
    // path for plugins that separate component and controller: query the
    // component for its controller class id and factory-create it. Vst3Slot's
    // constructor then joins the two halves over IConnectionPoint.
    Vst::IEditController* controller = nullptr;
    if (component->queryInterface(Vst::IEditController::iid, (void**)&controller) != kResultOk) {
        controller = nullptr;
    }
    if (!controller) {
        TUID controller_cid;
        if (component->getControllerClassId(controller_cid) == kResultOk) {
            factory->createInstance(controller_cid, Vst::IEditController::iid,
                                    (void**)&controller);
            if (controller && controller->initialize(&Vst3Slot::host_app()) != kResultOk) {
                controller->release();
                controller = nullptr;
            }
        }
    }

    PluginInfo filled = info;
    if (filled.name.empty())         filled.name = chosen.name;
    if (filled.manufacturer.empty()) filled.manufacturer = ""; // PClassInfo2 carries vendor
    if (filled.unique_id.empty())    filled.unique_id = chosen.name;

    return std::make_unique<Vst3Slot>(std::move(filled), handle, factory,
                                      component, processor, controller);
}

std::unique_ptr<PluginSlot> make_vst3_slot(PluginInfo info,
                                           Vst::IComponent* component,
                                           Vst::IAudioProcessor* processor,
                                           Vst::IEditController* controller) {
    if (!component) return nullptr;
    // No module handle and no factory: nothing to dlclose, nothing to release
    // beyond the interfaces the caller handed over.
    return std::make_unique<Vst3Slot>(std::move(info), /*handle=*/nullptr,
                                      /*factory=*/nullptr, component, processor,
                                      controller);
}

} // namespace pulp::host
