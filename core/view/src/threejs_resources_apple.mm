// SPDX-License-Identifier: MIT
//
// threejs_resources_apple.mm — iOS Three.js IIFE resource lookup.
//
// iOS/simulator implementation of `pulp::view::threejs_iife_source()`.
// On iOS we load `threejs/three.iife.js` from inside the AUv3 `.appex`
// via `NSBundle`. iOS bundles are flat — `[NSBundle resourcePath]` IS
// the bundle root, so the loader resolves
// `<resourcePath>/threejs/three.iife.js` = `<appex>/threejs/...`
// (NOT `<appex>/Resources/threejs/...`, which is the macOS-only
// layout that NSBundle does not search on iOS). The bundle is located
// by walking up from the `PulpAUViewController` class (the principal
// class of the `.appex`). On macOS we deliberately return
// `std::nullopt` — the macOS lane resolves Three.js source files via
// V8's public ESM API, not via the embedded IIFE bundle.

#include "pulp/view/threejs_resources.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)

#import <Foundation/Foundation.h>

namespace pulp::view {

std::optional<std::string> threejs_iife_source() {
    @autoreleasepool {
        // Walk up from the AU view controller principal class so we
        // resolve to the `.appex` bundle, not the host application
        // bundle. If the principal class hasn't been loaded yet (e.g.
        // unit tests running outside an .appex), fall back to the
        // main bundle so the test infrastructure can stage the
        // resource alongside the test binary.
        Class principalClass = NSClassFromString(@"PulpAUViewController");
        NSBundle *bundle = principalClass ? [NSBundle bundleForClass:principalClass]
                                          : [NSBundle mainBundle];
        if (!bundle) {
            return std::nullopt;
        }

        NSString *path = [bundle pathForResource:@"three.iife"
                                          ofType:@"js"
                                     inDirectory:@"threejs"];
        if (!path) {
            // Try without the threejs/ subdirectory in case a future
            // resource layout collapses the folder.
            path = [bundle pathForResource:@"three.iife" ofType:@"js"];
        }
        if (!path) {
            return std::nullopt;
        }

        NSError *err = nil;
        NSString *contents = [NSString stringWithContentsOfFile:path
                                                       encoding:NSUTF8StringEncoding
                                                          error:&err];
        if (!contents || err) {
            return std::nullopt;
        }
        const char *utf8 = [contents UTF8String];
        if (!utf8) {
            return std::nullopt;
        }
        return std::string{utf8};
    }
}

}  // namespace pulp::view

#endif  // __APPLE__ && (TARGET_OS_IPHONE || TARGET_OS_SIMULATOR)
