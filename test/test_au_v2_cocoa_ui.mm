// test_au_v2_cocoa_ui.mm — AU v2 Cocoa editor-view advertisement.
//
// AU v2 adapters must advertise kAudioUnitProperty_CocoaUI so hosts (Logic,
// auval) learn the plugin has a custom editor instead of showing a generic
// param view. The cross-TU filler hook (g_cocoa_view_info_filler) is registered
// by the per-target Cocoa view module at static-init, and the factory class gets
// a per-plugin-unique name (PULP_AU_COCOA_VIEW_CLASS) so two Pulp AUs in one
// host don't collide on a process-global ObjC class name.
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

#else

TEST_CASE("AU v2 Cocoa view advertisement is mac-only", "[auv2][cocoa]") {
    SUCCEED("Not macOS — AU v2 Cocoa view test is a no-op.");
}

#endif
