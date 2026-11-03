// pulp-slice-formats-<kind> <out-dir>
//
// The slice's processor and editor inside Pulp's real VST3, CLAP and AU v2
// adapters, in process, each opening the editor through the editor-host
// contract the way that format's host does:
//
// - VST3: PulpVst3Processor::createView() -> ContractPlugView, attached to a
//   real NSView;
// - CLAP: the plug-in's contract clap_plugin_gui, set_parent on a real NSView;
// - AU v2: kAudioUnitProperty_CocoaUI -> the contract view factory, whose
//   container view is added to a real window.
//
// For each: open, attach, resize, a knob drag that the host sees as one
// gesture, Space forwarded to the host, close (every gesture released, every
// subview gone), reopen with the value surviving, close. Each check prints one
// "[<kind>/<format>] ok|FAIL: ..." line; exit code 0 means every check passed.

#include "gain_meter_processor.hpp"
#include "host_window_mac.hpp"

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/clap_adapter.hpp>
#include <pulp/format/clap_contract_gui.hpp>
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/vst3_adapter.hpp>
#include <pulp/format/vst3_contract_view.hpp>
#include <pulp/state/store.hpp>

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include <clap/clap.h>
#include <pluginterfaces/base/keycodes.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef SLICE_KIND
#error "SLICE_KIND must name this executable's editor kind"
#endif
#ifndef SLICE_VALUE_KEY
#error "SLICE_VALUE_KEY must name the key the editor reports its value under"
#endif

extern "C" const clap_plugin_entry_t clap_entry;
extern "C" void* PulpSliceFormatsAUFactory(const AudioComponentDescription*);

namespace pulp::slice {
int slice_editor_live_instances();
}

using namespace pulp;
using namespace pulp::slice;

namespace {

int g_failures = 0;

void check(const char* format, bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[%s/%s] FAIL: %s\n", SLICE_KIND, format, what);
    } else {
        std::printf("[%s/%s] ok: %s\n", SLICE_KIND, format, what);
    }
}

float observe_value(format::Editor* editor) {
    std::string reply;
    if (editor == nullptr || editor->automation() == nullptr ||
        !editor->automation()->observe("", reply))
        return -1.0f;
    const std::string key = "\"" SLICE_VALUE_KEY "\":";
    const auto at = reply.find(key);
    return at == std::string::npos ? -1.0f : std::strtof(reply.c_str() + at + key.size(), nullptr);
}

bool drag(format::Editor* editor) {
    std::string error;
    return editor != nullptr && editor->automation() != nullptr &&
           editor->automation()->inject_input("drag -50", error);
}

const format::EditorKeyEvent kSpace{format::EditorKey::space, U' ', 0, true, false};

// ── VST3 ─────────────────────────────────────────────────────────────────────

class Vst3HostApp final : public Steinberg::Vst::IHostApplication {
  public:
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override {
        name[0] = 0;
        return Steinberg::kResultTrue;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID,
                                                 void** obj) override {
        if (obj)
            *obj = nullptr;
        return Steinberg::kNotImplemented;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        return 1;
    }
};

// The host's side of the gesture path, as "B1 V1 ... E1".
class Vst3Handler final : public Steinberg::Vst::IComponentHandler {
  public:
    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override {
        stream += "B" + std::to_string(id) + " ";
        ++open;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                              Steinberg::Vst::ParamValue) override {
        stream += "V" + std::to_string(id) + " ";
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override {
        stream += "E" + std::to_string(id) + " ";
        --open;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32) override {
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
            return Steinberg::kResultTrue;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        return 1;
    }
    std::string stream;
    int open = 0;
};

void run_vst3(void* parent) {
    const char* f = "vst3";
    Vst3HostApp app;
    Vst3Handler handler;
    format::vst3::PulpVst3Processor vst3(GainMeterProcessor::create);
    check(f, vst3.initialize(&app) == Steinberg::kResultOk, "the component initializes");
    vst3.setComponentHandler(&handler);
    const int baseline = host_view_subview_count(parent);
    float held = -1.0f;
    for (int round = 0; round < 2; ++round) {
        auto* view = dynamic_cast<format::vst3::ContractPlugView*>(
            vst3.createView(Steinberg::Vst::ViewType::kEditor));
        check(f, view != nullptr, "createView returns the contract view");
        if (view == nullptr)
            return;
        check(f, view->attached(parent, Steinberg::kPlatformTypeNSView) == Steinberg::kResultTrue,
              "attach to a real NSView");
        check(f, host_view_subview_count(parent) > baseline, "attach added a native subview");
        Steinberg::ViewRect rect(0, 0, 320, 200);
        check(f, view->onSize(&rect) == Steinberg::kResultTrue, "resize to 320x200");
        if (round == 0) {
            handler.stream.clear();
            check(f, drag(view->editor()), "a knob drag through the editor");
            held = observe_value(view->editor());
            check(f, held > kGainDefault + 0.05f, "the drag raised the gain");
            check(f,
                  handler.stream.rfind("B1 ", 0) == 0 && handler.stream.size() >= 3 &&
                      handler.stream.compare(handler.stream.size() - 3, 3, "E1 ") == 0,
                  "the host sees one gesture: begin first, end last");
        } else {
            check(f, std::fabs(observe_value(view->editor()) - held) < 1e-4f,
                  "the reopened editor shows the surviving value");
        }
        check(f, view->onKeyDown(u' ', Steinberg::KEY_SPACE, 0) == Steinberg::kResultFalse,
              "Space is forwarded to the host");
        check(f, view->removed() == Steinberg::kResultOk, "close");
        check(f, handler.open == 0, "no gesture is left open after closing");
        check(f, host_view_subview_count(parent) == baseline, "closing removed every subview");
        view->release();
        check(f, slice_editor_live_instances() == 0, "closing destroyed the editor's UI");
    }
    vst3.terminate();
}

// ── CLAP ─────────────────────────────────────────────────────────────────────

void run_clap(void* parent) {
    const char* f = "clap";
    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.name = "pulp-slice-formats";
    host.vendor = "Pulp";
    host.url = "";
    host.version = "1";
    host.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
    host.request_restart = [](const clap_host_t*) {};
    host.request_process = [](const clap_host_t*) {};
    host.request_callback = [](const clap_host_t*) {};

    check(f, clap_entry.init("pulp-slice-formats"), "the entry initializes");
    auto* factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t* plugin = factory->create_plugin(factory, &host, desc->id);
    check(f, plugin != nullptr && plugin->init(plugin), "the plug-in instantiates");
    if (plugin == nullptr)
        return;
    auto& store = static_cast<format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data)->store;
    const auto* gui =
        static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    check(f, gui != nullptr, "the plug-in offers its contract GUI");
    if (gui == nullptr) {
        plugin->destroy(plugin);
        return;
    }
    const int baseline = host_view_subview_count(parent);
    float held = -1.0f;
    for (int round = 0; round < 2; ++round) {
        check(f, gui->create(plugin, CLAP_WINDOW_API_COCOA, false), "gui create");
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_COCOA;
        window.cocoa = parent;
        check(f, gui->set_parent(plugin, &window), "attach to a real NSView");
        check(f, host_view_subview_count(parent) > baseline, "attach added a native subview");
        check(f, gui->set_size(plugin, 320, 200), "resize to 320x200");
        auto* session = format::clap_adapter::contract_gui_session(plugin);
        check(f, session != nullptr, "the GUI's contract session is reachable");
        auto* editor = session != nullptr ? session->editor() : nullptr;
        if (round == 0) {
            check(f, drag(editor), "a knob drag through the editor");
            held = observe_value(editor);
            check(f, held > kGainDefault + 0.05f, "the drag raised the gain");
            check(f, std::fabs(store.get_normalized(kGainParam) - held) < 1e-4f,
                  "the plug-in's store holds the dragged value");
        } else {
            check(f, std::fabs(observe_value(editor) - held) < 1e-4f,
                  "the reopened editor shows the surviving value");
        }
        check(f,
              session != nullptr && session->offer_key(kSpace) == format::KeyDisposition::forward,
              "Space is forwarded to the host");
        gui->destroy(plugin);
        check(f, store.open_gesture_count() == 0, "no gesture is left open after closing");
        check(f, host_view_subview_count(parent) == baseline, "closing removed every subview");
        check(f, slice_editor_live_instances() == 0, "closing destroyed the editor's UI");
    }
    plugin->destroy(plugin);
    clap_entry.deinit();
}

// ── AU v2 ────────────────────────────────────────────────────────────────────

void run_au(void* parent) {
    const char* f = "au";
    AudioComponentDescription desc{kAudioUnitType_Effect, 'SlFm', 'PlSl', 0, 0};
    AudioComponent comp = AudioComponentRegister(
        &desc, CFSTR("Pulp: editor-host slice formats"), 1,
        reinterpret_cast<AudioComponentFactoryFunction>(PulpSliceFormatsAUFactory));
    AudioUnit au = nullptr;
    check(f, comp != nullptr && AudioComponentInstanceNew(comp, &au) == noErr,
          "the audio unit instantiates");
    if (au == nullptr)
        return;
    format::au::PulpEditorContext ctx{};
    UInt32 size = sizeof(ctx);
    AudioUnitGetProperty(au, format::au::kPulpEditorContextProperty, kAudioUnitScope_Global, 0,
                         &ctx, &size);
    NSView* host_view = (__bridge NSView*)parent;
    const int baseline = host_view_subview_count(parent);
    float held = -1.0f;
    for (int round = 0; round < 2; ++round) {
        // Everything AppKit autoreleases while the view is open drains here, as
        // a host's run loop does, so closing really lets go of the view.
        bool opened = false;
        @autoreleasepool {
            AudioUnitCocoaViewInfo info{};
            size = sizeof(info);
            const bool advertised =
                AudioUnitGetProperty(au, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                                     &info, &size) == noErr;
            check(f, advertised, "the unit advertises its Cocoa view");
            if (!advertised)
                break;
            Class factory_class = NSClassFromString((__bridge NSString*)info.mCocoaAUViewClass[0]);
            CFRelease(info.mCocoaAUViewBundleLocation);
            CFRelease(info.mCocoaAUViewClass[0]);
            id<AUCocoaUIBase> factory = [[factory_class alloc] init];
            NSView* view = [factory uiViewForAudioUnit:au withSize:NSMakeSize(320, 200)];
            check(f, view != nil, "attach to a real NSView");
            if (view == nil)
                break;
            opened = true;
            [host_view addSubview:view];
            check(f, host_view_subview_count(parent) > baseline, "attach added a native subview");
            [view setFrameSize:NSMakeSize(320, 200)];
            auto* session = format::au::contract_view_session((__bridge void*)view);
            check(f, session != nullptr, "the view's contract session is reachable");
            auto* editor = session != nullptr ? session->editor() : nullptr;
            check(f, editor != nullptr && editor->size().width == 320, "resize to 320x200");
            if (round == 0) {
                check(f, drag(editor), "a knob drag through the editor");
                held = observe_value(editor);
                check(f, held > kGainDefault + 0.05f, "the drag raised the gain");
                check(f,
                      ctx.store != nullptr &&
                          std::fabs(ctx.store->get_normalized(kGainParam) - held) < 1e-4f,
                      "the unit's store holds the dragged value");
            } else {
                check(f, std::fabs(observe_value(editor) - held) < 1e-4f,
                      "the reopened editor shows the surviving value");
            }
            check(f,
                  session != nullptr &&
                      session->offer_key(kSpace) == format::KeyDisposition::forward,
                  "Space is forwarded to the host");
            [view removeFromSuperview]; // the host closes its editor and drops the view
        }
        if (!opened)
            break;
        host_pump_events();
        check(f, ctx.store != nullptr && ctx.store->open_gesture_count() == 0,
              "no gesture is left open after closing");
        check(f, host_view_subview_count(parent) == baseline, "closing removed every subview");
        check(f, slice_editor_live_instances() == 0, "closing destroyed the editor's UI");
    }
    AudioComponentInstanceDispose(au);
}

} // namespace

int main() {
    // Plug-in editors are refused in CI and headless environments; this is a
    // test that opens them on purpose, in a window it never shows.
    for (const char* name : {"CI", "PULP_HEADLESS", "PULP_TEST_MODE", "PULP_DISABLE_PLUGIN_EDITOR"})
        ::unsetenv(name);
    host_app_init();
    void* window = host_window_create(480, 320);
    void* parent = host_window_content_view(window);
    run_vst3(parent);
    run_clap(parent);
    run_au(parent);
    host_window_destroy(window);
    std::printf("[%s/formats] %s\n", SLICE_KIND, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
