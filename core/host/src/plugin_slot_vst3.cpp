// VST3 plugin slot implementation.
//
// Loads a .vst3 bundle, resolves GetPluginFactory, creates the first audio
// processor class, and wires the PluginSlot interface onto IComponent +
// IAudioProcessor. Minimum viable host — covers audio pass-through with
// stereo 2-in/2-out, latency reporting, and bypass. Parameter automation,
// state serialization, MIDI routing, and editor views are follow-up work.

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

// Hosting helpers — workstream 03 slice 3.5.
#include <public.sdk/source/vst/hosting/parameterchanges.h>
#include <public.sdk/source/vst/hosting/eventlist.h>

#include <pulp/host/dl_shim.hpp>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
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

// Minimal IHostApplication — only exists so IComponent::initialize() can take
// a context. We don't proxy any useful services back to the plugin.
class HostApp final : public Vst::IHostApplication {
public:
    tresult PLUGIN_API getName(Vst::String128 name) override {
        const char* kName = "Pulp";
        for (int i = 0; i < 127 && kName[i]; ++i) name[i] = (Vst::TChar)kName[i];
        name[4] = 0;
        return kResultTrue;
    }
    tresult PLUGIN_API createInstance(TUID /*cid*/, TUID /*iid*/, void** obj) override {
        if (obj) *obj = nullptr;
        return kNotImplemented;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, Vst::IHostApplication::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *obj = static_cast<Vst::IHostApplication*>(this);
            return kResultTrue;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
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
        cache_params_();
    }

    ~Vst3Slot() override {
        release();
        // Combined plugins implement IComponent + IEditController on the
        // same object — terminating both pointers would call IPluginBase
        // ::terminate() twice. Detect by raw-pointer equality on the
        // FUnknown side and only terminate once.
        const bool combined = (controller_ != nullptr
            && static_cast<FUnknown*>(controller_) == static_cast<FUnknown*>(component_));
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

        // Build VST3 event list from Pulp's MidiBuffer. Workstream 03 3.5 —
        // previously midi_in was discarded, so instruments loaded in the
        // host received no MIDI. Maps note_on/note_off; other event types
        // (CC, pitchbend, aftertouch) will follow when the host queue
        // itself carries them — MidiBuffer today is choc::ShortMessage only.
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
        // Plain-domain input, feed normalized to the controller AND queue a
        // processor-side edit via ParameterEventQueue in the next process()
        // block. For now the controller mirror suffices for UI; processor
        // automation is delivered via ParameterEventQueue at process() time.
        Vst::ParamValue norm =
            controller_->plainParamToNormalized(id, plain_value);
        controller_->setParamNormalized(id, norm);
        // Also stash in pending_host_edits_ so the next process() picks it up.
        pending_host_edits_.push_back({id, norm});
    }

    void set_bypass(bool bypassed) override {
        bypassed_.store(bypassed, std::memory_order_relaxed);
    }
    bool is_bypassed() const override {
        return bypassed_.load(std::memory_order_relaxed);
    }

    std::vector<uint8_t> save_state() const override {
        if (!component_) return {};
        VectorStream stream;
        if (component_->getState(&stream) != kResultOk) return {};
        return stream.take();
    }

    bool restore_state(const std::vector<uint8_t>& data) override {
        if (!component_ || data.empty()) return false;
        VectorStream stream(data);
        return component_->setState(&stream) == kResultOk;
    }

    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    int latency_samples() const override {
        if (!processor_) return 0;
        return (int)processor_->getLatencySamples();
    }
    int tail_samples() const override {
        if (!processor_) return 0;
        return (int)processor_->getTailSamples();
    }

    static HostApp& host_app() {
        static HostApp app;
        return app;
    }

    // Minimal IBStream backed by std::vector<uint8_t> for state round-trips.
    class VectorStream final : public IBStream {
    public:
        VectorStream() = default;
        explicit VectorStream(const std::vector<uint8_t>& src) : buf_(src) {}
        std::vector<uint8_t> take() { return std::move(buf_); }
        tresult PLUGIN_API read(void* buffer, int32 num_bytes,
                                int32* num_bytes_read) override {
            if (num_bytes < 0) return kInvalidArgument;
            int64 remaining = (int64)buf_.size() - pos_;
            int64 n = num_bytes < remaining ? num_bytes : remaining;
            if (n > 0) std::memcpy(buffer, buf_.data() + pos_, (size_t)n);
            pos_ += n;
            if (num_bytes_read) *num_bytes_read = (int32)n;
            return kResultOk;
        }
        tresult PLUGIN_API write(void* buffer, int32 num_bytes,
                                 int32* num_bytes_written) override {
            if (num_bytes < 0) return kInvalidArgument;
            const auto* p = static_cast<const uint8_t*>(buffer);
            buf_.insert(buf_.end(), p, p + num_bytes);
            pos_ = (int64)buf_.size();
            if (num_bytes_written) *num_bytes_written = num_bytes;
            return kResultOk;
        }
        tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
            int64 new_pos = pos_;
            switch (mode) {
                case kIBSeekSet: new_pos = pos; break;
                case kIBSeekCur: new_pos = pos_ + pos; break;
                case kIBSeekEnd: new_pos = (int64)buf_.size() + pos; break;
                default: return kInvalidArgument;
            }
            if (new_pos < 0 || new_pos > (int64)buf_.size()) return kInvalidArgument;
            pos_ = new_pos;
            if (result) *result = pos_;
            return kResultOk;
        }
        tresult PLUGIN_API tell(int64* pos) override {
            if (!pos) return kInvalidArgument;
            *pos = pos_;
            return kResultOk;
        }
        tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
            if (FUnknownPrivate::iidEqual(iid, IBStream::iid)
                || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
                *obj = static_cast<IBStream*>(this);
                return kResultTrue;
            }
            *obj = nullptr;
            return kNoInterface;
        }
        uint32 PLUGIN_API addRef() override { return 1; }
        uint32 PLUGIN_API release() override { return 1; }
    private:
        std::vector<uint8_t> buf_;
        int64 pos_ = 0;
    };

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
    Vst::IComponent* component_ = nullptr;
    Vst::IAudioProcessor* processor_ = nullptr;
    Vst::IEditController* controller_ = nullptr;
    std::vector<HostParamInfo> params_;
    struct PendingEdit { uint32_t id; Vst::ParamValue normalized; };
    std::vector<PendingEdit> pending_host_edits_;
    std::vector<float*> in_ptrs_;
    std::vector<float*> out_ptrs_;
    // Workstream 03 slice 3.5 — pre-allocated event/param buffers so the
    // process callback never heap-allocates.
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
        // this, though a few require a proper CFBundle. Adding CFBundle
        // support is a follow-up.
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
    // component for its controller class id and factory-create it; this is
    // a minimum viable implementation (no IConnectionPoint wiring yet).
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

} // namespace pulp::host
