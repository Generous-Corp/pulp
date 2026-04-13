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

#include <dlfcn.h>
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
             Vst::IComponent* component, Vst::IAudioProcessor* processor)
        : info_(std::move(info)),
          handle_(handle),
          factory_(factory),
          component_(component),
          processor_(processor) {}

    ~Vst3Slot() override {
        release();
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
                 const midi::MidiBuffer& /*midi_in*/,
                 midi::MidiBuffer& /*midi_out*/,
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

        Vst::ProcessData data{};
        data.processMode        = Vst::kRealtime;
        data.symbolicSampleSize = Vst::kSample32;
        data.numSamples         = num_samples;
        data.numInputs          = nch_in  ? 1 : 0;
        data.numOutputs         = nch_out ? 1 : 0;
        data.inputs             = nch_in  ? &in_bus  : nullptr;
        data.outputs            = nch_out ? &out_bus : nullptr;
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

    std::vector<HostParamInfo> parameters() const override {
        // TODO: query IEditController for parameter metadata.
        return {};
    }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}

    void set_bypass(bool bypassed) override {
        bypassed_.store(bypassed, std::memory_order_relaxed);
    }
    bool is_bypassed() const override {
        return bypassed_.load(std::memory_order_relaxed);
    }

    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }

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

private:
    PluginInfo info_;
    void* handle_ = nullptr;
    IPluginFactory* factory_ = nullptr;
    Vst::IComponent* component_ = nullptr;
    Vst::IAudioProcessor* processor_ = nullptr;
    std::vector<float*> in_ptrs_;
    std::vector<float*> out_ptrs_;
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

    PluginInfo filled = info;
    if (filled.name.empty())         filled.name = chosen.name;
    if (filled.manufacturer.empty()) filled.manufacturer = ""; // PClassInfo2 carries vendor
    if (filled.unique_id.empty())    filled.unique_id = chosen.name;

    return std::make_unique<Vst3Slot>(std::move(filled), handle, factory,
                                      component, processor);
}

} // namespace pulp::host
