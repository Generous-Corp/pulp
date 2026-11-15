// test_native_surface_au_v2.mm - the metal-probe editor (a native Metal
// surface) opened through the AU v2 contract view, in an offscreen window.
//
// au_v2_contract_view.mm is compiled into this test with test-unique class
// names, and a fake Audio Unit hands it the processor, store and session
// registry the way an AU v2 adapter does. What the host sees is asserted by
// the checks shared with the other formats (support/native_surface_format_checks.hpp).
// Skips, saying so, without a Metal device. Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include "support/native_surface_format_checks.hpp"
#include "support/recording_contract_editor.hpp"

#include "metal_probe_processor.hpp"

#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include <pulp/format/au_v2_common.hpp>
#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/editor_package.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <pulp/state/store.hpp>

#include <memory>

using namespace pulp::test_support;

namespace {

// The probe plug-in's editor is its UI package's, installed as the generated
// hook does for a plug-in binary.
struct InstallHook {
    InstallHook() {
        pulp::format::install_plugin_editor_factory_hook(&pulp_native_metal_probe::editor_factory);
    }
} g_install_hook;

// What an AU adapter instance holds for its editors, in the same order.
struct FakeInstance {
    pulp::state::StateStore store;
    std::unique_ptr<pulp_native_metal_probe::MetalProbeProcessor> processor =
        std::make_unique<pulp_native_metal_probe::MetalProbeProcessor>();
    pulp::runtime::AliveToken alive;
    std::unique_ptr<pulp::format::ContractSessionRegistry> sessions =
        std::make_unique<pulp::format::ContractSessionRegistry>();

    FakeInstance() {
        processor->set_state_store(&store);
        processor->define_parameters(store);
    }
};
FakeInstance* g_instance = nullptr;

struct FakeAU {
    AudioComponentPlugInInterface iface{};
};

OSStatus fake_get_property(void*, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement,
                           void* out, UInt32* io_size) {
    if (id != pulp::format::au::kPulpEditorContextProperty || !out || !io_size ||
        *io_size < sizeof(pulp::format::au::PulpEditorContext) || g_instance == nullptr)
        return kAudioUnitErr_InvalidProperty;
    auto* ctx = static_cast<pulp::format::au::PulpEditorContext*>(out);
    ctx->processor = g_instance->processor.get();
    ctx->store = &g_instance->store;
    ctx->owner_alive = g_instance->alive.capture();
    ctx->contract_sessions = g_instance->sessions.get();
    *io_size = sizeof(*ctx);
    return noErr;
}
OSStatus fake_open(void*, AudioComponentInstance) {
    return noErr;
}
OSStatus fake_close(void* self) {
    delete static_cast<FakeAU*>(self);
    return noErr;
}
AudioComponentMethod fake_lookup(SInt16 selector) {
    if (selector == kAudioUnitGetPropertySelect)
        return reinterpret_cast<AudioComponentMethod>(&fake_get_property);
    return nullptr;
}
AudioComponentPlugInInterface* fake_factory(const AudioComponentDescription*) {
    auto* au = new FakeAU;
    au->iface.Open = fake_open;
    au->iface.Close = fake_close;
    au->iface.Lookup = fake_lookup;
    return &au->iface;
}

AudioUnit fake_audio_unit() {
    static AudioComponent comp = [] {
        AudioComponentDescription desc{kAudioUnitType_Effect, 'NsAu', 'PlTs', 0, 0};
        return AudioComponentRegister(&desc, CFSTR("Pulp: native surface AU test"), 1,
                                      fake_factory);
    }();
    AudioUnit au = nullptr;
    REQUIRE(comp != nullptr);
    REQUIRE(AudioComponentInstanceNew(comp, &au) == noErr);
    return au;
}

} // namespace

TEST_CASE("AU v2 gives a native-surface editor its size, first frame, resize and reopen",
          "[native-surface][auv2][editor-host][format-host]") {
    if (!metal_device_available())
        SKIP("no Metal device on this host; the AU v2 native surface was not exercised");
    EditorEnvironment env;
    FakeInstance instance;
    g_instance = &instance;
    AudioUnit au = fake_audio_unit();
    MainLoop loop;
    NSView* container = nil;

    SurfaceFormatDriver driver;
    driver.open = [&](NSWindow* window, float gain) -> NSView* {
        instance.store.set_value(pulp_native_metal_probe::kGain, gain);
        Class factory_class = NSClassFromString(@"PulpAUContractViewFactory_NativeSurface");
        REQUIRE(factory_class != nil);
        id<AUCocoaUIBase> factory = [[factory_class alloc] init];
        container = [factory uiViewForAudioUnit:au withSize:NSMakeSize(640, 480)];
        if (container != nil)
            [window.contentView addSubview:container];
        return container;
    };
    driver.editor = [&]() -> pulp::format::Editor* {
        if (container == nil)
            return nullptr;
        auto* session = pulp::format::au::contract_view_session((__bridge void*)container);
        return session ? session->editor() : nullptr;
    };
    driver.host_resize = [&](std::uint32_t width, std::uint32_t height) {
        // An AU host resizes the view it was given; the view tells the editor.
        [container setFrameSize:NSMakeSize(width, height)];
        // The format took it when the editor's session reports the size.
        auto* session = pulp::format::au::contract_view_session((__bridge void*)container);
        return session && session->size().width == width && session->size().height == height;
    };
    driver.reported_size = [&] {
        auto* session = pulp::format::au::contract_view_session((__bridge void*)container);
        return session ? session->size() : pulp::format::EditorSize{};
    };
    // The view's dealloc closes the editor, which the live-surface counts after
    // `release` prove.
    driver.close_removes_surface = false;
    driver.close = [&] { [container removeFromSuperview]; };
    driver.release = [&] { container = nil; };
    driver.run_loop = [&](double seconds) { loop.run_for(seconds); };

    check_native_surface_lifecycle(driver);

    AudioComponentInstanceDispose(au);
    g_instance = nullptr;
}
