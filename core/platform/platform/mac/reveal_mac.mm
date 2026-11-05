// macOS reveal-in-Finder via NSWorkspace.

#include <pulp/platform/reveal_in_file_manager.hpp>

#include <TargetConditionals.h>

#if TARGET_OS_OSX

#import <AppKit/AppKit.h>

namespace pulp::platform {

RevealOutcome reveal_in_file_manager(const std::filesystem::path& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
        return RevealOutcome::not_found;

    @autoreleasepool {
        NSString* native = [NSString stringWithUTF8String:path.c_str()];
        if (!native)
            return RevealOutcome::failed;
        NSURL* url = [NSURL fileURLWithPath:native];
        if (!url)
            return RevealOutcome::failed;
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
        return RevealOutcome::revealed;
    }
}

} // namespace pulp::platform

#endif
