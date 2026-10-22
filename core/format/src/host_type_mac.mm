// Apple-only `current_auv3_wrapper_identifier` impl.
//
// AU v3 plug-ins live in a sandboxed `.appex`. The bundle's own
// executable name (`PulpAUv3Extension`, `*.AppExtension`, etc.) does
// NOT classify the host — the wrapping host's bundle id has to come
// from Apple's host-service framework instead.
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
//   3. The process name (`getprogname()`) when it carries a
//      recognisable Apple wrapper name (`AUHostingServiceXPC_*`,
//      `AUHostingService`). The classifier in `host_type.cpp` then
//      decides whether the name resolves to a known host.
//
// This runs on every plug-in instantiation, so it reads the main bundle
// through CoreFoundation and the process name through libc: NSBundle and
// NSProcessInfo cost a few hundred microseconds in a host (a scanner, a
// validator) that has not created them yet.
//
// Returns an empty string when no wrapper context is detected — the
// caller falls back to the executable-path heuristic.

#include <pulp/format/host_type.hpp>

#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>
#include <string>

namespace pulp::format {

static std::string cfstring_to_std(CFStringRef s) {
    if (s == nullptr)
        return {};
    if (const char* fast = CFStringGetCStringPtr(s, kCFStringEncodingUTF8))
        return fast;
    const CFIndex max =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8))
        return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

std::string current_auv3_wrapper_identifier() {
    {
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
        //
        // Detect the extension case by inspecting the main bundle PATH
        // (which is the on-disk bundle, e.g. `…/MyPlugin.appex`), not
        // the bundle IDENTIFIER (which is a reverse-DNS string like
        // `com.vendor.pluginAUv3` and never carries a `.appex` suffix).
        // The previous identifier-suffix check was always false in real
        // AUv3 extension processes, so this function leaked the
        // extension's own bundle id as if it were the host id, breaking
        // downstream host classification.
        CFBundleRef main = CFBundleGetMainBundle();
        const std::string main_id =
            main ? cfstring_to_std(CFBundleGetIdentifier(main)) : std::string{};
        bool main_is_extension = false;
        if (main) {
            if (CFURLRef url = CFBundleCopyBundleURL(main)) {
                if (CFStringRef extension = CFURLCopyPathExtension(url)) {
                    main_is_extension =
                        CFStringCompare(extension, CFSTR("appex"), 0) == kCFCompareEqualTo;
                    CFRelease(extension);
                }
                CFRelease(url);
            }
        }

        // getprogname() rather than -[NSProcessInfo processName]: the same
        // executable name, without creating NSProcessInfo.
        const char* program = getprogname();
        std::string process_name_str = program ? program : "";

        // (3) Recognize Apple's AUHostingService wrapper name. The
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
        if (!main_is_extension && !main_id.empty())
            return main_id;

        // No wrapper context detected — caller falls back to the
        // executable-path heuristic.
        return {};
    }
}

}  // namespace pulp::format
