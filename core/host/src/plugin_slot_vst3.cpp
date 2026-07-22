// VST3 plugin slot implementation.
//
// Loads a .vst3 bundle, resolves GetPluginFactory, creates the first audio
// processor class, and wires the PluginSlot interface onto IComponent +
// IAudioProcessor. The host covers audio pass-through with stereo 2-in/2-out,
// latency reporting, bypass, parameter edits, MIDI note events, state
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

#include <pulp/host/dl_shim.hpp>
#include <pulp/host/detail/vst3_connection.hpp>
#include <pulp/host/detail/vst3_state_sync.hpp>
#include <pulp/host/detail/vst3_editor.hpp>
#include <pulp/host/hosted_editor_container.hpp>
#include <atomic>
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
        static const char kName[] = "Pulp";
        int i = 0;
        for (; i < 127 && kName[i] != '\0'; ++i) name[i] = (Vst::TChar)kName[i];
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

class Vst3Slot final : public PluginSlot {
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
    }

    ~Vst3Slot() override {
        // Tear the editor down before terminating the controller it came from.
        close_editor_();
        release();
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

    bool prepare(double sample_rate, int max_block_size) override {
        if (!component_ || !processor_) return false;
        if (active_) release();

        // Configure stereo 2-in/2-out. Many plugins need setBusArrangements
        // before setActive. Missing buses (e.g. generators with 0 inputs)
        // degrade gracefully.
        Vst::SpeakerArrangement arr_in  = Vst::SpeakerArr::kStereo;
        Vst::SpeakerArrangement arr_out = Vst::SpeakerArr::kStereo;
        processor_->setBusArrangements(&arr_in, 1, &arr_out, 1);

        // Activate all audio + event buses the plugin exposes.
        auto activate_all = [&](Vst::MediaType m, Vst::BusDirection d) {
            int32 count = component_->getBusCount(m, d);
            for (int32 i = 0; i < count; ++i) component_->activateBus(m, d, i, true);
        };
        activate_all(Vst::kAudio, Vst::kInput);
        activate_all(Vst::kAudio, Vst::kOutput);
        activate_all(Vst::kEvent, Vst::kInput);
        activate_all(Vst::kEvent, Vst::kOutput);

        Vst::ProcessSetup setup{};
        setup.processMode       = Vst::kRealtime;
        setup.symbolicSampleSize = Vst::kSample32;
        setup.maxSamplesPerBlock = max_block_size;
        setup.sampleRate         = sample_rate;
        if (processor_->setupProcessing(setup) != kResultOk) {
            runtime::log_warn("VST3Slot: setupProcessing failed for '{}'", info_.name);
            return false;
        }
        if (component_->setActive(true) != kResultOk) {
            runtime::log_warn("VST3Slot: setActive(true) failed for '{}'", info_.name);
            return false;
        }
        processor_->setProcessing(true);
        max_block_size_ = max_block_size;
        active_ = true;
        // Reserve both pending-edit vectors so the audio-thread swap + drain
        // cycle never hits a grow-and-copy allocation. One slot per
        // advertised parameter is the worst case when every knob moves in a
        // single block.
        {
            std::lock_guard<std::mutex> lock(pending_host_edits_mu_);
            pending_host_edits_.reserve(params_.size());
            drain_scratch_.reserve(params_.size());
        }
        return true;
    }

    void release() override {
        if (!active_) return;
        if (processor_) processor_->setProcessing(false);
        if (component_) component_->setActive(false);
        active_ = false;
    }

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& /*midi_out*/,
                 const ParameterEventQueue& param_events,
                 int num_samples) override {
        if (!active_ || !processor_ || bypassed_.load(std::memory_order_relaxed)) {
            for (size_t c = 0; c < output.num_channels(); ++c) {
                auto* dst = output.channel_ptr(c);
                if (c < input.num_channels()) {
                    std::memcpy(dst, input.channel_ptr(c), sizeof(float) * (size_t)num_samples);
                } else {
                    std::memset(dst, 0, sizeof(float) * (size_t)num_samples);
                }
            }
            return;
        }

        const int32 nch_in  = (int32)input.num_channels();
        const int32 nch_out = (int32)output.num_channels();

        in_ptrs_.resize((size_t)nch_in);
        for (int32 c = 0; c < nch_in; ++c) {
            in_ptrs_[(size_t)c] = const_cast<float*>(input.channel_ptr((size_t)c));
        }
        out_ptrs_.resize((size_t)nch_out);
        for (int32 c = 0; c < nch_out; ++c) {
            out_ptrs_[(size_t)c] = output.channel_ptr((size_t)c);
        }

        Vst::AudioBusBuffers in_bus{};
        in_bus.numChannels      = nch_in;
        in_bus.silenceFlags     = 0;
        in_bus.channelBuffers32 = in_ptrs_.data();
        Vst::AudioBusBuffers out_bus{};
        out_bus.numChannels      = nch_out;
        out_bus.silenceFlags     = 0;
        out_bus.channelBuffers32 = out_ptrs_.data();

        Vst::ProcessContext ctx{};
        ctx.sampleRate = sample_rate_;
        ctx.state      = Vst::ProcessContext::kPlaying;

        // Build the VST3 event list from Pulp's MidiBuffer. The current host
        // queue carries short MIDI messages, so this maps note_on/note_off
        // and leaves richer event types to the queue layer that can represent
        // them explicitly.
        in_events_.clear();
        for (auto it = midi_in.begin(); it != midi_in.end(); ++it) {
            const auto& me = *it;
            const auto& m = me.message;
            if (m.length() < 3) continue;
            uint8_t status = m.data()[0] & 0xF0;
            uint8_t ch     = m.data()[0] & 0x0F;
            if (status == 0x90 && m.data()[2] > 0) {
                Vst::Event e{};
                e.busIndex      = 0;
                e.sampleOffset  = me.sample_offset;
                e.flags         = Vst::Event::kIsLive;
                e.type          = Vst::Event::kNoteOnEvent;
                e.noteOn.channel  = ch;
                e.noteOn.pitch    = static_cast<int16>(m.data()[1]);
                e.noteOn.velocity = static_cast<float>(m.data()[2]) / 127.0f;
                e.noteOn.noteId   = -1;
                in_events_.addEvent(e);
            } else if (status == 0x80 ||
                       (status == 0x90 && m.data()[2] == 0)) {
                Vst::Event e{};
                e.busIndex      = 0;
                e.sampleOffset  = me.sample_offset;
                e.flags         = Vst::Event::kIsLive;
                e.type          = Vst::Event::kNoteOffEvent;
                e.noteOff.channel  = ch;
                e.noteOff.pitch    = static_cast<int16>(m.data()[1]);
                e.noteOff.velocity = static_cast<float>(m.data()[2]) / 127.0f;
                e.noteOff.noteId   = -1;
                in_events_.addEvent(e);
            }
        }

        // Build parameter-automation input. Each Pulp ParameterEvent
        // becomes one queue-point at its sample_offset. Values are already
        // in plain domain on our side; VST3 expects normalized [0..1], so
        // we round-trip via controller_->plainParamToNormalized().
        in_param_changes_.clearQueue();

        // Drain host-originated set_parameter edits that arrived since the
        // previous block. Delivered as time=0 points. try_lock keeps the
        // audio thread from blocking on the host side; if contended, edits
        // ride to the next process() call.
        //
        // The two-buffer swap keeps this RT-safe. The host thread writes
        // into pending_host_edits_ (a std::vector<PendingEdit> with reserved
        // capacity). The audio thread, under try_lock, std::swaps it with
        // drain_scratch_ (also reserved) in O(1) -- just swaps the internal
        // pointers, no allocation. Lock goes out of scope; the audio thread
        // iterates drain_scratch_ outside the lock to push parameter points,
        // then calls drain_scratch_.clear(), which for std::vector is element
        // destruction only. Vector capacity is preserved by the standard, and
        // ParamValue plus uint32_t are trivially destructible, so clear()
        // compiles to "set size = 0". RT-safe by construction.
        if (controller_) {
            std::unique_lock<std::mutex> lock(pending_host_edits_mu_,
                                              std::try_to_lock);
            if (lock.owns_lock() && !pending_host_edits_.empty()) {
                std::swap(pending_host_edits_, drain_scratch_);
            }
        }
        if (controller_ && !drain_scratch_.empty()) {
            for (const auto& e : drain_scratch_) {
                Steinberg::int32 idx = 0;
                auto* q = in_param_changes_.addParameterData(e.id, idx);
                if (!q) continue;
                Steinberg::int32 point = 0;
                q->addPoint(0, e.normalized, point);
            }
            drain_scratch_.clear();  // vector::clear preserves capacity
        }

        if (controller_ && !param_events.empty()) {
            for (const auto& pe : param_events) {
                Steinberg::int32 idx = 0;
                auto* q = in_param_changes_.addParameterData(pe.param_id, idx);
                if (!q) continue;
                Vst::ParamValue norm = controller_->plainParamToNormalized(
                    pe.param_id, static_cast<Vst::ParamValue>(pe.value));
                Steinberg::int32 point = 0;
                q->addPoint(pe.sample_offset, norm, point);
            }
        }

        Vst::ProcessData data{};
        data.processMode        = Vst::kRealtime;
        data.symbolicSampleSize = Vst::kSample32;
        data.numSamples         = num_samples;
        data.numInputs          = nch_in  ? 1 : 0;
        data.numOutputs         = nch_out ? 1 : 0;
        data.inputs             = nch_in  ? &in_bus  : nullptr;
        data.outputs            = nch_out ? &out_bus : nullptr;
        data.inputEvents        = in_events_.getEventCount() > 0 ? &in_events_ : nullptr;
        data.inputParameterChanges =
            in_param_changes_.getParameterCount() > 0 ? &in_param_changes_ : nullptr;
        data.processContext     = &ctx;

        if (processor_->process(data) != kResultOk) {
            // Plugin rejected the block; fall back to input pass-through so the
            // host buffer isn't left with stale contents.
            for (size_t c = 0; c < output.num_channels(); ++c) {
                auto* dst = output.channel_ptr(c);
                if (c < input.num_channels()) {
                    std::memcpy(dst, input.channel_ptr(c), sizeof(float) * (size_t)num_samples);
                } else {
                    std::memset(dst, 0, sizeof(float) * (size_t)num_samples);
                }
            }
        }
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

        // Pending edits live in a reserved-capacity std::vector. Coalesce on
        // the host side (linear scan) so the audio-thread drain stays
        // allocation-free. set_parameter is user-gesture rate, not sample
        // rate, so linear coalesce is cheap; the RT-safety win on the audio
        // thread is worth it.
        std::lock_guard<std::mutex> lock(pending_host_edits_mu_);
        for (auto& e : pending_host_edits_) {
            if (e.id == id) { e.normalized = norm; return; }
        }
        pending_host_edits_.push_back({id, norm});
    }

    void set_bypass(bool bypassed) override {
        bypassed_.store(bypassed, std::memory_order_relaxed);
    }
    bool is_bypassed() const override {
        return bypassed_.load(std::memory_order_relaxed);
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
    // Host set_parameter edits. Vector-backed so the audio thread's drain is
    // allocation-free: std::swap(pending, drain_scratch_) is O(1) and
    // drain_scratch_.clear() preserves capacity. Coalescing happens on the
    // host side in set_parameter (linear scan). Both vectors reserve() at
    // prepare() time based on the plugin's parameter count.
    struct PendingEdit {
        uint32_t id;
        Vst::ParamValue normalized;
    };
    std::mutex                  pending_host_edits_mu_;
    std::vector<PendingEdit>    pending_host_edits_;   // written by host
    std::vector<PendingEdit>    drain_scratch_;        // drained by audio
    std::vector<float*> in_ptrs_;
    std::vector<float*> out_ptrs_;
    // Pre-allocated event/param buffers so the process callback never
    // heap-allocates.
    Vst::EventList in_events_;
    Vst::ParameterChanges in_param_changes_;
    std::atomic<bool> bypassed_{false};
    double sample_rate_ = 44100.0;
    int max_block_size_ = 0;
    bool active_ = false;
};

} // namespace

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
