// test_au_v2_cocoa_ui.mm — AU v2 Cocoa editor-view advertisement.
//
// Regression test for the bug ChainerSynth surfaced: no AU v2 adapter
// advertised kAudioUnitProperty_CocoaUI, so hosts (Logic, auval) never learned
// the plugin had a custom editor and showed their own generic param view. The
// fix wires a cross-TU filler hook (g_cocoa_view_info_filler) that the
// per-target Cocoa view module registers at static-init, and gives the factory
// class a per-plugin-unique name (PULP_AU_COCOA_VIEW_CLASS) so two Pulp AUs in
// one host don't collide on a process-global ObjC class name.
//
// This test compiles au_v2_cocoa_view.mm with PULP_AU_GUI + a test-unique
// class name and asserts the registration + the advertised view info, without
// constructing a real AudioComponentInstance.

#include <catch2/catch_test_macros.hpp>

#if defined(__APPLE__) && !TARGET_OS_IPHONE

#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include <pulp/format/au_v2_adapter.hpp>

TEST_CASE("AU v2 advertises a unique, resolvable Cocoa view factory",
          "[auv2][cocoa][editor]") {
    using namespace pulp::format::au;

    // The Cocoa view module's static-init registrar installs the filler when
    // linked (it's compiled into this test target with PULP_AU_GUI).
    REQUIRE(g_cocoa_view_info_filler != nullptr);

    AudioUnitCocoaViewInfo info{};
    REQUIRE(g_cocoa_view_info_filler(&info));

    // Bundle URL handed to the host (+1 retained; host owns/releases).
    REQUIRE(info.mCocoaAUViewBundleLocation != nullptr);

    // Advertised class name == the per-target unique name (set via the
    // PULP_AU_COCOA_VIEW_CLASS define on this test target below).
    REQUIRE(info.mCocoaAUViewClass[0] != nullptr);
    NSString* className = (__bridge NSString*)info.mCocoaAUViewClass[0];
    REQUIRE([className isEqualToString:@"PulpAUCocoaViewFactory_Test"]);

    // The advertised class resolves and conforms to AUCocoaUIBase — i.e. the
    // name we hand the host is the class we actually registered.
    Class cls = NSClassFromString(className);
    REQUIRE(cls != nil);
    REQUIRE([cls conformsToProtocol:@protocol(AUCocoaUIBase)]);

    // Release the +1 CF objects the filler handed us (the host would).
    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);
}

TEST_CASE("AU v2 editor pins the design viewport per the shared resize contract",
          "[auv2][cocoa][editor][resize]") {
    // Regression coverage for the Logic-clipping bug: the AU v2 Cocoa view
    // never called set_design_viewport, so a DAW resize clipped the fixed-size
    // tree while VST3/AUv3 scaled it. The pin decision is the shared
    // should_pin_design_viewport() predicate (also used by PulpPlugView), so
    // the formats cannot drift again; assert its full three-way contract here.
    // The set_design_viewport call itself cannot be exercised headlessly —
    // uiViewForAudioUnit: refuses to build an editor in CI/test environments
    // (editor_launch_blocked_by_environment) — so the windowed behavior rides
    // the VST3 resize tests (test_vst3_editor.cpp) as the cross-format
    // contract, plus manual Logic/auval verification.
    using pulp::format::ViewSize;
    using pulp::format::should_pin_design_viewport;

    // Not resizable (min==0): pin at preferred (letterbox scaling).
    ViewSize fixed{};
    fixed.preferred_width = 640;
    fixed.preferred_height = 400;
    CHECK(should_pin_design_viewport(fixed));

    // Resizable + aspect-locked: pin viewport + aspect (design-import path).
    ViewSize locked = fixed;
    locked.min_width = 640;
    locked.min_height = 400;
    locked.aspect_ratio = 1.6;
    CHECK(should_pin_design_viewport(locked));

    // Resizable + aspect==0: free drag — NO pin, the root reflows via Yoga.
    ViewSize free_drag = locked;
    free_drag.aspect_ratio = 0.0;
    CHECK_FALSE(should_pin_design_viewport(free_drag));

    // Degenerate preferred size: never pin (avoids a 0-aspect viewport).
    ViewSize degenerate{};
    degenerate.preferred_width = 0;
    degenerate.preferred_height = 0;
    CHECK_FALSE(should_pin_design_viewport(degenerate));
}

#else

TEST_CASE("AU v2 Cocoa view advertisement is mac-only", "[auv2][cocoa]") {
    SUCCEED("Not macOS — AU v2 Cocoa view test is a no-op.");
}

#endif
