// test_au_v2_cocoa_shipping.mm — the AU v2 Cocoa view module as a plug-in ships.
//
// Compiles au_v2_cocoa_view.mm with the definitions pulp_add_plugin() gives a
// shipped AU v2 target (PULP_AU_GUI and a per-plug-in factory class) and without
// PULP_AU_V2_TEST_HOOKS, then checks the factory class carries no test-only
// accessor. Objective-C dispatches by name at runtime, so a method compiled
// into a shipped plug-in is reachable from any code in the host process.

#include <TargetConditionals.h>
#include <catch2/catch_test_macros.hpp>

#if defined(__APPLE__) && !TARGET_OS_IPHONE

#import <AppKit/AppKit.h>

TEST_CASE("A shipped AU v2 Cocoa view factory has no test-only host accessor",
          "[auv2][cocoa][editor]") {
    Class factory_class = NSClassFromString(@"PulpAUCocoaViewFactory_Shipping");
    REQUIRE(factory_class != nil);
    CHECK_FALSE(
        [factory_class respondsToSelector:NSSelectorFromString(@"pulpEditorHostForTesting:")]);
}

#else

TEST_CASE("AU v2 Cocoa view shipping check is mac-only", "[auv2][cocoa]") {
    REQUIRE(true);
}

#endif
