// Apple-only `detect_host_version` impl.
//
// Reads the host process's main bundle `CFBundleShortVersionString`,
// parses it via `parse_host_version`, and returns the result. Falls
// back to an empty `HostVersion` if the bundle isn't readable or has
// no version string — adapters treat that as "version-gated quirks off".

#include <pulp/format/host_version.hpp>

#import <Foundation/Foundation.h>

namespace pulp::format {

HostVersion detect_host_version(HostType /*type*/) {
    @autoreleasepool {
        NSBundle* bundle = [NSBundle mainBundle];
        if (bundle == nil) return {};
        id raw = [bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        if (![raw isKindOfClass:[NSString class]]) return {};
        NSString* version = (NSString*) raw;
        const char* utf8 = [version UTF8String];
        if (utf8 == nullptr) return {};
        const auto parsed = parse_host_version(std::string_view(utf8));
        return parsed.value_or(HostVersion{});
    }
}

}  // namespace pulp::format
