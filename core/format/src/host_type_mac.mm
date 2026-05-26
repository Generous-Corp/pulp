// Apple-only `current_auv3_wrapper_identifier` impl.
//
// AU v3 plug-ins live in a sandboxed `.appex`. The bundle's own
// executable name (`PulpAUv3Extension`, `*.AppExtension`, etc.) does
// NOT classify the host — the wrapping host's bundle id has to come
// from Apple's host-service framework instead. Item 3.1 / DAW-quirks
// row 22 / item 5.11.
//
// Order of preference, in approximate reliability:
//   1. `AU_HOST_BUNDLE_ID` / `AU_HOST_IDENTIFIER` environment variables
//      — Apple's host services set these for some AU v3 wrapper paths
//      (Logic 10.5+ AUHostingServiceXPC, MainStage, certain iOS host
//      apps). Authoritative when present.
//   2. The main bundle id when the main bundle is NOT the AU v3
//      extension itself — i.e. the AU is being loaded in-process (rare
//      for AU v3 on shipping macOS, but happens during auval and inside
//      the Pulp standalone host).
//   3. `[NSProcessInfo processInfo].processName` when it carries a
//      recognisable Apple wrapper name (`AUHostingServiceXPC_*`,
//      `AUHostingService`). The classifier in `host_type.cpp` then
//      decides whether the name resolves to a known host.
//
// Returns an empty string when no wrapper context is detected — the
// caller falls back to the executable-path heuristic.

#include <pulp/format/host_type.hpp>

#import <Foundation/Foundation.h>

#include <cstdlib>

namespace pulp::format {

static std::string nsstring_to_std(NSString* s) {
    if (s == nil) return {};
    const char* utf8 = [s UTF8String];
    if (utf8 == nullptr) return {};
    return std::string(utf8);
}

std::string current_auv3_wrapper_identifier() {
    @autoreleasepool {
        // (1) Environment variable channel — preferred when present.
        if (const char* env = std::getenv("AU_HOST_BUNDLE_ID"); env && *env) {
            return std::string(env);
        }
        if (const char* env = std::getenv("AU_HOST_IDENTIFIER"); env && *env) {
            return std::string(env);
        }

        // (2) Process / bundle inspection. The bundle id of the main
        // bundle is the AU v3 extension's own id when we are running
        // sandboxed; in that case we cannot derive the host id from it.
        // Detect the extension case by looking for the `.appex` suffix
        // and skip (2).
        NSBundle* main = [NSBundle mainBundle];
        NSString* main_id = main ? [main bundleIdentifier] : nil;
        const bool main_is_extension =
            main_id != nil && [main_id hasSuffix:@".appex"];

        NSProcessInfo* info = [NSProcessInfo processInfo];
        NSString* process_name = info ? [info processName] : nil;
        std::string process_name_str = nsstring_to_std(process_name);

        // (3) Recognise Apple's AUHostingService wrapper name. The
        // suffix is informative on macOS Logic 10.5+ (e.g.
        // `AUHostingServiceXPC_arrow` → Logic). Bare `AUHostingService`
        // is the generic wrapper used by many hosts — the classifier in
        // `host_type.cpp` returns Unknown for that so the caller falls
        // back appropriately.
        if (!process_name_str.empty() &&
            process_name_str.rfind("AUHostingService", 0) == 0) {
            return process_name_str;
        }

        // When the main bundle is not the extension, use it as the host
        // identifier. This is the in-process load path (auval, the Pulp
        // standalone host loading its own AU).
        if (!main_is_extension) {
            std::string id = nsstring_to_std(main_id);
            if (!id.empty()) return id;
        }

        // No wrapper context detected — caller falls back to the
        // executable-path heuristic.
        return {};
    }
}

}  // namespace pulp::format
