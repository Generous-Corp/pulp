// test_native_surface_au_v3.mm - the metal-probe editor (a native Metal
// surface) opened through the macOS AU v3 view controller, in an offscreen
// window.
//
// A real PulpAudioUnit is built over the probe processor, and the extension's
// principal class is bound to it as the extension does. The test plays the
// host's part: it adds the controller's view to a window, resizes it, and
// unbinds the audio unit. What the host sees is asserted by the checks shared
// with the other formats (support/native_surface_format_checks.hpp). Skips,
// saying so, without a Metal device. Compiled with ARC.

#include <catch2/catch_test_macros.hpp>

#include "support/native_surface_format_checks.hpp"
#include "support/recording_contract_editor.hpp"

#include "metal_probe_processor.hpp"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>

#import "../core/format/src/au_audio_unit.h"
#import "../core/format/src/au_v3_contract_view.h"

#include <pulp/format/contract_editor_session.hpp>
#include <pulp/format/editor_package.hpp>
#include <pulp/format/registry.hpp>
#include <pulp/state/store.hpp>

using namespace pulp::test_support;

namespace {

struct InstallHook {
    InstallHook() {
        pulp::format::install_plugin_editor_factory_hook(&pulp_native_metal_probe::editor_factory);
    }
} g_install_hook;

struct ScopedFactoryRegistration {
    explicit ScopedFactoryRegistration(pulp::format::ProcessorFactory factory)
        : previous(pulp::format::registered_factory()) {
        pulp::format::register_plugin(factory);
    }
    ~ScopedFactoryRegistration() {
        pulp::format::register_plugin(previous);
    }
    pulp::format::ProcessorFactory previous;
};

} // namespace

TEST_CASE("AU v3 gives a native-surface editor its size, first frame, resize and reopen",
          "[native-surface][auv3][editor-host][format-host]") {
    if (!metal_device_available())
        SKIP("no Metal device on this host; the AU v3 native surface was not exercised");
    EditorEnvironment env;
    MainLoop loop;
    ScopedFactoryRegistration registration(pulp_native_metal_probe::create_metal_probe);

    // Each open binds a fresh controller to a fresh audio unit, as a host does
    // for each plug-in window.
    PulpAudioUnit* unit = nil;
    NSViewController* controller = nil;
    NSView* host_container = nil;

    auto contract_view = [&]() -> PulpAUv3ContractView* {
        for (NSView* v in controller.view.subviews)
            if ([v isKindOfClass:[PulpAUv3ContractView class]])
                return (PulpAUv3ContractView*)v;
        return nil;
    };

    SurfaceFormatDriver driver;
    driver.open = [&](NSWindow* window, float gain) -> NSView* {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Effect;
        desc.componentSubType = 'NsAu';
        desc.componentManufacturer = 'Plup';
        NSError* error = nil;
        unit = [[PulpAudioUnit alloc] initWithComponentDescription:desc options:0 error:&error];
        REQUIRE(unit != nil);
        [unit pulpStore]->set_value(pulp_native_metal_probe::kGain, gain);
        controller = [[NSClassFromString(@"PulpAUMacViewController") alloc] init];
        (void)controller.view;
        [controller setValue:unit forKey:@"audioUnit"];
        // The host's own container for the remote view: the controller's view
        // fills it, and the host sizes it to the controller's preferredContentSize.
        const NSSize preferred = controller.preferredContentSize;
        host_container =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, preferred.width, preferred.height)];
        host_container.autoresizingMask = NSViewNotSizable;
        [window.contentView addSubview:host_container];
        [host_container addSubview:controller.view];
        return contract_view();
    };
    driver.editor = [&]() -> pulp::format::Editor* {
        auto* view = contract_view();
        auto* session = view ? [view session] : nullptr;
        return session ? session->editor() : nullptr;
    };
    driver.host_resize = [&](std::uint32_t width, std::uint32_t height) {
        // An AU host sizes the view that holds the plug-in's; the controller's
        // view and the contract view inside it follow.
        [host_container setFrameSize:NSMakeSize(width, height)];
        return true;
    };
    driver.reported_size = [&] {
        auto* view = contract_view();
        auto* session = view ? [view session] : nullptr;
        return session ? session->size() : pulp::format::EditorSize{};
    };
    driver.close = [&] {
        // The host drops the plug-in's window: the controller's view leaves the
        // window, and the audio unit goes, which closes the editor in order.
        [host_container removeFromSuperview];
        host_container = nil;
        [controller.view removeFromSuperview];
        [controller setValue:nil forKey:@"audioUnit"];
        controller = nil;
        unit = nil;
    };
    driver.run_loop = [&](double seconds) { loop.run_for(seconds); };

    check_native_surface_lifecycle(driver);
}
